#!/usr/bin/env python3
"""Install global Claude Code hooks for the ESP32 status bridge.

Run from this repository on each computer:
    python3 tools/install_claude_status_hooks.py

The installer keeps ~/.claude/settings.json as the single Claude Code hook
location, but points every hook at ~/.local/bin/esp32-claude-hook. The wrapper
then locates this repository by environment variable, current working directory,
or common fallback paths.
"""

from __future__ import annotations

import json
import os
import stat
from pathlib import Path
from typing import Any

HOOK_EVENTS = {
    "UserPromptSubmit": "user_prompt",
    "PreToolUse": "pre_tool",
    "PostToolUse": "post_tool",
    "Notification": "notification",
    "Stop": "stop",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def wrapper_text(root: Path) -> str:
    repo_path = str(root)
    return f'''#!/usr/bin/env bash
set -euo pipefail

repo_from_pwd() {{
  local dir="$PWD"
  while [[ "$dir" != "/" ]]; do
    if [[ -x "$dir/tools/claude_hook_post.py" ]]; then
      printf '%s\\n' "$dir/tools/claude_hook_post.py"
      return 0
    fi
    dir="$(dirname "$dir")"
  done
  return 1
}}

candidates=()
if [[ -n "${{CLAUDE_ESP32_HOOK:-}}" ]]; then
  candidates+=("$CLAUDE_ESP32_HOOK")
fi
if [[ -n "${{CLAUDE_ESP32_REPO:-}}" ]]; then
  candidates+=("$CLAUDE_ESP32_REPO/tools/claude_hook_post.py")
fi
if hook_from_pwd="$(repo_from_pwd 2>/dev/null)"; then
  candidates+=("$hook_from_pwd")
fi
candidates+=(
  "{repo_path}/tools/claude_hook_post.py"
  "$HOME/ESP32S3-TFT-BS/tools/claude_hook_post.py"
)

for hook in "${{candidates[@]}}"; do
  if [[ -x "$hook" ]]; then
    exec "$hook" "$@"
  fi
done

echo "[esp32-claude-hook] claude_hook_post.py not found; set CLAUDE_ESP32_REPO=/path/to/ESP32S3-TFT-BS" >&2
exit 0
'''


def load_settings(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Invalid JSON in {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise SystemExit(f"{path} must contain a JSON object")
    return data


def main() -> int:
    root = repo_root()
    home = Path.home()
    wrapper = home / ".local" / "bin" / "esp32-claude-hook"
    settings_path = home / ".claude" / "settings.json"

    wrapper.parent.mkdir(parents=True, exist_ok=True)
    wrapper.write_text(wrapper_text(root), encoding="utf-8")
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    settings_path.parent.mkdir(parents=True, exist_ok=True)
    settings = load_settings(settings_path)
    hooks = settings.setdefault("hooks", {})
    if not isinstance(hooks, dict):
        raise SystemExit(f"{settings_path}: hooks must be a JSON object")

    for event, arg in HOOK_EVENTS.items():
        hooks[event] = [
            {
                "hooks": [
                    {
                        "type": "command",
                        "command": f"{wrapper} {arg}",
                    }
                ]
            }
        ]

    settings_path.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"Installed wrapper: {wrapper}")
    print(f"Updated settings:  {settings_path}")
    print("Start the bridge separately: python3 tools/claude_status_ble_bridge.py --listen-port 8765 --connect-on-start")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
