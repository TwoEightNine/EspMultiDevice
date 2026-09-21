#!/usr/bin/env ruby
require "json"
require "uri"
require "yaml"

def fail_check(message)
  warn "FAIL #{message}"
  exit 1
end

def fetch_path(root, *path)
  path.reduce(root) { |value, key| value.is_a?(Hash) ? value[key] : nil }
end

mode = ARGV.fetch(0, "config")
path = ARGV.fetch(1)

if mode == "bootstrap"
  response = JSON.parse(File.read(path))
  websocket = response["websocket"]
  fail_check("bootstrap response has no websocket object") unless websocket.is_a?(Hash)
  url = websocket["url"]
  token = websocket["token"]
  fail_check("bootstrap did not advertise WSS") unless url.is_a?(String) && url.start_with?("wss://")
  fail_check("bootstrap returned no credential") unless token.is_a?(String) && !token.empty?
  fail_check("bootstrap returned placeholder credential") if token.match?(/placeholder|test.token|replace/i)
  puts "PASS bootstrap response shape and signed WSS credential"
  exit 0
end

config = YAML.safe_load(File.read(path), aliases: true)
fail_check("configuration root is malformed") unless config.is_a?(Hash)

required_modules = {
  "VAD" => "SileroVAD",
  "ASR" => "OpenaiASR",
  "LLM" => "OpenAILLM",
  "TTS" => "EdgeTTS",
  "Intent" => "nointent",
  "Memory" => "mem_local_short"
}
required_modules.each do |kind, expected|
  fail_check("selected_module.#{kind} must be #{expected}") unless
    fetch_path(config, "selected_module", kind) == expected
end

websocket = fetch_path(config, "server", "websocket")
begin
  uri = URI.parse(websocket.to_s)
rescue URI::InvalidURIError
  fail_check("advertised websocket URL is malformed")
end
fail_check("advertised websocket must use WSS") unless uri.scheme == "wss"
fail_check("advertised websocket must use a DNS hostname") unless
  uri.host&.match?(/[a-z]/i) && !uri.host.match?(/\A\d+(?:\.\d+){3}\z/)
fail_check("advertised websocket must not contain userinfo or a fragment") if
  uri.userinfo || uri.fragment
fail_check("advertised websocket port is invalid") unless uri.port.between?(1, 65_535)
fail_check("advertised websocket path is invalid") unless uri.path.start_with?("/")
fail_check("backend authentication must be enabled") unless
  fetch_path(config, "server", "auth", "enabled") == true
fail_check("WebSocket ping handling must be enabled") unless
  config["enable_websocket_ping"] == true
fail_check("MQTT advertisement must remain disabled") unless
  fetch_path(config, "server", "mqtt_gateway").nil? &&
  fetch_path(config, "server", "mqtt_signature_key").nil?

asr = fetch_path(config, "ASR", "OpenaiASR")
llm = fetch_path(config, "LLM", "OpenAILLM")
fail_check("OpenaiASR provider is incomplete") unless
  asr.is_a?(Hash) && asr["type"] == "openai" &&
  asr["base_url"] == "https://api.openai.com/v1/audio/transcriptions" &&
  asr["model_name"] == "gpt-4o-mini-transcribe-2025-12-15"
fail_check("OpenAILLM provider is incomplete") unless
  llm.is_a?(Hash) && llm["type"] == "openai" &&
  llm["base_url"] == "https://api.openai.com/v1" &&
  llm["model_name"] == "gpt-5.4-mini-2026-03-17"
[asr["api_key"], llm["api_key"]].each do |key|
  fail_check("OpenAI API key is missing or still a placeholder") unless
    key.is_a?(String) && !key.empty? && !key.match?(/replace|placeholder|your|你的/i)
end
fail_check("Simplified-Chinese EdgeTTS voice is not configured") unless
  fetch_path(config, "TTS", "EdgeTTS", "type") == "edge" &&
  fetch_path(config, "TTS", "EdgeTTS", "voice") == "zh-CN-XiaoxiaoNeural" &&
  fetch_path(config, "TTS", "EdgeTTS", "language") == "中文"
fail_check("Silero VAD is not configured") unless
  fetch_path(config, "VAD", "SileroVAD", "type") == "silero"
fail_check("nointent is not configured") unless
  fetch_path(config, "Intent", "nointent", "type") == "nointent"
fail_check("mem_local_short must use OpenAILLM") unless
  fetch_path(config, "Memory", "mem_local_short", "type") == "mem_local_short" &&
  fetch_path(config, "Memory", "mem_local_short", "llm") == "OpenAILLM"
prompt = config["prompt"].to_s
fail_check("language-matching bilingual prompt is missing") unless
  prompt.match?(/Simplified Chinese/i) && prompt.match?(/same language/i)

puts "PASS private backend configuration"
