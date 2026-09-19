#!/usr/bin/env bash
set -euo pipefail

tool_dir="$(cd "$(dirname "$0")" && pwd)"
config_file="$tool_dir/.runtime/data/.config.yaml"
endpoint_file="$tool_dir/.runtime/endpoint.env"
ca_file="$tool_dir/.runtime/caddy-data/caddy/pki/authorities/local/root.crt"

ruby "$tool_dir/check_config.rb" config "$config_file"
test -s "$ca_file"
host="$(sed -n 's/^XIAOZHI_LAN_HOST=//p' "$endpoint_file")"
if [[ -z "$host" || "$host" != *.* || "$host" == *:* ]]; then
  echo "FAIL endpoint must be a DNS hostname" >&2
  exit 1
fi

cd "$tool_dir"
wait_for_service() {
  local service="$1"
  local deadline=$((SECONDS + 90))
  local container state health
  while (( SECONDS < deadline )); do
    container="$(docker-compose --env-file .runtime/endpoint.env ps -q "$service")"
    if [[ -z "$container" ]]; then
      echo "FAIL $service container is absent" >&2
      return 1
    fi
    state="$(docker inspect --format '{{.State.Status}}' "$container")"
    health="$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$container")"
    if [[ "$health" == "healthy" || ( "$health" == "none" && "$state" == "running" ) ]]; then
      return 0
    fi
    if [[ "$health" == "unhealthy" || "$state" == "exited" || "$state" == "dead" ]]; then
      echo "FAIL $service container state=$state health=$health" >&2
      return 1
    fi
    sleep 2
  done
  echo "FAIL $service did not become healthy within 90 seconds" >&2
  return 1
}

wait_for_service xiaozhi
wait_for_service caddy
echo "PASS pinned containers are running and healthy"

response_file="$(mktemp -t esp-media-bootstrap.XXXXXX)"
trap 'rm -f "$response_file"' EXIT
curl --silent --show-error --cacert "$ca_file" --output "$response_file" \
  --connect-timeout 15 --max-time 15 \
  -H 'Activation-Version: 1' \
  -H 'Device-Id: 02:00:00:00:00:01' \
  -H 'Client-Id: 00000000-0000-4000-8000-000000000001' \
  -H 'Accept-Language: zh-CN' \
  -H 'Content-Type: application/json' \
  --data '{"version":2,"language":"zh-CN"}' \
  "https://$host:8443/xiaozhi/ota/"
ruby "$tool_dir/check_config.rb" bootstrap "$response_file"
echo "BACKEND_READINESS PASS"
