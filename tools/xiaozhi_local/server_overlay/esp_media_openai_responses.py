"""OpenAI Responses API adapter for ESP Media's Xiaozhi overlay.

The pinned upstream server expects the Chat Completions streaming/tool-call
shape.  This provider owns the translation so upstream code and device RPCs
remain unchanged while every LLM request uses the Responses API.
"""

import re
from types import SimpleNamespace

import httpx
import openai

from core.providers.llm.base import LLMProviderBase
from core.utils.util import check_model_key


WEB_SEARCH_INSTRUCTIONS = (
    "\n\nYou can search the web for current facts such as weather, news, "
    "prices, exchange rates, schedules, and recent events. Use it whenever an "
    "answer depends on information newer than your training or on today's "
    "conditions; use the configured location when the user names none. Your "
    "answer is spoken aloud: never include links, URLs, citations, markdown, "
    "headings, or bullet lists. Name a source only when it matters. The device "
    "search_web tool only opens results on screen, so use it only when the "
    "user asks to see or open results."
)

_LOCATION_FIELDS = ("country", "region", "city", "timezone")


def web_search_tool(config):
    """Build the hosted web_search tool from the LLM config, or None."""
    settings = config.get("web_search")
    if settings is None or settings is False:
        return None
    if settings is True:
        settings = {}
    if not isinstance(settings, dict):
        raise ValueError("web_search must be a mapping")
    if settings.get("enabled", True) is False:
        return None
    tool = {"type": "web_search"}
    location = {
        field: str(settings[field]) for field in _LOCATION_FIELDS
        if settings.get(field)
    }
    if location:
        country = location.get("country", "")
        if country and not re.fullmatch(r"[A-Z]{2}", country):
            raise ValueError("web_search.country must be an ISO 3166-1 alpha-2 code")
        tool["user_location"] = {"type": "approximate", **location}
    return tool


class SpokenTextFilter:
    """Streaming filter that keeps links and citations out of spoken text.

    Web-search answers carry inline citations such as
    ``([example.com](https://example.com/...))``.  The TTS sentence splitter
    would cut those at their dots and speak them, and the device would show
    them as captions.  Text is held back only while a bracket or URL that may
    still become a link is open, and never longer than ``MAX_HOLD`` chars.
    """

    MAX_HOLD = 400
    _CITATION = re.compile(r"[ \t]*\(\s*(?:\[[^\]\n]*\]\([^)\s]*\)[\s,;]*)+\)")
    _LINK = re.compile(r"\[([^\]\n]*)\]\((?:https?://|www\.)[^)\s]*\)")
    _URL = re.compile(r"[ \t]*\(?(?:https?://|www\.)[^\s)]+\)?")
    # A URL that still runs to the end of the buffer may continue.
    _OPEN_URL = re.compile(r"(?:https?://|www\.)[^\s)]*\Z")
    # The final word, after whitespace or an opening bracket.
    _TAIL = re.compile(r"[^\s(\[]*\Z")

    def __init__(self):
        self._pending = ""

    @classmethod
    def _clean(cls, text):
        text = cls._CITATION.sub("", text)
        text = cls._LINK.sub(r"\1", text)
        return cls._URL.sub("", text)

    @classmethod
    def _hold_index(cls, text):
        """Return where an unfinished link, citation, or URL may begin."""
        stack = []
        last_closed_bracket = None
        for index, char in enumerate(text):
            if char in "([":
                if char == "(" and last_closed_bracket is not None and \
                        last_closed_bracket[1] == index - 1:
                    stack.append(last_closed_bracket[0])
                else:
                    stack.append(index)
                last_closed_bracket = None
            elif char in ")]":
                opened = stack.pop() if stack else None
                last_closed_bracket = (opened, index) \
                    if char == "]" and opened is not None else None
        hold = len(text)
        if stack:
            hold = stack[0]
        if last_closed_bracket is not None and last_closed_bracket[1] == len(text) - 1:
            hold = min(hold, last_closed_bracket[0])
        url = cls._OPEN_URL.search(text)
        if url:
            hold = min(hold, url.start())
        tail = cls._TAIL.search(text).group(0).lower()
        if tail and any(scheme.startswith(tail) for scheme in ("https://", "http://", "www.")):
            hold = min(hold, len(text) - len(tail))
        # Hold trailing spaces, and the spaces before a held span, so a
        # citation or URL removed later cannot leave a stray space before
        # the next punctuation.
        while hold > 0 and text[hold - 1] in " \t":
            hold -= 1
        return hold

    def feed(self, delta):
        if not delta:
            return ""
        # Decide what to hold on the raw text: cleaning a partial link could
        # strip half of it and let the rest leak through later.
        text = self._pending + delta
        hold = self._hold_index(text)
        if len(text) - hold > self.MAX_HOLD:
            hold = len(text)
        self._pending = text[hold:]
        return self._clean(text[:hold])

    def flush(self):
        text = self._clean(self._pending)
        self._pending = ""
        return text


