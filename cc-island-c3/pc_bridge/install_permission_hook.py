"""Install or remove the non-decision Codex permission status hook."""

import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

from serve_usage import DEFAULT_PERMISSION_STATE_PATH


HOOK_SCRIPT = Path(__file__).with_name("permission_request_hook.py")
HOOK_EVENTS = (
    "PermissionRequest",
    "PostToolUse",
    "Stop",
    "Interrupt",
    "UserPromptSubmit",
    "SessionStart",
    "SessionEnd",
)


def hook_command(state_path: Path = DEFAULT_PERMISSION_STATE_PATH) -> str:
    parts = [sys.executable, str(HOOK_SCRIPT), "--state-path", str(state_path)]
    if os.name == "nt":
        return subprocess.list2cmdline(parts)
    return shlex.join(parts)


def load_hooks_file(hooks_path: Path) -> dict:
    try:
        config = json.loads(hooks_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {"hooks": {}}
    if not isinstance(config, dict):
        raise ValueError(f"Expected a JSON object in {hooks_path}")
    hooks = config.setdefault("hooks", {})
    if not isinstance(hooks, dict):
        raise ValueError(f"Expected a hooks object in {hooks_path}")
    return config


def save_hooks_file(hooks_path: Path, config: dict) -> None:
    hooks_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=hooks_path.parent,
        prefix=hooks_path.name + ".",
        suffix=".tmp",
        delete=False,
    ) as handle:
        json.dump(config, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
        temp_path = Path(handle.name)
    os.replace(temp_path, hooks_path)


def install_hooks(hooks_path: Path, state_path: Path = DEFAULT_PERMISSION_STATE_PATH) -> int:
    config = load_hooks_file(hooks_path)
    hooks = config["hooks"]
    command = hook_command(state_path)
    added = 0

    for event_name in HOOK_EVENTS:
        groups = hooks.setdefault(event_name, [])
        if not isinstance(groups, list):
            raise ValueError(f"Expected {event_name} hooks to be an array")
        if any(
            isinstance(group, dict)
            and any(
                isinstance(handler, dict)
                and handler.get("type") == "command"
                and handler.get("command") == command
                for handler in group.get("hooks", [])
            )
            for group in groups
        ):
            continue
        groups.append({"hooks": [{"type": "command", "command": command}]})
        added += 1

    if added:
        save_hooks_file(hooks_path, config)
    return added


def remove_hooks(hooks_path: Path, state_path: Path = DEFAULT_PERMISSION_STATE_PATH) -> int:
    config = load_hooks_file(hooks_path)
    hooks = config["hooks"]
    command = hook_command(state_path)
    removed = 0

    for event_name in HOOK_EVENTS:
        groups = hooks.get(event_name)
        if not isinstance(groups, list):
            continue
        remaining_groups = []
        for group in groups:
            if not isinstance(group, dict) or not isinstance(group.get("hooks"), list):
                remaining_groups.append(group)
                continue
            handlers = group["hooks"]
            filtered = [
                handler
                for handler in handlers
                if not (
                    isinstance(handler, dict)
                    and handler.get("type") == "command"
                    and handler.get("command") == command
                )
            ]
            removed += len(handlers) - len(filtered)
            if filtered:
                group["hooks"] = filtered
                remaining_groups.append(group)
        if remaining_groups:
            hooks[event_name] = remaining_groups
        else:
            hooks.pop(event_name, None)

    if removed:
        save_hooks_file(hooks_path, config)
    return removed


def default_hooks_path() -> Path:
    profile = Path(os.environ.get("USERPROFILE", str(Path.home())))
    return profile / ".codex" / "hooks.json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hooks-path", type=Path, default=default_hooks_path())
    parser.add_argument("--state-path", type=Path, default=DEFAULT_PERMISSION_STATE_PATH)
    parser.add_argument("--remove", action="store_true", help="Remove this bridge's hook entries")
    args = parser.parse_args()

    try:
        if args.remove:
            count = remove_hooks(args.hooks_path, args.state_path)
            print(f"Removed {count} Codex Miao hook entries.")
        else:
            count = install_hooks(args.hooks_path, args.state_path)
            print(f"Added {count} Codex Miao hook entries.")
        print(f"Hook configuration: {args.hooks_path}")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Could not update Codex hooks: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
