import json
import sys
import secrets
import unittest
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from session_activity import SessionActivityMonitor


class SessionActivityTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parent / (".activity-test-" + secrets.token_hex(8))
        self.root.mkdir()
        self.addCleanup(self.cleanup)
        self.now = 1_800_000_000.0
        self.monitor = SessionActivityMonitor(
            self.root, self.root / "permissions.json", clock=lambda: self.now,
            wall_clock=lambda: self.now, discovery_interval=0,
        )
        self.monitor.scan()

    def cleanup(self):
        for path in self.root.iterdir():
            path.unlink()
        self.root.rmdir()

    def append(self, filename, kind, **payload):
        from datetime import datetime, timezone
        record = {
            "timestamp": datetime.fromtimestamp(self.now, timezone.utc).isoformat(),
            "type": "response_item" if kind in {"function_call", "function_call_output", "reasoning"} else "event_msg",
            "payload": {"type": kind, **payload},
        }
        with (self.root / filename).open("ab") as handle:
            handle.write((json.dumps(record) + "\n").encode())

    def scan(self):
        self.monitor.scan()
        return self.monitor.snapshot()

    def test_long_tool_survives_log_tail_and_silence(self):
        self.append("a.jsonl", "task_started", turn_id="t1")
        self.append("a.jsonl", "function_call", call_id="python")
        self.assertEqual("working", self.scan()["agent_signal"])
        for _ in range(300):
            self.append("a.jsonl", "token_count")
        self.now += 3600
        self.assertEqual("working", self.scan()["agent_signal"])

    def test_partial_utf8_record_is_applied_only_after_newline(self):
        self.append("a.jsonl", "task_started", turn_id="t1")
        self.scan()
        raw = json.dumps({"type": "response_item", "payload": {"type": "function_call", "call_id": "中文"}}, ensure_ascii=False).encode()
        path = self.root / "a.jsonl"
        with path.open("ab") as handle:
            handle.write(raw[:-2])
        self.assertEqual("thinking", self.scan()["agent_signal"])
        with path.open("ab") as handle:
            handle.write(raw[-2:] + b"\n")
        self.assertEqual("working", self.scan()["agent_signal"])

    def test_missing_file_becomes_unknown_without_inventing_completion(self):
        self.append("a.jsonl", "task_started")
        self.scan()
        (self.root / "a.jsonl").unlink()
        self.assertEqual("thinking", self.scan()["agent_signal"])
        self.now += 16
        result = self.scan()
        self.assertEqual("unknown", result["agent_signal"])
        self.assertEqual(0, result["completed_sessions"])

    def test_terminal_hold_is_not_extended_by_polling(self):
        self.append("a.jsonl", "task_started", turn_id="t1")
        self.scan()
        self.now += 1
        self.append("a.jsonl", "task_complete", turn_id="t1")
        completed = self.scan()
        self.assertEqual("done", completed["agent_signal"])
        self.now += 30
        self.assertEqual(completed["status_revision"], self.scan()["status_revision"])
        self.now += 31
        self.assertEqual("idle", self.scan()["agent_signal"])

    def test_old_turn_completion_cannot_stop_new_turn(self):
        self.append("a.jsonl", "task_started", turn_id="t2")
        self.scan()
        self.now += 1
        self.append("a.jsonl", "task_complete", turn_id="t1")
        self.assertEqual("thinking", self.scan()["agent_signal"])

    def test_two_sessions_keep_working_when_one_finishes(self):
        for name in ("a.jsonl", "b.jsonl"):
            self.append(name, "task_started")
        self.assertEqual(2, self.scan()["active_sessions"])
        self.now += 1
        self.append("b.jsonl", "task_complete")
        result = self.scan()
        self.assertEqual("thinking", result["agent_signal"])
        self.assertEqual(1, result["active_sessions"])
        self.assertEqual(1, result["completed_sessions"])

    def test_cold_start_does_not_replay_historical_completion(self):
        self.append("a.jsonl", "task_started")
        self.append("a.jsonl", "task_complete")
        other = SessionActivityMonitor(self.root, self.root / "permissions.json")
        other.scan()
        self.assertEqual("idle", other.snapshot()["agent_signal"])

    def test_truncated_file_does_not_retain_old_pending_call(self):
        self.append("a.jsonl", "function_call", call_id="old")
        self.assertEqual("working", self.scan()["agent_signal"])
        (self.root / "a.jsonl").write_bytes(b"\n")
        self.assertEqual("unknown", self.scan()["agent_signal"])

    def test_stalled_monitor_reports_unknown(self):
        self.append("a.jsonl", "task_started")
        self.scan()
        self.now += 20
        self.assertEqual("unknown", self.monitor.snapshot()["agent_signal"])

    def test_yielded_python_process_stays_working_until_poll_finishes(self):
        self.append("a.jsonl", "task_started", turn_id="t1")
        self.append("a.jsonl", "function_call", call_id="start", name="exec_command", arguments='{"cmd":"python job.py"}')
        self.append("a.jsonl", "function_call_output", call_id="start", output='{"session_id":123,"output":"still running"}')
        self.assertEqual("working", self.scan()["agent_signal"])
        self.now += 3600
        self.assertEqual("working", self.scan()["agent_signal"])
        self.append("a.jsonl", "function_call", call_id="poll", name="write_stdin", arguments='{"session_id":123}')
        self.append("a.jsonl", "function_call_output", call_id="poll", output='{"exit_code":0,"output":"done"}')
        self.assertEqual("thinking", self.scan()["agent_signal"])

    def test_new_turn_advances_revision_even_if_state_stays_thinking(self):
        self.append("a.jsonl", "task_started", turn_id="t1")
        first = self.scan()
        self.now += 1
        self.append("a.jsonl", "task_started", turn_id="t2")
        self.assertGreater(self.scan()["status_revision"], first["status_revision"])

    def test_cold_start_does_not_replay_historical_error(self):
        self.append("a.jsonl", "task_started")
        self.append("a.jsonl", "turn_aborted", reason="error")
        other = SessionActivityMonitor(self.root, self.root / "permissions.json")
        other.scan()
        self.assertEqual("idle", other.snapshot()["agent_signal"])

    def test_permission_is_correlated_to_session_and_cleared_by_completion(self):
        path = self.root / "a.jsonl"
        path.write_text(json.dumps({"type": "session_meta", "payload": {"id": "s1"}}) + "\n")
        self.append("a.jsonl", "task_started", turn_id="t1")
        self.scan()
        self.now += 1
        self.monitor.permission_path.write_text(json.dumps({"pending": [{"session_id": "s1", "turn_id": "t1", "created_at": self.now}]}))
        self.assertEqual(1, self.monitor.snapshot()["permission_sessions"])
        self.now += 1
        self.append("a.jsonl", "task_complete", turn_id="t1")
        self.assertEqual(0, self.scan()["permission_sessions"])

    def test_http_payload_uses_monitor_snapshot_and_health_diagnostics_omit_text(self):
        from serve_usage import build_current_payload
        self.append("a.jsonl", "task_started", turn_id="t1", text="private prompt")
        self.scan()
        result = build_current_payload(Path("auth.json"), self.root / "mode.json", activity_monitor=self.monitor)
        self.assertEqual("thinking", result["agent_signal"])
        self.assertEqual(1, result["active_sessions"])
        self.assertFalse(result["usage_available"])
        self.assertNotIn("private prompt", json.dumps(self.monitor.snapshot(diagnostics=True)))

    def test_slow_filesystem_scan_does_not_block_snapshot_watchdog(self):
        from unittest.mock import patch
        self.append("a.jsonl", "task_started")
        self.scan()
        self.append("a.jsonl", "reasoning")
        entered, release, returned = threading.Event(), threading.Event(), threading.Event()
        results = []
        original = self.monitor._read

        def delayed_read(*args):
            entered.set()
            release.wait(2)
            return original(*args)

        with patch.object(self.monitor, "_read", side_effect=delayed_read):
            worker = threading.Thread(target=self.monitor.scan)
            worker.start()
            self.assertTrue(entered.wait(1))
            self.now += 20
            reader = threading.Thread(target=lambda: (results.append(self.monitor.snapshot()), returned.set()))
            reader.start()
            nonblocking = returned.wait(0.2)
            release.set()
            worker.join(2)
            reader.join(2)
        self.assertTrue(nonblocking, "HTTP status was blocked on filesystem I/O")
        self.assertEqual("unknown", results[0]["agent_signal"])

    def test_repeated_completion_does_not_remove_or_extend_hold(self):
        self.append("a.jsonl", "task_started", turn_id="t1")
        self.scan()
        self.now += 1
        self.append("a.jsonl", "task_complete", turn_id="t1")
        self.scan()
        self.now += 30
        self.append("a.jsonl", "task_complete", turn_id="t1")
        self.assertEqual("done", self.scan()["agent_signal"])
        self.now += 31
        self.assertEqual("idle", self.scan()["agent_signal"])

    def test_pause_and_resume_each_advance_status_revision(self):
        from serve_usage import build_current_payload
        self.append("a.jsonl", "task_started")
        live = self.scan()
        mode = self.root / "mode.json"
        mode.write_text('{"mode":"offline"}')
        paused = build_current_payload(Path("auth"), mode, activity_monitor=self.monitor)
        self.assertEqual("idle", paused["agent_signal"])
        self.assertGreater(paused["status_revision"], live["status_revision"])
        mode.write_text('{"mode":"live"}')
        resumed = build_current_payload(Path("auth"), mode, activity_monitor=self.monitor)
        self.assertEqual("thinking", resumed["agent_signal"])
        self.assertGreater(resumed["status_revision"], paused["status_revision"])

    def test_internal_child_session_is_excluded(self):
        (self.root / "a.jsonl").write_text(json.dumps({"type": "session_meta", "payload": {
            "id": "child", "source": {"subagent": {"parent_thread_id": "parent"}}
        }}) + "\n")
        self.append("a.jsonl", "task_started")
        result = self.scan()
        self.assertEqual("idle", result["agent_signal"])
        self.assertEqual(0, result["active_sessions"])

    def test_large_session_bootstrap_keeps_header_identity(self):
        path = self.root / "a.jsonl"
        path.write_text(json.dumps({"type": "session_meta", "payload": {"id": "original-session"}}) + "\n")
        with path.open("ab") as handle:
            handle.write((b'{"payload":{"type":"token_count"}}\n') * 10000)
        self.append("a.jsonl", "task_started")
        self.scan()
        sessions = self.monitor.snapshot(diagnostics=True)["sessions"]
        self.assertEqual("original-session", sessions[0]["id"])


if __name__ == "__main__":
    unittest.main()
