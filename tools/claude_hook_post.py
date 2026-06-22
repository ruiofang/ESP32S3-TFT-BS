#!/usr/bin/env python3
"""
Tiny hook helper for Claude Code -> claude_status_ble_bridge.py.

Reads the Claude Code hook payload from stdin, maps it to the bridge's
JSON shape, and posts a single line to the bridge daemon over TCP.

Wire into ~/.claude/settings.json:

    {
      "hooks": {
        "UserPromptSubmit": [{"hooks":[{"type":"command",
            "command":"/abs/path/to/claude_hook_post.py user_prompt"}]}],
        "PreToolUse":       [{"hooks":[{"type":"command",
            "command":"/abs/path/to/claude_hook_post.py pre_tool"}]}],
        "PostToolUse":      [{"hooks":[{"type":"command",
            "command":"/abs/path/to/claude_hook_post.py post_tool"}]}],
        "Stop":             [{"hooks":[{"type":"command",
            "command":"/abs/path/to/claude_hook_post.py stop"}]}]
      }
    }

The hook event name is the first CLI arg. Stdin is Claude Code's hook JSON.
"""

from __future__ import annotations

import json
import os
import socket
import sys

HOST = os.environ.get("CLAUDE_BRIDGE_HOST", "127.0.0.1")
PORT = int(os.environ.get("CLAUDE_BRIDGE_PORT", "8765"))


def clip_utf8(value: object, byte_limit: int) -> str:
    text = str(value or "")
    data = text.encode("utf-8")[:byte_limit]
    return data.decode("utf-8", errors="ignore")


def first_line(value: object, byte_limit: int = 63) -> str:
    return clip_utf8((str(value or "").strip().splitlines() or [""])[0], byte_limit)


def nested_get(obj: dict, *keys: str) -> object:
    cur: object = obj
    for key in keys:
        if not isinstance(cur, dict):
            return None
        cur = cur.get(key)
    return cur


def has_truthy_error_flag(hook: dict) -> bool:
    for key in ("is_error", "error", "failed", "success"):
        if key in hook:
            value = hook.get(key)
            if key == "success":
                return value is False or str(value).lower() == "false"
            return bool(value) and str(value).lower() not in ("false", "0", "none", "")
    response = hook.get("tool_response") or hook.get("response") or hook.get("result")
    return isinstance(response, dict) and has_truthy_error_flag(response)


def classify_message_state(message: object, default: str) -> str:
    text = str(message or "").lower()
    if any(word in text for word in ("未登录", "请登录", "登录", "login", "sign in", "auth", "unauthorized")):
        return "waiting"
    if any(word in text for word in ("失败", "错误", "报错", "error", "failed", "failure", "exception", "traceback", "denied")):
        return "error"
    return default


def main() -> int:
    event = (sys.argv[1] if len(sys.argv) > 1 else "").strip().lower()
    try:
        hook = json.load(sys.stdin)
        if not isinstance(hook, dict):
            hook = {}
    except (ValueError, json.JSONDecodeError):
        hook = {}

    patch: dict = {}

    if event == "user_prompt" or event == "userpromptsubmit":
        patch["state"] = "thinking"
        prompt = hook.get("prompt") or hook.get("user_prompt") or ""
        patch["msg"] = first_line(prompt)
        patch["tool"] = ""

    elif event == "pre_tool" or event == "pretooluse":
        tool = hook.get("tool_name") or hook.get("tool") or ""
        tool_input = hook.get("tool_input") or {}
        patch["state"] = "writing" if str(tool) in ("Edit", "Write", "NotebookEdit") else "tool"
        patch["tool"] = clip_utf8(tool, 23)
        # Try to surface the first interesting field of tool_input
        msg_bits = []
        for key in ("file_path", "path", "command", "url", "pattern"):
            v = tool_input.get(key) if isinstance(tool_input, dict) else None
            if v:
                msg_bits.append(f"{key}={v}")
                break
        patch["msg"] = clip_utf8(" | ".join(msg_bits) if msg_bits else f"running {tool}", 63)

    elif event == "post_tool" or event == "posttooluse":
        tool = hook.get("tool_name") or hook.get("tool") or ""
        response = hook.get("tool_response") or hook.get("response") or hook.get("result") or {}
        message = (
            hook.get("message") or hook.get("error") or hook.get("stderr") or
            nested_get(response, "message") or nested_get(response, "error") or
            nested_get(response, "stderr") or ""
        )
        patch["state"] = "error" if has_truthy_error_flag(hook) else classify_message_state(message, "thinking")
        patch["tool"] = ""
        if patch["state"] == "error" and message:
            patch["msg"] = first_line(message)
        else:
            patch["msg"] = clip_utf8(f"{tool} done" if tool else "tool done", 63)

    elif event == "stop":
        patch["state"] = "done"
        patch["tool"] = ""
        patch["msg"] = "Claude finished turn"

    elif event == "notification":
        msg = hook.get("message") or hook.get("notification") or ""
        patch["state"] = classify_message_state(msg, "waiting")
        patch["msg"] = first_line(msg)

    elif event in ("error", "subagentstop", "subagent_stop"):
        msg = hook.get("message", "")
        patch["state"] = "error" if event == "error" else classify_message_state(msg, "thinking")
        patch["msg"] = first_line(msg)

    else:
        # Unknown event: forward as-is if the JSON already looks like a status patch
        if isinstance(hook, dict) and "state" in hook:
            patch = hook
        else:
            return 0  # nothing to do

    # Optional: read token totals from env if your wrapper exports them
    for env_key, json_key in (("CLAUDE_TOKENS_IN", "ti"), ("CLAUDE_TOKENS_OUT", "to"),
                                ("CLAUDE_MODEL", "model")):
        v = os.environ.get(env_key)
        if v:
            if json_key in ("ti", "to"):
                try:
                    patch[json_key] = int(v)
                except ValueError:
                    pass
            else:
                patch[json_key] = clip_utf8(v, 23)

    line = (json.dumps(patch, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
    try:
        with socket.create_connection((HOST, PORT), timeout=1.5) as s:
            s.sendall(line)
            s.settimeout(0.5)
            try:
                s.recv(16)
            except (OSError, socket.timeout):
                pass
    except OSError as exc:
        # Don't block Claude Code if the bridge isn't running.
        sys.stderr.write(f"[claude_hook_post] bridge unreachable: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