class LLMProvider(LLMProviderBase):
    """GPT-5.6 Luna provider with Xiaozhi-compatible streaming results."""

    def __init__(self, config):
        self.model_name = config.get("model_name")
        self.api_key = config.get("api_key")
        self.base_url = config.get("base_url", config.get("url"))
        self.reasoning_effort = config.get("reasoning_effort", "medium")
        if self.reasoning_effort not in {
            "none", "low", "medium", "high", "xhigh", "max"
        }:
            raise ValueError("reasoning_effort is invalid")
        self.web_search = web_search_tool(config)

        timeout_value = config.get("timeout", 300)
        if isinstance(timeout_value, dict):
            timeout = httpx.Timeout(
                pool=timeout_value.get("pool", 2.0),
                connect=timeout_value.get("connect", 3.0),
                write=timeout_value.get("write", 5.0),
                read=timeout_value.get("read", 300.0),
            )
        elif isinstance(timeout_value, (int, float)) and timeout_value > 0:
            timeout = httpx.Timeout(timeout_value)
        else:
            timeout = httpx.Timeout(300)

        key_problem = check_model_key("LLM", self.api_key)
        if key_problem:
            raise ValueError("OpenAI API key is not configured")
        self.client = openai.OpenAI(
            api_key=self.api_key, base_url=self.base_url, timeout=timeout
        )

    @staticmethod
    def _content(value):
        return "" if value is None else str(value)

    @classmethod
    def _to_responses_input(cls, dialogue):
        """Split the system instructions and normalize stored tool history."""
        instructions = ""
        items = []
        for message in dialogue:
            role = message.get("role")
            if role == "system":
                instructions = cls._content(message.get("content"))
                continue
            if role == "assistant" and message.get("tool_calls"):
                for call in message["tool_calls"]:
                    function = call.get("function", {})
                    items.append({
                        "type": "function_call",
                        "call_id": cls._content(call.get("id")),
                        "name": cls._content(function.get("name")),
                        "arguments": cls._content(function.get("arguments")),
                    })
                continue
            if role == "tool":
                items.append({
                    "type": "function_call_output",
                    "call_id": cls._content(message.get("tool_call_id")),
                    "output": cls._content(message.get("content")),
                })
                continue
            if role in {"user", "assistant"}:
                items.append({"role": role, "content": cls._content(message.get("content"))})
        return instructions, items

    @staticmethod
    def _to_responses_tools(functions):
        if not functions:
            return None
        tools = []
        for tool in functions:
            function = tool.get("function", tool)
            if tool.get("type") not in (None, "function"):
                continue
            name = function.get("name")
            if not name:
                continue
            tools.append({
                "type": "function",
                "name": name,
                "description": function.get("description", ""),
                "parameters": function.get("parameters", {"type": "object", "properties": {}}),
            })
        return tools or None

    @staticmethod
    def _tool_delta(index, call_id, name, arguments):
        return SimpleNamespace(
            index=index,
            id=call_id,
            function=SimpleNamespace(name=name, arguments=arguments),
        )

    def _stream(self, dialogue, functions=None, max_tokens=None):
        instructions, input_items = self._to_responses_input(dialogue)
        request = {
            "model": self.model_name,
            "instructions": instructions or None,
            "input": input_items,
            "stream": True,
            "reasoning": {"effort": self.reasoning_effort},
        }
        if max_tokens is not None:
            request["max_output_tokens"] = max_tokens
        tools = self._to_responses_tools(functions)
        web_search = getattr(self, "web_search", None)
        if tools and web_search:
            # Hosted search runs inside this response; only conversational
            # turns that already offer tools get it, never memory summaries.
            tools.append(dict(web_search))
            request["instructions"] = (instructions or "") + WEB_SEARCH_INSTRUCTIONS
        if tools:
            request["tools"] = tools

        calls = {}
        next_index = 0
        spoken = SpokenTextFilter()
        stream = self.client.responses.create(**request)
        try:
            for event in stream:
                event_type = getattr(event, "type", "")
                if event_type == "response.output_text.delta":
                    text = spoken.feed(getattr(event, "delta", ""))
                    if text:
                        yield text, None
                    continue

                if event_type == "response.output_text.done":
                    text = spoken.flush()
                    if text:
                        yield text, None
                    continue

                if event_type == "response.output_item.added":
                    item = getattr(event, "item", None)
                    if getattr(item, "type", None) != "function_call":
                        continue
                    item_id = getattr(event, "item_id", None) or getattr(item, "id", None)
                    call_id = getattr(item, "call_id", None) or item_id
                    state = calls.get(item_id)
                    if state is None:
                        state = {
                            "index": next_index,
                            "id": call_id,
                            "name": getattr(item, "name", ""),
                            "sent": 0,
                        }
                        calls[item_id] = state
                        next_index += 1
                    else:
                        state["id"] = call_id or state["id"]
                        state["name"] = getattr(item, "name", "") or state["name"]
                    yield "", [self._tool_delta(
                        state["index"], state["id"], state["name"], ""
                    )]
                    continue

                if event_type == "response.function_call_arguments.delta":
                    item_id = getattr(event, "item_id", None)
                    state = calls.get(item_id)
                    if state is None:
                        state = {
                            "index": next_index,
                            "id": item_id,
                            "name": "",
                            "sent": 0,
                        }
                        calls[item_id] = state
                        next_index += 1
                    delta = getattr(event, "delta", "")
                    state["sent"] += len(delta)
                    yield "", [self._tool_delta(
                        state["index"], state["id"], state["name"], delta
                    )]
                    continue

                if event_type == "response.output_item.done":
                    item = getattr(event, "item", None)
                    if getattr(item, "type", None) != "function_call":
                        continue
                    item_id = getattr(event, "item_id", None) or getattr(item, "id", None)
                    state = calls.get(item_id)
                    if state is None:
                        state = {"index": next_index, "id": None, "name": "", "sent": 0}
                        calls[item_id] = state
                        next_index += 1
                    call_id = getattr(item, "call_id", None) or state["id"] or item_id
                    name = getattr(item, "name", None) or state["name"]
                    arguments = self._content(getattr(item, "arguments", ""))
                    remaining = arguments[state["sent"]:]
                    if remaining or not state["sent"]:
                        yield "", [self._tool_delta(
                            state["index"], call_id, name, remaining
                        )]
            text = spoken.flush()
            if text:
                yield text, None
        finally:
            close = getattr(stream, "close", None)
            if callable(close):
                close()

    def response(self, session_id, dialogue, **kwargs):
        del session_id
        for content, _ in self._stream(
            dialogue, max_tokens=kwargs.get("max_tokens")
        ):
            if content:
                yield content

    def response_with_functions(self, session_id, dialogue, functions=None, **kwargs):
        del session_id
        yield from self._stream(
            dialogue, functions=functions, max_tokens=kwargs.get("max_tokens")
        )
