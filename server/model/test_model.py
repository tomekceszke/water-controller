"""Unit tests (no database): uv run --with-requirements server/model/requirements.txt python -m unittest server/model/test_model.py"""
import pathlib
import sys
import unittest

import numpy as np
import pandas as pd

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from anomaly import _pick, inject, night_counts  # noqa: E402
from features import (SPLITS, TZ, WRAP_L, calendar, clean_flows, hourly_frame, spread_hourly,  # noqa: E402
                      split_of, unknown_hours)
from predict import parse_duration, parse_when  # noqa: E402
from train import level_factor  # noqa: E402


def local(s):
    return pd.Timestamp(s, tz=TZ).tz_convert("UTC")


def flows(*rows, device="30aea40aba44"):
    """rows: (start local, stop local, liters)."""
    return pd.DataFrame([{"device": device, "start_ts": local(a), "stop_ts": local(b), "liters": float(v),
                          "max_lpm": None, "closed": False, "suspect": False} for a, b, v in rows])


NO_EVENTS = pd.DataFrame(columns=["start", "end", "kind", "note"])


class HourlyTest(unittest.TestCase):
    def test_flow_split_across_hour_boundary(self):
        f = flows(("2025-05-06 10:30", "2025-05-06 11:30", 60))
        idx = pd.date_range(local("2025-05-06 10:00"), periods=2, freq="h")
        self.assertEqual(spread_hourly(f, idx).tolist(), [30.0, 30.0])

    def test_dst_spring_forward(self):
        # 2026-03-29 02:00 local does not exist: 01:30 -> 03:30 local is one real hour
        f = flows(("2026-03-29 01:30", "2026-03-29 03:30", 10))
        idx = pd.date_range(local("2026-03-29 01:00"), periods=2, freq="h")
        self.assertEqual(spread_hourly(f, idx).tolist(), [5.0, 5.0])
        self.assertEqual(calendar(idx)["hour"].tolist(), [1, 3])

    def test_gap_longer_than_a_day_is_unknown(self):
        f = flows(("2025-05-01 08:00", "2025-05-01 08:01", 1), ("2025-05-01 20:00", "2025-05-01 20:01", 1),
                  ("2025-05-04 08:00", "2025-05-04 08:01", 1))
        idx = pd.date_range(local("2025-05-01 08:00"), local("2025-05-04 09:00"), freq="h", inclusive="left")
        u = unknown_hours(f, idx)
        self.assertFalse(u[local("2025-05-01 12:00")])  # 12 h pause: a normal quiet stretch
        self.assertTrue(u[local("2025-05-02 12:00")])   # 2.5 day pause: outage
        self.assertFalse(u[local("2025-05-04 08:00")])

    def test_hourly_frame_counts_only_flows_above_min(self):
        f = flows(("2025-05-06 10:00", "2025-05-06 10:01", 0.05), ("2025-05-06 10:10", "2025-05-06 10:11", 2))
        h = hourly_frame(f, NO_EVENTS)
        row = h.loc[local("2025-05-06 10:00")]
        self.assertEqual(row["flows"], 1)
        self.assertAlmostEqual(row["liters"], 2.05)


class CleanTest(unittest.TestCase):
    def test_legacy_wrap_removed_when_rate_impossible(self):
        f = flows(("2025-05-06 10:00", "2025-05-06 10:02", 100), device="bigquery")
        c = clean_flows(f)
        self.assertTrue(c["wrap_fixed"].iloc[0])
        self.assertAlmostEqual(c["liters"].iloc[0], 100 - WRAP_L)

    def test_plausible_and_new_firmware_flows_untouched(self):
        legacy = clean_flows(flows(("2025-05-06 10:00", "2025-05-06 10:20", 300), device="bigquery"))
        new = clean_flows(flows(("2025-05-06 10:00", "2025-05-06 10:02", 100)))
        self.assertEqual(legacy["liters"].iloc[0], 300)
        self.assertEqual(new["liters"].iloc[0], 100)
        self.assertFalse(legacy["wrap_fixed"].iloc[0] or new["wrap_fixed"].iloc[0])


