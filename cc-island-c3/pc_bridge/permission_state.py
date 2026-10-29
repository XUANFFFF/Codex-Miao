"""Correlate observational hooks without making approval decisions."""

import hashlib
import json
import secrets

MAX_EVENT_AGE = 24 * 60 * 60
HOOK_EVENTS = (
    "PreToolUse", "PermissionRequest", "PostToolUse", "Stop", "Interrupt",
    "UserPromptSubmit", "SessionStart", "SessionEnd",
)


def live_items(value, now):
    result = []
    for item in value if isinstance(value, list) else []:
        if not isinstance(item, dict) or not isinstance(item.get("session_id"), str):
            continue
        try:
            age = now - float(item["created_at"])
        except (KeyError, TypeError, ValueError):
            continue
        if 0 <= age <= MAX_EVENT_AGE:
            result.append(item)
    return result


def read_pending(path, now):
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    return live_items(state.get("pending"), now) if isinstance(state, dict) else []


def input_fingerprint(event):
    name, value = event.get("tool_name"), event.get("tool_input")
    if not isinstance(name, str) or value is None:
        return ""
    if name in {"Bash", "apply_patch"} and isinstance(value, dict):
        # The native approval reason is not part of the tool's actual input.
        value = {key: item for key, item in value.items() if key != "description"}
    encoded = json.dumps([name, value], sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def update_permission_state(state, event, now):
    state = state if isinstance(state, dict) else {}
    pending = live_items(state.get("pending"), now)
    tools = live_items(state.get("tools"), now)
    name = event["hook_event_name"]
    scope = {key: str(event.get(key) or "") for key in ("session_id", "turn_id", "agent_id")}
    tool_id = event.get("tool_use_id")
    tool_id = tool_id if isinstance(tool_id, str) else ""

    def same_scope(item):
        return all(str(item.get(key) or "") == value for key, value in scope.items())

    fingerprint = input_fingerprint(event) if name in {"PreToolUse", "PermissionRequest"} else ""
    if name == "PreToolUse" and tool_id:
        tools = [item for item in tools if not (same_scope(item) and item.get("tool_use_id") == tool_id)]
        tools.append({**scope, "tool_use_id": tool_id, "fingerprint": fingerprint, "created_at": now})
    elif name == "PermissionRequest":
        if not tool_id and fingerprint:
            bound = {item.get("tool_use_id") for item in pending if same_scope(item)}
            candidates = [item for item in tools if same_scope(item)
                          and item.get("fingerprint") == fingerprint and item.get("tool_use_id") not in bound]
            if len(candidates) == 1:
                tool_id = candidates[0]["tool_use_id"]
        # Missing/ambiguous identities remain unresolved until an explicit
        # lifecycle boundary. PostToolUse never guesses from just a turn/time.
        duplicate = tool_id and any(same_scope(item) and item.get("tool_use_id") == tool_id for item in pending)
        if not duplicate:
            pending.append({**scope, "request_id": secrets.token_hex(8), "tool_use_id": tool_id,
                            "fingerprint": fingerprint, "created_at": now})
    elif name == "PostToolUse" and tool_id:
        pending = [item for item in pending if not (same_scope(item) and item.get("tool_use_id") == tool_id)]
        tools = [item for item in tools if not (same_scope(item) and item.get("tool_use_id") == tool_id)]
    elif name in {"Stop", "Interrupt", "UserPromptSubmit", "SessionStart", "SessionEnd"}:
        def closed(item):
            if item.get("session_id") != scope["session_id"]:
                return False
            if name == "SessionEnd":
                return True
            if name == "SessionStart":
                return event.get("source") != "compact"
            if str(item.get("agent_id") or "") != scope["agent_id"]:
                return False
            if name == "UserPromptSubmit":
                return not scope["turn_id"] or str(item.get("turn_id") or "") != scope["turn_id"]
            return str(item.get("turn_id") or "") == scope["turn_id"]

        pending = [item for item in pending if not closed(item)]
        tools = [item for item in tools if not closed(item)]
    return {"schema": 2, "pending": pending, "tools": tools,
            "last_event": {"name": name, "at": now, **scope}}
