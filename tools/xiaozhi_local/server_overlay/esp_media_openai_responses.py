"""OpenAI Responses API adapter for ESP Media's Xiaozhi overlay.

The pinned upstream server expects the Chat Completions streaming/tool-call
shape.  This provider owns the translation so upstream code and device RPCs
remain unchanged while every LLM request uses the Responses API.
"""

from types import SimpleNamespace

import httpx
import openai

from core.providers.llm.base import LLMProviderBase
from core.utils.util import check_model_key


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
        if tools:
            request["tools"] = tools

        calls = {}
        next_index = 0
        stream = self.client.responses.create(**request)
        try:
            for event in stream:
                event_type = getattr(event, "type", "")
                if event_type == "response.output_text.delta":
                    yield getattr(event, "delta", ""), None
                    continue

                if event_type == "response.output_item.added":
                    item = getattr(event, "item", None)
                    if getattr(item, "type", None) != "function_call":
                        continue
                    item_id = getattr(event, "item_id", None) or getattr(item, "id", None)
                    call_id = getattr(item, "call_id", None) or item_id
                    index = getattr(event, "output_index", next_index)
                    next_index = max(next_index, index + 1)
                    calls[item_id] = {
                        "index": index,
                        "id": call_id,
                        "name": getattr(item, "name", ""),
                        "sent": 0,
                    }
                    yield "", [self._tool_delta(
                        index, call_id, getattr(item, "name", ""), ""
                    )]
                    continue

                if event_type == "response.function_call_arguments.delta":
                    item_id = getattr(event, "item_id", None)
                    state = calls.get(item_id)
                    if state is None:
                        index = getattr(event, "output_index", next_index)
                        next_index = max(next_index, index + 1)
                        state = {
                            "index": index,
                            "id": item_id,
                            "name": "",
                            "sent": 0,
                        }
                        calls[item_id] = state
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
                        index = getattr(event, "output_index", next_index)
                        next_index = max(next_index, index + 1)
                        state = {"index": index, "id": None, "name": "", "sent": 0}
                    call_id = getattr(item, "call_id", None) or state["id"] or item_id
                    name = getattr(item, "name", None) or state["name"]
                    arguments = self._content(getattr(item, "arguments", ""))
                    remaining = arguments[state["sent"]:]
                    if remaining or not state["sent"]:
                        yield "", [self._tool_delta(
                            state["index"], call_id, name, remaining
                        )]
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
