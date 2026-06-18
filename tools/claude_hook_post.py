#!/usr/bin/env python3
"""
Tiny hook helper for Claude Code -> claude_status_bridge.py.

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
        patch["msg"] = (str(prompt).strip().splitlines() or [""])[0][:63]
        patch["tool"] = ""

    elif event == "pre_tool" or event == "pretooluse":
        tool = hook.get("tool_name") or hook.get("tool") or ""
        tool_input = hook.get("tool_input") or {}
        patch["state"] = "writing" if str(tool) in ("Edit", "Write", "NotebookEdit") else "tool"
        patch["tool"] = str(tool)[:23]
        # Try to surface the first interesting field of tool_input
        msg_bits = []
        for key in ("file_path", "path", "command", "url", "pattern"):
            v = tool_input.get(key) if isinstance(tool_input, dict) else None
            if v:
                msg_bits.append(f"{key}={v}")
                break
        patch["msg"] = " | ".join(msg_bits)[:63] if msg_bits else f"running {tool}"

    elif event == "post_tool" or event == "posttooluse":
        tool = hook.get("tool_name") or hook.get("tool") or ""
        # After a tool finishes Claude is typically back to thinking before next step.
        patch["state"] = "thinking"
        patch["tool"] = ""
        patch["msg"] = f"{tool} done" if tool else "tool done"

    elif event == "stop":
        patch["state"] = "done"
        patch["tool"] = ""
        patch["msg"] = "Claude finished turn"

    elif event == "notification":
        patch["state"] = "waiting"
        msg = hook.get("message") or hook.get("notification") or ""
        patch["msg"] = str(msg)[:63]

    elif event in ("error", "subagentstop", "subagent_stop"):
        patch["state"] = "error" if event == "error" else "thinking"
        patch["msg"] = str(hook.get("message", "")).strip()[:63]

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
                patch[json_key] = v[:23]

    line = (json.dumps(patch, separators=(",", ":")) + "\n").encode("utf-8")
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
