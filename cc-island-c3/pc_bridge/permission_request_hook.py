"""Mirror Codex permission-hook lifecycle events to the local bridge state file."""

import argparse
import json
import sys
from pathlib import Path

from serve_usage import DEFAULT_PERMISSION_STATE_PATH, apply_permission_hook_event


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--state-path",
        type=Path,
        default=DEFAULT_PERMISSION_STATE_PATH,
        help="Path shared with serve_usage.py for the pending-permission state",
    )
    args = parser.parse_args()

    try:
        event = json.load(sys.stdin)
        apply_permission_hook_event(event, args.state_path)
    except Exception:
        # A telemetry write failure must not approve, reject, or block the normal prompt.
        return 0
    if event.get("hook_event_name") == "Stop":
        # Stop hooks require JSON output; an empty object has no control effect.
        print("{}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
