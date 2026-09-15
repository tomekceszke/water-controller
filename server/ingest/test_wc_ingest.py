"""Unit tests for payload validation: uv run --with paho-mqtt python -m unittest server/ingest/test_wc_ingest.py"""
import json
import pathlib
import sys
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wc_ingest import parse  # noqa: E402

DEV = "30aea40aba44"
NOW = int(time.time())


def msg(kind, **data):
    return f"water/{DEV}/{kind}", json.dumps(data).encode()


class ParseTest(unittest.TestCase):
    def test_flow_as_sent_by_firmware(self):
        kind, row = parse(*msg("flow", start=NOW - 60, stop=NOW, pulses=2000, liters=4.193, max_lpm=12.5,
                                closed=False, pulses_per_liter=477))
        self.assertEqual(kind, "flow")
        self.assertEqual(row["pulses"], 2000)
        self.assertEqual(row["pulses_per_liter"], 477)
        self.assertFalse(row["closed"])

    def test_sample_valve_rule_alert(self):
        self.assertEqual(parse(*msg("sample", ts=NOW, pulses=950, period_ms=10000, lpm=11.95, flow_pulses=4000))[0],
                         "sample")
        self.assertEqual(parse(*msg("valve", ts=NOW, state="closed", reason="tier1", detail="flow > 20 min"))[1]["state"],
                         "closed")
        self.assertTrue(parse(*msg("rule", ts=NOW, rule="max_volume", close=True, pulses=2862, detail="6 L"))[1]["close"])
        self.assertEqual(parse(*msg("alert", ts=NOW, detail="6.2 L/min"))[1]["detail"], "6.2 L/min")

    def test_status(self):
        self.assertEqual(parse(f"water/{DEV}/status", b"offline"), ("status", {"device": DEV, "status": "offline"}))
        with self.assertRaises(ValueError):
            parse(f"water/{DEV}/status", b"rebooting")

    def test_rejects_bad_topics(self):
        for topic in ("water/30aea40aba44/unknown", "water/NOTAMAC/flow", "heating/30aea40aba44/flow", "water/flow"):
            with self.assertRaises(ValueError, msg=topic):
                parse(topic, b"{}")

    def test_rejects_unsynced_clock_and_future(self):
        with self.assertRaises(ValueError):
            parse(*msg("valve", ts=12345, state="open", reason="user", detail=""))
        with self.assertRaises(ValueError):
            parse(*msg("alert", ts=NOW + 3 * 86400, detail=""))

    def test_rejects_bad_values(self):
        with self.assertRaises(ValueError):
            parse(*msg("flow", start=NOW, stop=NOW - 5, pulses=10, closed=False))
        with self.assertRaises(ValueError):
            parse(*msg("flow", start=NOW - 5, stop=NOW, pulses=-1, closed=False))
        with self.assertRaises(ValueError):
            parse(*msg("flow", start=NOW - 5, stop=NOW, pulses=True, closed=False))
        with self.assertRaises(ValueError):
            parse(*msg("valve", ts=NOW, state="ajar", reason="user", detail=""))
        with self.assertRaises(ValueError):
            parse(f"water/{DEV}/sample", b"not json")
        with self.assertRaises(ValueError):
            parse(f"water/{DEV}/sample", b"[1, 2]")


if __name__ == "__main__":
    unittest.main()
