"""Incremental, per-session Codex activity; silence is not a completion event."""

import json
import re
import secrets
import threading
import time
from dataclasses import dataclass, field, replace
from datetime import datetime
from pathlib import Path

ACTIVE = {"thinking", "working", "permission"}
PRIORITY = ("permission", "working", "thinking", "blocked", "unknown", "done", "idle")
TAIL_BYTES = 256 * 1024
READ_BYTES = 256 * 1024
MAX_LINE_BYTES = 256 * 1024


def event_time(record, fallback):
    value = record.get("timestamp")
    if not isinstance(value, str):
        return fallback
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except (ValueError, OverflowError):
        return fallback


def execution_result(output):
    """Recognize unified-exec and code-mode yields without treating them as done."""
    data = output
    if isinstance(output, str):
        try:
            data = json.loads(output)
        except ValueError:
            data = {}
    jobs = set()
    finished = False
    if isinstance(data, dict):
        finished = isinstance(data.get("exit_code"), int)
        if data.get("session_id") is not None and not finished:
            jobs.add("process:" + str(data["session_id"]))
        if data.get("cell_id") is not None and not finished:
            jobs.add("cell:" + str(data["cell_id"]))
    text = output if isinstance(output, str) else ""
    for pattern, prefix in ((r"Process running with session ID ([\w-]+)", "process:"),
                            (r"Script running with cell ID ([\w-]+)", "cell:")):
        jobs.update(prefix + value for value in re.findall(pattern, text))
    finished = finished or bool(re.search(r"(?m)^Process exited with code |^Script completed", text))
    return jobs, finished


@dataclass
class Session:
    path: Path
    identity: tuple
    session_id: str
    offset: int = 0
    partial: bytes = b""
    discard_line: bool = False
    state: str = "unknown"
    turn_id: str = ""
    updated_at: float = 0
    resolved_at: float = 0
    terminal_until: float = 0
    missing_since: float | None = None
    internal: bool = False
    pending: set = field(default_factory=set)
    jobs: set = field(default_factory=set)
    calls: dict = field(default_factory=dict)

    def apply(self, record, now, wall_now, live):
        if not isinstance(record, dict) or not isinstance(record.get("payload"), dict):
            return
        payload = record["payload"]
        kind = payload.get("type")
        if record.get("type") == "session_meta":
            self.session_id = str(payload.get("id") or self.session_id)
            source = payload.get("source")
            self.internal = self.internal or (isinstance(source, dict) and "subagent" in source)
            return
        if record.get("type") == "turn_context":
            self.internal = self.internal or payload.get("model") in {
                "codex-auto-review", "codex_auto_review", "approvals-reviewer"
            }
            return
        if kind not in {"task_started", "task_complete", "turn_aborted", "user_message",
                        "function_call", "custom_tool_call", "function_call_output",
                        "custom_tool_call_output", "reasoning"}:
            return
        timestamp = event_time(record, wall_now)
        if timestamp < self.updated_at:
            return
        turn_id = str(payload.get("turn_id") or "")
        if kind in {"task_complete", "turn_aborted"} and turn_id and self.turn_id and turn_id != self.turn_id:
            return
        if kind == "task_started":
            self.pending.clear()
            self.jobs.clear()
            self.calls.clear()
            self.turn_id = turn_id
            self.terminal_until = 0
            self.state = "thinking"
        elif kind == "user_message":
            if self.state not in ACTIVE:
                self.pending.clear()
                self.terminal_until = 0
                self.turn_id = turn_id
            self.state = "working" if self.pending or self.jobs else "thinking"
        elif kind in {"task_complete", "turn_aborted"}:
            if self.state in {"done", "blocked"} and self.terminal_until:
                return  # Repeated terminal events must not restart or erase the hold.
            was_active = self.state in ACTIVE
            self.pending.clear()
            self.jobs.clear()
            self.calls.clear()
            self.resolved_at = timestamp
            if kind == "turn_aborted":
                interrupted = payload.get("reason") in {"interrupted", "cancelled", "canceled", "aborted_by_user"}
                self.state = "blocked" if live and was_active and not interrupted else "idle"
                self.terminal_until = now + 60 if self.state == "blocked" else 0
            else:
                self.state = "done" if live and was_active else "idle"
                self.terminal_until = now + 60 if self.state == "done" else 0
        elif kind in {"function_call", "custom_tool_call"}:
            call_id = payload.get("call_id")
            if isinstance(call_id, str) and call_id:
                self.pending.add(call_id)
                try:
                    arguments = payload.get("arguments") or {}
                    arguments = json.loads(arguments) if isinstance(arguments, str) else arguments
                    if isinstance(arguments, dict):
                        self.calls[call_id] = {
                            prefix + str(arguments[key]) for key, prefix in (("session_id", "process:"), ("cell_id", "cell:"))
                            if arguments.get(key) is not None
                        }
                except ValueError:
                    pass
            self.state = "working"
        elif kind in {"function_call_output", "custom_tool_call_output"}:
            call_id = payload.get("call_id")
            if not isinstance(call_id, str):
                return
            self.pending.discard(call_id)
            jobs, finished = execution_result(payload.get("output"))
            references = self.calls.pop(call_id, set())
            if finished:
                self.jobs.difference_update(references)
            self.jobs.update(jobs)
            self.resolved_at = timestamp
            # A tool can yield a running subprocess. The turn remains active
            # until task_complete/turn_aborted, even while no log is appended.
            if self.state in ACTIVE:
                self.state = "working" if self.pending or self.jobs else "thinking"
        elif kind == "reasoning" and self.state in ACTIVE:
            self.state = "working" if self.pending or self.jobs else "thinking"
        self.updated_at = timestamp

    def visible_state(self, now):
        if self.missing_since is not None and now - self.missing_since >= 15:
            return "unknown" if self.state in ACTIVE or self.state == "unknown" else "idle"
        if self.state in {"done", "blocked"} and now >= self.terminal_until:
            return "idle"
        return self.state


