import json
import hashlib
import hmac
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


BRIDGE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE_DIR))

from serve_usage import (
    ChallengeStore,
    UsageCache,
    build_discovery_beacon,
    derive_agent_signal_from_codex_sessions,
    is_authorized_request,
    read_bridge_token,
    request_signature,
)


class UsageStateTests(unittest.TestCase):
    def classify(self, records, *, modified_at):
        path = SimpleNamespace(
            stat=lambda: SimpleNamespace(st_mtime=modified_at.timestamp())
        )
        lines = [json.dumps(record) for record in records]
        with (
            patch("serve_usage.find_latest_codex_session_path", return_value=path),
            patch("serve_usage.read_recent_session_lines", return_value=lines),
        ):
            return derive_agent_signal_from_codex_sessions(Path("sessions"))

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

    def test_usage_cache_reuses_value_until_ttl_expires(self):
        now = [0.0]
        calls = []

        def fetch():
            calls.append(len(calls) + 1)
            return {"window": calls[-1]}

        cache = UsageCache(fetch, ttl_seconds=30, clock=lambda: now[0])
        self.assertEqual({"window": 1}, cache.get())
        now[0] = 20.0
        self.assertEqual({"window": 1}, cache.get())
        now[0] = 30.0
        self.assertEqual({"window": 2}, cache.get())
        self.assertEqual(2, len(calls))

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