class SplitTest(unittest.TestCase):
    def test_splits_are_contiguous_and_ordered(self):
        spans = list(SPLITS.values())
        for (_, end), (start, _) in zip(spans, spans[1:]):
            self.assertEqual(end, start)
        self.assertEqual(list(SPLITS), ["train", "validation", "test"])

    def test_boundary_belongs_to_later_split(self):
        idx = pd.DatetimeIndex([local("2024-12-31 23:00"), local("2025-01-01 00:00"), local("2026-01-01 00:00")])
        self.assertEqual(list(split_of(idx)), ["train", "validation", "test"])


class LevelTest(unittest.TestCase):
    def test_factor_follows_actual_over_predicted(self):
        idx = pd.date_range(local("2025-05-01 00:00"), periods=24 * 60, freq="h")
        y, p = pd.Series(2.0, index=idx), pd.Series(1.0, index=idx)
        f = level_factor(y, p, idx, 28)
        self.assertAlmostEqual(f.iloc[-1], 2.0)
        self.assertEqual(f.iloc[0], 1.0)  # nothing before the first day
        self.assertTrue((level_factor(y, p, idx, 0) == 1.0).all())


class AnomalyTest(unittest.TestCase):
    def test_injected_leak_merges_with_adjacent_real_flow(self):
        real = clean_flows(flows(("2025-05-06 03:10", "2025-05-06 03:15", 5)))
        s = local("2025-05-06 03:15:03")  # 3 s after the real flow: the firmware sees one flow
        merged, absorbed = inject(real, [(s, s + pd.Timedelta(minutes=15), 90.0)])
        self.assertEqual(len(merged), 1)
        self.assertEqual(absorbed, [0])
        self.assertAlmostEqual(merged[0]["liters"], 95.0)
        self.assertEqual(merged[0]["stop"] - merged[0]["start"], pd.Timedelta(minutes=20, seconds=3))

    def test_separate_flow_stays_separate(self):
        real = clean_flows(flows(("2025-05-06 03:10", "2025-05-06 03:15", 5)))
        s = local("2025-05-06 03:30")
        merged, absorbed = inject(real, [(s, s + pd.Timedelta(minutes=15), 90.0)])
        self.assertEqual((len(merged), absorbed, merged[0]["liters"]), (1, [], 90.0))

    def test_pick_most_sensitive_within_budget(self):
        v = np.array([0.1, 0.5, 1.0, 5.0])  # log scale; margins go up to log(8) = 2.08
        a, m, n = _pick(v, {0.99: np.zeros(4)}, "upper", allowed=1)
        self.assertEqual(n, 1)
        self.assertTrue(1.0 <= m < 1.1)
        _, m, n = _pick(v, {0.99: np.zeros(4)}, "upper", allowed=0)  # unreachable: widest margin
        self.assertEqual((round(m, 2), n), (2.08, 1))

    def test_night_counts_skip_incomplete_nights(self):
        idx = pd.date_range(local("2025-05-06 00:00"), periods=48, freq="h")
        df = calendar(idx).assign(flows=1, unknown=False, known=False)
        df.loc[local("2025-05-07 02:00"), "unknown"] = True
        c = night_counts(df)
        self.assertEqual(c.to_dict(), {pd.Timestamp("2025-05-06"): 6})


class PredictTest(unittest.TestCase):
    def test_weekday_means_next_occurrence(self):
        now = pd.Timestamp("2026-09-29 22:00", tz=TZ)  # Tuesday
        self.assertEqual(parse_when("thu 17:00", now), pd.Timestamp("2026-10-01 17:00", tz=TZ))
        self.assertEqual(parse_when("tue 3", now), pd.Timestamp("2026-10-06 03:00", tz=TZ))
        self.assertEqual(parse_when("2026-10-01 17:00", now), pd.Timestamp("2026-10-01 17:00", tz=TZ))

    def test_durations(self):
        self.assertEqual([parse_duration(x) for x in ("15m", "90s", "1h30m", "42")], [900, 90, 5400, 42])


if __name__ == "__main__":
    unittest.main()
