import json
import hashlib
import hmac
import os
import sys
import threading
import time
import unittest
from contextlib import redirect_stdout
from datetime import datetime, timedelta, timezone
from io import StringIO
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


BRIDGE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE_DIR))

from serve_usage import (
    ChallengeStore,
    UsageCache,
    apply_permission_hook_event,
    build_discovery_beacon,
    build_current_payload,
    compute_directed_broadcast,
    derive_agent_signal_from_codex_sessions,
    has_pending_permission,
    is_authorized_request,
    choose_discovery_host,
    read_bridge_token,
    request_signature,
)
from install_permission_hook import HOOK_EVENTS, install_hooks, remove_hooks
from permission_request_hook import main as permission_request_hook_main


class UsageStateTests(unittest.TestCase):
    def temporary_directory(self):
        path = BRIDGE_DIR / f".test-runtime-{time.monotonic_ns()}-{id(self)}"
        path.mkdir()
        self.addCleanup(self.remove_temporary_directory, path)
        return path

    @staticmethod
    def remove_temporary_directory(path):
        for child in path.iterdir():
            child.unlink()
        path.rmdir()

    def classify(self, records, *, modified_at):
        path = SimpleNamespace(
            stat=lambda: SimpleNamespace(st_mtime=modified_at.timestamp())
        )
        lines = [json.dumps(record) for record in records]
        with (
            patch("serve_usage.find_codex_session_paths", return_value=[path]),
            patch("serve_usage.read_recent_session_lines", return_value=lines),
        ):
            return derive_agent_signal_from_codex_sessions(Path("sessions"), Path("permission.json"))

    @staticmethod
    def record(timestamp, record_type, payload_type, **payload):
        return {
            "timestamp": timestamp.isoformat().replace("+00:00", "Z"),
            "type": record_type,
            "payload": {"type": payload_type, **payload},
        }

    def test_running_python_tool_is_not_idle_when_session_log_is_stale(self):
        now = datetime.now(timezone.utc)
        command_started = now - timedelta(minutes=4)
        records = [
            self.record(command_started, "event_msg", "task_started"),
            self.record(
                command_started,
                "response_item",
                "function_call",
                call_id="python-call-1",
                name="exec_command",
                arguments='{"cmd":"python long_job.py"}',
            ),
        ]
        self.assertEqual("working", self.classify(records, modified_at=command_started))

    def write_session(self, root, name, records, modified_at):
        path = root / name
        path.write_text("\n".join(json.dumps(record) for record in records) + "\n", encoding="utf-8")
        timestamp = modified_at.timestamp()
        os.utime(path, (timestamp, timestamp))
        return path

    def test_active_older_session_is_not_masked_by_newer_idle_session(self):
        now = datetime.now(timezone.utc)
        active_at = now - timedelta(minutes=4)
        idle_at = now - timedelta(seconds=5)
        active_records = [
            self.record(active_at, "event_msg", "task_started"),
            self.record(
                active_at,
                "response_item",
                "function_call",
                call_id="python-call-1",
                name="exec_command",
                arguments='{"cmd":"python long_job.py"}',
            ),
        ]
        idle_records = [
            self.record(idle_at, "event_msg", "task_started"),
            self.record(idle_at, "event_msg", "task_complete"),
        ]

        root = self.temporary_directory()
        self.write_session(root, "older-active.jsonl", active_records, active_at)
        self.write_session(root, "newer-idle.jsonl", idle_records, idle_at)

        self.assertEqual("working", derive_agent_signal_from_codex_sessions(root, root / "permission.json"))

    def test_unclosed_tool_call_expires_after_bounded_lease(self):
        now = datetime.now(timezone.utc)
        command_started = now - timedelta(minutes=31)
        records = [
            self.record(command_started, "event_msg", "task_started"),
            self.record(
                command_started,
                "response_item",
                "function_call",
                call_id="python-call-1",
                name="exec_command",
                arguments='{"cmd":"python lost_process.py"}',
            ),
        ]

        root = self.temporary_directory()
        self.write_session(root, "stale.jsonl", records, command_started)
        self.assertEqual("idle", derive_agent_signal_from_codex_sessions(root, root / "permission.json"))

    def test_completed_python_tool_and_turn_return_to_idle(self):
        now = datetime.now(timezone.utc)
        command_started = now - timedelta(minutes=4)
        command_finished = now - timedelta(seconds=10)
        records = [
            self.record(command_started, "event_msg", "task_started"),
            self.record(
                command_started,
                "response_item",
                "function_call",
                call_id="python-call-1",
                name="exec_command",
                arguments='{"cmd":"python short_job.py"}',
            ),
            self.record(
                command_finished,
                "response_item",
                "function_call_output",
                call_id="python-call-1",
                output="completed",
            ),
            self.record(command_finished, "event_msg", "task_complete"),
        ]
        self.assertEqual("idle", self.classify(records, modified_at=command_finished))

    def test_recent_reasoning_before_completion_does_not_keep_turn_active(self):
        now = datetime.now(timezone.utc)
        turn_started = now - timedelta(seconds=30)
        reasoning_at = now - timedelta(seconds=5)
        turn_completed = now - timedelta(seconds=1)
        records = [
            self.record(turn_started, "event_msg", "task_started"),
            self.record(reasoning_at, "response_item", "reasoning"),
            self.record(turn_completed, "event_msg", "task_complete"),
        ]

        self.assertEqual("idle", self.classify(records, modified_at=turn_completed))

    def test_interrupted_turn_clears_pending_tool_call(self):
        now = datetime.now(timezone.utc)
        turn_started = now - timedelta(seconds=40)
        command_started = now - timedelta(seconds=30)
        interrupted_at = now - timedelta(seconds=1)
        records = [
            self.record(turn_started, "event_msg", "task_started"),
            self.record(
                command_started,
                "response_item",
                "function_call",
                call_id="python-call-1",
                name="exec_command",
                arguments='{"cmd":"python long_job.py"}',
            ),
            self.record(
                interrupted_at,
                "event_msg",
                "turn_aborted",
                reason="interrupted",
            ),
        ]

        self.assertEqual("idle", self.classify(records, modified_at=interrupted_at))

    def test_permission_hook_tracks_pending_then_approved_tool_completion(self):
        directory = self.temporary_directory()
        state_path = directory / "permission.json"
        apply_permission_hook_event(
            {"hook_event_name": "PermissionRequest", "session_id": "s1", "turn_id": "t1"},
            state_path,
            clock=lambda: 100,
        )
        self.assertTrue(has_pending_permission(state_path, now=101))

        apply_permission_hook_event(
            {"hook_event_name": "PostToolUse", "session_id": "s1", "turn_id": "t1"},
            state_path,
            clock=lambda: 102,
        )
        self.assertFalse(has_pending_permission(state_path, now=103))

    def test_permission_hook_clears_pending_after_denied_tool_result(self):
        directory = self.temporary_directory()
        state_path = directory / "permission.json"
        apply_permission_hook_event(
            {"hook_event_name": "PermissionRequest", "session_id": "s1", "turn_id": "t1"},
            state_path,
            clock=lambda: 100,
        )

        apply_permission_hook_event(
            {
                "hook_event_name": "PostToolUse",
                "session_id": "s1",
                "turn_id": "t1",
                "tool_response": "Permission denied by user",
            },
            state_path,
            clock=lambda: 102,
        )
        self.assertFalse(has_pending_permission(state_path, now=103))

    def test_hook_script_leaves_permission_decision_to_codex_and_returns_stop_json(self):
        directory = self.temporary_directory()
        state_path = directory / "permission.json"

        def run_hook(event):
            output = StringIO()
            with (
                patch(
                    "sys.argv",
                    ["permission_request_hook.py", "--state-path", str(state_path)],
                ),
                patch("sys.stdin", StringIO(json.dumps(event))),
                redirect_stdout(output),
            ):
                self.assertEqual(0, permission_request_hook_main())
            return output.getvalue()

        permission_output = run_hook(
            {"hook_event_name": "PermissionRequest", "session_id": "s1", "turn_id": "t1"}
        )
        self.assertEqual("", permission_output)
        self.assertTrue(has_pending_permission(state_path))

        stop_output = run_hook({"hook_event_name": "Stop", "session_id": "s1", "turn_id": "t1"})
        self.assertEqual("{}\n", stop_output)
        self.assertFalse(has_pending_permission(state_path))

    def test_permission_state_replace_retries_transient_windows_lock(self):
        directory = self.temporary_directory()
        state_path = directory / "permission.json"
        real_replace = os.replace
        replace_attempts = 0

        def replace_after_transient_lock(source, destination):
            nonlocal replace_attempts
            replace_attempts += 1
            if replace_attempts == 1:
                raise PermissionError("temporary file lock")
            return real_replace(source, destination)

        with (
            patch("serve_usage.IS_WINDOWS", True),
            patch(
                "serve_usage.os.replace",
                side_effect=replace_after_transient_lock,
            ) as replace,
        ):
            apply_permission_hook_event(
                {"hook_event_name": "PermissionRequest", "session_id": "s1", "turn_id": "t1"},
                state_path,
                clock=lambda: 100,
            )

        self.assertEqual(2, replace.call_count)
        self.assertTrue(has_pending_permission(state_path, now=101))

    def test_permission_hook_interrupt_clears_only_the_matching_session(self):
        directory = self.temporary_directory()
        state_path = directory / "permission.json"
        for session_id in ("s1", "s2"):
            apply_permission_hook_event(
                {
                    "hook_event_name": "PermissionRequest",
                    "session_id": session_id,
                    "turn_id": "t1",
                },
                state_path,
                clock=lambda: 100,
            )

        apply_permission_hook_event(
            {"hook_event_name": "Interrupt", "session_id": "s1", "turn_id": "t1"},
            state_path,
            clock=lambda: 102,
        )
        self.assertTrue(has_pending_permission(state_path, now=103))
        saved_state = json.loads(state_path.read_text(encoding="utf-8"))
        self.assertEqual(["s2"], [item["session_id"] for item in saved_state["pending"]])

    def test_permission_hook_pending_state_expires(self):
        directory = self.temporary_directory()
        state_path = directory / "permission.json"
        apply_permission_hook_event(
            {"hook_event_name": "PermissionRequest", "session_id": "s1", "turn_id": "t1"},
            state_path,
            clock=lambda: 100,
        )
        self.assertFalse(has_pending_permission(state_path, now=100 + 24 * 60 * 60 + 1))

    def test_permission_hook_state_overrides_durable_session_signal(self):
        root = self.temporary_directory()
        state_path = root / "permission.json"
        apply_permission_hook_event(
            {"hook_event_name": "PermissionRequest", "session_id": "s1", "turn_id": "t1"},
            state_path,
            clock=lambda: time.time(),
        )
        self.assertEqual("permission", derive_agent_signal_from_codex_sessions(root, state_path))

    def test_hook_installer_is_idempotent_and_preserves_existing_hooks(self):
        directory = self.temporary_directory()
        hooks_path = directory / "hooks.json"
        state_path = directory / "permission.json"
        hooks_path.write_text(
            json.dumps(
                {
                    "description": "keep this",
                    "hooks": {
                        "SessionStart": [
                            {"hooks": [{"type": "command", "command": "existing-hook"}]}
                        ]
                    },
                }
            ),
            encoding="utf-8",
        )

        self.assertEqual(len(HOOK_EVENTS), install_hooks(hooks_path, state_path))
        self.assertEqual(0, install_hooks(hooks_path, state_path))
        config = json.loads(hooks_path.read_text(encoding="utf-8"))
        self.assertEqual("keep this", config["description"])
        self.assertTrue(
            any(
                handler.get("command") == "existing-hook"
                for group in config["hooks"]["SessionStart"]
                for handler in group["hooks"]
            )
        )

        self.assertEqual(len(HOOK_EVENTS), remove_hooks(hooks_path, state_path))
        config = json.loads(hooks_path.read_text(encoding="utf-8"))
        self.assertEqual(
            [{"type": "command", "command": "existing-hook"}],
            config["hooks"]["SessionStart"][0]["hooks"],
        )

    def wait_for(self, predicate, timeout=1.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.005)
        return predicate()

    def test_usage_cache_returns_without_waiting_for_cloud_fetch(self):
        fetch_started = threading.Event()
        release_fetch = threading.Event()
        returned = threading.Event()
        results = []

        def fetch():
            fetch_started.set()
            release_fetch.wait(2)
            return {"window": 1}

        cache = UsageCache(fetch)
        caller = threading.Thread(target=lambda: (results.append(cache.get()), returned.set()))
        caller.start()
        returned_early = returned.wait(0.2)
        release_fetch.set()
        caller.join(1)

        self.assertTrue(fetch_started.wait(1))
        self.assertTrue(returned_early, "usage cache blocked the request on cloud I/O")
        self.assertEqual([None], results)
        self.assertTrue(self.wait_for(lambda: cache.get() == {"window": 1}))

    def test_usage_cache_reuses_value_until_ttl_expires(self):
        now = [0.0]
        calls = []

        def fetch():
            calls.append(len(calls) + 1)
            return {"window": calls[-1]}

        cache = UsageCache(fetch, ttl_seconds=30, clock=lambda: now[0])
        self.assertIsNone(cache.get())
        self.assertTrue(self.wait_for(lambda: cache.get() == {"window": 1}))
        now[0] = 20.0
        self.assertEqual({"window": 1}, cache.get())
        now[0] = 30.0
        self.assertEqual(({"window": 1}, True), cache.get_snapshot())
        self.assertTrue(self.wait_for(lambda: cache.get() == {"window": 2}))
        self.assertEqual(2, len(calls))

    def test_usage_cache_keeps_last_good_value_when_refresh_fails(self):
        now = [0.0]
        calls = []
        refresh_started = threading.Event()
        release_refresh = threading.Event()

        def fetch():
            calls.append(len(calls) + 1)
            if len(calls) == 1:
                return {"window": 80}
            refresh_started.set()
            release_refresh.wait(1)
            raise RuntimeError("cloud unavailable")

        cache = UsageCache(fetch, ttl_seconds=30, retry_seconds=10, clock=lambda: now[0])
        self.assertIsNone(cache.get())
        self.assertTrue(self.wait_for(lambda: cache.get() == {"window": 80}))
        now[0] = 30.0

        self.assertEqual(({"window": 80}, True), cache.get_snapshot())
        self.assertTrue(refresh_started.wait(1))
        release_refresh.set()
        time.sleep(0.02)
        self.assertEqual({"window": 80}, cache.get())
        self.assertEqual(2, len(calls))

    def test_local_signal_is_returned_when_cloud_usage_is_unavailable(self):
        directory = self.temporary_directory()
        mode_path = directory / "mode.json"
        cache = UsageCache(lambda: (_ for _ in ()).throw(RuntimeError("offline")))
        with patch("serve_usage.derive_agent_signal_from_codex_sessions", return_value="working"):
            payload = build_current_payload(Path("auth.json"), mode_path, cache)

        self.assertEqual("working", payload["agent_signal"])
        self.assertFalse(payload["usage_available"])
        self.assertNotIn("window_pct", payload)

    def test_directed_broadcast_accepts_localized_ipconfig_mask_label(self):
        output = """以太网适配器 以太网:
   IPv4 地址 . . . . . . . . . . . : 192.168.1.42
   子网掩码  . . . . . . . . . . . : 255.255.255.0
   默认网关  . . . . . . . . . . . : 192.168.1.1
"""
        completed = SimpleNamespace(stdout=output)
        with patch("serve_usage.subprocess.run", return_value=completed):
            self.assertEqual("192.168.1.255", compute_directed_broadcast("192.168.1.42"))

    def test_discovery_host_override_must_be_a_local_lan_address(self):
        output = """Ethernet adapter Ethernet:
   IPv4 Address. . . . . . . . . . . : 192.168.1.42
   Subnet Mask . . . . . . . . . . . : 255.255.255.0
"""
        completed = SimpleNamespace(stdout=output)
        with (
            patch("serve_usage.socket.getaddrinfo", return_value=[]),
            patch("serve_usage.subprocess.run", return_value=completed),
        ):
            self.assertEqual("192.168.1.42", choose_discovery_host("192.168.1.42"))
            with self.assertRaises(ValueError):
                choose_discovery_host("192.168.1.99")

    def test_bridge_request_signatures_are_one_time_and_bound_to_client(self):
        token = "a" * 64
        client_ip = "192.168.1.20"
        challenges = ChallengeStore()
        self.assertTrue(is_authorized_request("127.0.0.1", "/usage", "", "", token, challenges))

        nonce = challenges.issue(client_ip)
        signature = request_signature(token, "GET", "/usage", nonce)
        self.assertNotIn(token, signature)
        self.assertFalse(
            is_authorized_request(client_ip, "/health", nonce, signature, token, challenges)
        )
        self.assertFalse(
            is_authorized_request("192.168.1.21", "/usage", nonce, signature, token, challenges)
        )
        self.assertTrue(
            is_authorized_request(client_ip, "/usage", nonce, signature, token, challenges)
        )
        self.assertFalse(
            is_authorized_request(client_ip, "/usage", nonce, signature, token, challenges)
        )

    def test_bridge_request_signature_challenge_expires(self):
        now = [10.0]
        token = "d" * 64
        client_ip = "192.168.1.20"
        challenges = ChallengeStore(ttl_seconds=5, clock=lambda: now[0])
        nonce = challenges.issue(client_ip)
        signature = request_signature(token, "GET", "/usage", nonce)

        now[0] = 15.0
        self.assertFalse(
            is_authorized_request(client_ip, "/usage", nonce, signature, token, challenges)
        )

    def test_bridge_token_loads_from_local_secrets_header(self):
        token = "b" * 64
        with patch.object(
            Path,
            "read_text",
            return_value=f'static constexpr char kBridgeAuthToken[] = "{token}";\n',
        ):
            secrets_path = Path("wifi_secrets.h")
            self.assertEqual(token, read_bridge_token(secrets_path))

    def test_bridge_token_rejects_example_placeholder(self):
        with patch.object(
            Path,
            "read_text",
            return_value='static constexpr char kBridgeAuthToken[] = "replace-this-token";\n',
        ):
            secrets_path = Path("wifi_secrets.h")
            with self.assertRaises(ValueError):
                read_bridge_token(secrets_path)

    def test_discovery_beacon_signs_endpoint_without_broadcasting_token(self):
        token = "c" * 64
        host = "192.168.1.20"
        port = 8765
        beacon = build_discovery_beacon(port, host, token)
        payload = json.loads(beacon)
        message = f"codex-miao-bridge\n1\n{host}\n{port}".encode("ascii")
        expected = hmac.new(token.encode("ascii"), message, hashlib.sha256).hexdigest()

        self.assertEqual(expected, payload["signature"])
        self.assertNotIn(token.encode("ascii"), beacon)


if __name__ == "__main__":
    unittest.main()
