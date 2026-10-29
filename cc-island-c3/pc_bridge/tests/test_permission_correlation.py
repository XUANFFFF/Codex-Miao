import json
import secrets
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from serve_usage import apply_permission_hook_event


class PermissionCorrelationTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parent / (".permission-test-" + secrets.token_hex(8))
        self.root.mkdir()
        self.path = self.root / "state.json"
        self.now = 100.0
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for path in self.root.iterdir():
            path.unlink()
        self.root.rmdir()

    def event(self, name, command="tool-a", call=None, *, turn="t1", **extra):
        event = {"hook_event_name": name, "session_id": "s1", "turn_id": turn,
                 "tool_name": "Bash", "tool_input": {"command": command}, **extra}
        if call:
            event["tool_use_id"] = call
        apply_permission_hook_event(event, self.path, clock=lambda: self.now)
        self.now += 1

    def pending(self):
        return json.loads(self.path.read_text())["pending"]

    def test_same_turn_keeps_distinct_permission_requests(self):
        self.event("PermissionRequest", "a")
        self.event("PermissionRequest", "b")
        self.assertEqual(2, len(self.pending()))

    def test_unrelated_tool_completion_does_not_clear_waiting_request(self):
        self.event("PermissionRequest", "a")
        self.event("PostToolUse", "b", "call-b")
        self.assertEqual(1, len(self.pending()))

    def test_pre_tool_identity_resolves_only_the_matching_request(self):
        self.event("PreToolUse", "a", "call-a")
        self.event("PermissionRequest", "a")
        self.event("PreToolUse", "b", "call-b")
        self.event("PermissionRequest", "b")
        self.event("PostToolUse", "a", "call-a")
        self.assertEqual(["call-b"], [item["tool_use_id"] for item in self.pending()])
        self.event("PostToolUse", "b", "call-b")
        self.assertEqual([], self.pending())

    def test_identical_parallel_inputs_are_not_guessed(self):
        self.event("PreToolUse", "same", "call-a")
        self.event("PreToolUse", "same", "call-b")
        self.event("PermissionRequest", "same")
        self.event("PostToolUse", "same", "call-b")
        self.assertEqual(1, len(self.pending()))
        self.event("PostToolUse", "same", "call-a")
        self.assertEqual(1, len(self.pending()))
        self.event("Stop")
        self.assertEqual([], self.pending())

    def test_repeated_post_does_not_consume_another_identical_request(self):
        self.event("PreToolUse", "same", "call-a")
        self.event("PermissionRequest", "same")
        self.event("PreToolUse", "same", "call-b")
        self.event("PermissionRequest", "same")
        self.event("PostToolUse", "same", "call-a")
        self.event("PostToolUse", "same", "call-a")
        self.assertEqual(1, len(self.pending()))
        self.assertEqual("call-b", self.pending()[0]["tool_use_id"])

    def test_late_stop_from_previous_turn_does_not_clear_new_request(self):
        self.event("PermissionRequest", "a", turn="t2")
        self.event("Stop", turn="t1")
        self.assertEqual(1, len(self.pending()))

    def test_compaction_does_not_clear_pending_requests(self):
        self.event("PermissionRequest")
        self.event("SessionStart", source="compact")
        self.assertEqual(1, len(self.pending()))

    def test_subagent_completion_and_stop_do_not_clear_parent_request(self):
        self.event("PreToolUse", "a", "call-a")
        self.event("PermissionRequest", "a")
        self.event("PostToolUse", "a", "call-a", agent_id="child")
        self.event("Stop", agent_id="child")
        self.assertEqual(1, len(self.pending()))
        self.event("PostToolUse", "a", "call-a")
        self.assertEqual([], self.pending())

    def test_approval_description_changes_do_not_break_unique_mapping(self):
        self.event("PreToolUse", "a", "call-a",
                   tool_input={"command": "a", "description": "read file"})
        self.event("PermissionRequest", "a",
                   tool_input={"description": "allow read?", "command": "a"})
        self.assertEqual("call-a", self.pending()[0]["tool_use_id"])
        self.event("PostToolUse", "a", "call-a")
        self.assertEqual([], self.pending())

    def test_persisted_state_never_contains_command_text(self):
        self.event("PreToolUse", "sensitive-command-value", "call-a")
        self.event("PermissionRequest", "sensitive-command-value")
        self.assertNotIn("sensitive-command-value", self.path.read_text())

    def test_registration_check_is_read_only_and_reports_missing_pre_hook(self):
        from install_permission_hook import install_hooks, check_hooks
        path = self.root / "hooks.json"
        install_hooks(path, self.path)
        before = path.read_bytes()
        report = check_hooks(path, self.path)
        self.assertEqual([], report["missing_events"])
        self.assertEqual(8, len(report["installed_events"]))
        self.assertEqual(before, path.read_bytes())
        config = json.loads(path.read_text(encoding="utf-8"))
        del config["hooks"]["PreToolUse"]
        path.write_text(json.dumps(config), encoding="utf-8")
        self.assertEqual(["PreToolUse"], check_hooks(path, self.path)["missing_events"])


if __name__ == "__main__":
    unittest.main()
