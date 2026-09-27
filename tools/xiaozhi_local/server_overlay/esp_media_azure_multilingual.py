"""Azure Speech REST TTS provider with per-segment language routing."""

import os
import re
from html import escape

import httpx

from core.providers.tts.base import TTSProviderBase


_CHINESE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")
_CYRILLIC = re.compile(r"[\u0400-\u052f]")
_REGION = re.compile(r"^[a-z0-9-]+$")


class TTSProvider(TTSProviderBase):
    """Use configured Azure neural voices without a fixed-language fallback."""

    def __init__(self, config, delete_audio_file):
        super().__init__(config, delete_audio_file)
        self.api_key = config.get("api_key")
        self.region = config.get("region")
        self.voices = config.get("voices") or {}
        self.fallback_locale = config.get("fallback_locale", "en-US")
        self.output_format = config.get(
            "output_format", "audio-24khz-48kbitrate-mono-mp3"
        )
        if not isinstance(self.api_key, str) or not self.api_key:
            raise ValueError("Azure Speech API key is not configured")
        if not isinstance(self.region, str) or not _REGION.fullmatch(self.region):
            raise ValueError("Azure Speech region is invalid")
        if self.fallback_locale not in self.voices:
            raise ValueError("Azure Speech fallback locale has no configured voice")
        for locale in ("en-US", "zh-CN", "ru-RU"):
            if not isinstance(self.voices.get(locale), str) or not self.voices[locale]:
                raise ValueError(f"Azure Speech voice is missing for {locale}")
        self.endpoint = (
            f"https://{self.region}.tts.speech.microsoft.com/cognitiveservices/v1"
        )
        self.audio_file_type = "mp3"
        self._last_locale = self.fallback_locale

    def _locale_for(self, text):
        chinese = len(_CHINESE.findall(text))
        cyrillic = len(_CYRILLIC.findall(text))
        if chinese > cyrillic:
            locale = "zh-CN"
        elif cyrillic > chinese:
            locale = "ru-RU"
        elif chinese or cyrillic:
            locale = self._last_locale
        else:
            locale = self._last_locale or self.fallback_locale
        if locale not in self.voices:
            locale = self.fallback_locale
        self._last_locale = locale
        return locale

    def _ssml(self, text, locale):
        voice = self.voices[locale]
        return (
            f'<speak version="1.0" xml:lang="{escape(locale, quote=True)}">'
            f'<voice name="{escape(voice, quote=True)}">{escape(text)}</voice>'
            "</speak>"
        )

    async def text_to_speak(self, text, output_file):
        locale = self._locale_for(text)
        headers = {
            "Ocp-Apim-Subscription-Key": self.api_key,
            "Content-Type": "application/ssml+xml",
            "X-Microsoft-OutputFormat": self.output_format,
            "User-Agent": "esp-media-xiaozhi",
        }
        try:
            async with httpx.AsyncClient(timeout=self.tts_timeout) as client:
                response = await client.post(
                    self.endpoint,
                    headers=headers,
                    content=self._ssml(text, locale).encode("utf-8"),
                )
        except httpx.HTTPError as error:
            raise RuntimeError(f"Azure Speech TTS transport failure: {type(error).__name__}")
        if response.status_code != 200:
            raise RuntimeError(f"Azure Speech TTS request failed: HTTP {response.status_code}")
        audio = response.content
        if not audio:
            raise RuntimeError("Azure Speech TTS returned no audio")
        if output_file:
            os.makedirs(os.path.dirname(output_file), exist_ok=True)
            with open(output_file, "wb") as stream:
                stream.write(audio)
            return None
        return audio