class SessionActivityMonitor:
    def __init__(self, sessions_root, permission_path, *, clock=time.monotonic,
                 wall_clock=time.time, discovery_interval=10):
        self.root = Path(sessions_root)
        self.permission_path = Path(permission_path)
        self.clock, self.wall_clock = clock, wall_clock
        self.started_wall = wall_clock()
        self.discovery_interval = discovery_interval
        self.stream = secrets.token_hex(8)
        self.revision = 0
        self.updated_at = 0
        self._fingerprint = None
        self._sessions = {}
        self._published = {}
        self._discovered_at = None
        self._scanned_at = None
        self._scan_ok = False
        self._first_scan = True
        self._lock = threading.RLock()
        self._scan_lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self._last_error = ""

    def start(self):
        self.scan()
        self._thread = threading.Thread(target=self._run, name="codex-activity", daemon=True)
        self._thread.start()

    def close(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)

    def _run(self):
        while not self._stop.wait(1):
            try:
                self.scan()
            except Exception as exc:
                with self._lock:
                    self._scan_ok = False
                    self._last_error = type(exc).__name__

    def _read(self, entry, stat, now, wall_now):
        with entry.path.open("rb") as handle:
            handle.seek(entry.offset)
            data = handle.read(READ_BYTES)
        entry.offset += len(data)
        lines = (entry.partial + data).split(b"\n")
        entry.partial = lines.pop()
        for line in lines:
            if entry.discard_line:
                entry.discard_line = False
                continue
            if len(line) > MAX_LINE_BYTES:
                continue
            try:
                record = json.loads(line)
            except (ValueError, UnicodeError):
                continue
            live = not self._first_scan and event_time(record, wall_now) >= self.started_wall
            entry.apply(record, now, wall_now, live)
        if len(entry.partial) > MAX_LINE_BYTES:
            entry.partial = b""
            entry.discard_line = True
        entry.missing_since = None

    def scan(self):
        with self._scan_lock:
            now, wall_now = self.clock(), self.wall_clock()
            paths = set(self._sessions)
            try:
                if not self.root.is_dir():
                    raise FileNotFoundError("session directory unavailable")
                if self._discovered_at is None or now - self._discovered_at >= self.discovery_interval:
                    recent = []
                    for path in self.root.rglob("*.jsonl"):
                        try:
                            recent.append((path.stat().st_mtime, str(path), path))
                        except OSError:
                            continue
                    paths.update(item[2] for item in sorted(recent, reverse=True)[:256])
                    self._discovered_at = now
                for path in paths:
                    entry = self._sessions.get(path)
                    try:
                        stat = path.stat()
                        identity = (stat.st_dev, stat.st_ino)
                        if entry is None or entry.identity != identity or stat.st_size < entry.offset:
                            offset = max(0, stat.st_size - TAIL_BYTES)
                            entry = Session(path, identity, path.stem, offset=offset, discard_line=offset > 0)
                            self._sessions[path] = entry
                            if offset:
                                with path.open("rb") as header:
                                    try:
                                        metadata = json.loads(header.readline(MAX_LINE_BYTES))
                                        if metadata.get("type") == "session_meta":
                                            entry.apply(metadata, now, wall_now, False)
                                    except (ValueError, UnicodeError, AttributeError):
                                        pass
                        if stat.st_size > entry.offset:
                            self._read(entry, stat, now, wall_now)
                            if self._first_scan and wall_now - entry.updated_at > 1800 and entry.state in ACTIVE:
                                entry.state = "unknown"
                        else:
                            entry.missing_since = None
                    except OSError:
                        if entry and entry.missing_since is None:
                            entry.missing_since = now
                scan_ok, last_error = True, ""
            except OSError as exc:
                scan_ok, last_error = False, type(exc).__name__
            with self._lock:
                self._published = {path: replace(entry) for path, entry in self._sessions.items()}
                self._scanned_at = now
                self._scan_ok = scan_ok
                self._last_error = last_error
            self._first_scan = False

    def _permissions(self, entries):
        try:
            data = json.loads(self.permission_path.read_text(encoding="utf-8"))
            pending = data.get("pending", []) if isinstance(data, dict) else []
        except (OSError, ValueError):
            return set()
        ids = set()
        for item in pending if isinstance(pending, list) else []:
            if not isinstance(item, dict) or not item.get("session_id"):
                continue
            try:
                created = float(item["created_at"])
            except (KeyError, TypeError, ValueError):
                continue
            if not 0 <= self.wall_clock() - created <= 86400:
                continue
            session_id = str(item["session_id"])
            entry = entries.get(session_id)
            if entry and (entry.internal or entry.resolved_at >= created):
                continue
            ids.add(session_id)
        return ids

    def snapshot(self, *, diagnostics=False, paused=False):
        with self._lock:
            now = self.clock()
            entries = {}
            for entry in self._published.values():
                previous = entries.get(entry.session_id)
                if previous is None or (entry.updated_at, entry.missing_since is None) > (previous.updated_at, previous.missing_since is None):
                    entries[entry.session_id] = entry
            states = {sid: entry.visible_state(now) for sid, entry in entries.items() if not entry.internal}
            for sid in self._permissions(entries):
                states[sid] = "permission"
            healthy = self._scan_ok and self._scanned_at is not None and now - self._scanned_at <= 10
            if not healthy:
                states = {sid: "unknown" for sid in states} or {"monitor": "unknown"}
            if paused:
                states = {}
            counts = {
                "active_sessions": sum(value in {"thinking", "working"} for value in states.values()),
                "permission_sessions": sum(value == "permission" for value in states.values()),
                "completed_sessions": sum(value == "done" for value in states.values()),
                "unknown_sessions": sum(value == "unknown" for value in states.values()),
            }
            signal = next((state for state in PRIORITY if state in states.values()), "idle")
            fingerprint = (paused, signal, tuple(sorted((sid, state, entries[sid].turn_id if sid in entries else "")
                                                for sid, state in states.items())))
            if fingerprint != self._fingerprint:
                self.revision += 1
                self.updated_at = int(self.wall_clock() * 1000)
                self._fingerprint = fingerprint
            result = {"agent_signal": signal, **counts, "status_stream": self.stream,
                      "status_revision": self.revision, "status_updated_at": self.updated_at,
                      "status_source": "codex-events"}
            if diagnostics:
                result["monitor_ok"] = healthy
                result["last_error"] = self._last_error
                result["tracked_files"] = len(self._published)
                result["sessions"] = [
                    {"id": sid, "state": state,
                     "source_updated_at": int(entries[sid].updated_at * 1000) if sid in entries else 0}
                    for sid, state in sorted(states.items()) if state != "idle"
                ][:16]
            return result
