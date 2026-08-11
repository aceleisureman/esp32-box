import unittest

from commands import match_rule
from realtime import stepfun_event_to_device


class ProtocolContractTest(unittest.TestCase):
    def test_session_created_is_not_ready(self):
        self.assertIsNone(
            stepfun_event_to_device({"type": "session.created"})
        )

    def test_session_updated_is_ready(self):
        self.assertEqual(
            stepfun_event_to_device({"type": "session.updated"}),
            ("json", {"type": "session.ready"}),
        )

    def test_named_song_command_keeps_song_name(self):
        self.assertEqual(
            match_rule("播放晴天"),
            {"action": "play_song", "song": "晴天"},
        )


if __name__ == "__main__":
    unittest.main()
