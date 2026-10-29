import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import send_serial


class SerialBridgeTests(unittest.TestCase):
    def test_serial_uses_the_same_snapshot_as_http(self):
        payload = {"agent_signal": "working", "active_sessions": 2, "status_revision": 9,
                   "status_stream": "one-bridge", "usage_available": False}
        args = SimpleNamespace(mode="bridge", json_file=None, bridge_url="http://127.0.0.1:8765")
        with patch("send_serial.read_bridge_payload", return_value=payload):
            self.assertEqual(payload, send_serial.load_payload(args))

    def test_device_debug_lines_do_not_count_as_acknowledgement(self):
        class Port:
            def __init__(self):
                self.lines = iter([b"wifi: connected\n", b"fetch: ok\n", b"OK\n"])

            def readline(self):
                return next(self.lines)

        self.assertEqual("OK", send_serial.read_reply(Port()))

    def test_remote_bridge_is_rejected_before_fetch(self):
        with self.assertRaises(ValueError):
            send_serial.read_bridge_payload("http://192.168.1.10:8765")

    def test_compact_snapshot_fits_device_serial_buffer(self):
        payload = {"agent_signal": "permission", "status_stream": "a" * 16,
                   "status_revision": 100000, "status_updated_at": 1800000000000,
                   "status_source": "codex-events", "active_sessions": 100,
                   "permission_sessions": 100, "completed_sessions": 100,
                   "unknown_sessions": 100, "window_pct": 100, "week_pct": 100,
                   "usage_available": True, "usage_stale": False, "reset_text": "12:30",
                   "reset_min": 1234}
        self.assertLess(len(send_serial.encode_payload(payload)), 1024)
        self.assertEqual(payload, json.loads(send_serial.encode_payload(payload)))


if __name__ == "__main__":
    unittest.main()
