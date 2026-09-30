"""Unit tests (no database): uv run --with-requirements server/model/requirements.txt python -m unittest server/model/test_model.py"""
import json
import pathlib
import sys
import unittest

import numpy as np
import pandas as pd

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from anomaly import _pick, inject, night_counts  # noqa: E402
from features import (SPLITS, TZ, WRAP_L, age_weights, calendar, clean_flows, hourly_frame, reconcile_factors,  # noqa: E402
                      spread_hourly, split_of, unknown_hours)
from invoices import parse_text  # noqa: E402
from predict import parse_duration, parse_when  # noqa: E402
from publish import build_config, guard  # noqa: E402
from score import hour_alarms  # noqa: E402
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
        c = clean_flows(f, readings=None)
        self.assertTrue(c["wrap_fixed"].iloc[0])
        self.assertAlmostEqual(c["liters"].iloc[0], 100 - WRAP_L)

    def test_plausible_and_new_firmware_flows_untouched(self):
        legacy = clean_flows(flows(("2025-05-06 10:00", "2025-05-06 10:20", 300), device="bigquery"), readings=None)
        new = clean_flows(flows(("2025-05-06 10:00", "2025-05-06 10:02", 100)), readings=None)
        self.assertEqual(legacy["liters"].iloc[0], 300)
        self.assertEqual(new["liters"].iloc[0], 100)
        self.assertFalse(legacy["wrap_fixed"].iloc[0] or new["wrap_fixed"].iloc[0])


class MeterTest(unittest.TestCase):
    def readings(self):
        return pd.DataFrame({"start": [local("2025-01-01"), local("2025-02-01"), local("2025-03-01")],
                             "end": [local("2025-02-01"), local("2025-03-01"), local("2025-04-01")],
                             "liters": [1000.0, 1000.0, 1000.0]})

    def test_legacy_volumes_scaled_to_the_meter(self):
        f = flows(("2025-01-10 10:00", "2025-01-10 11:00", 800), ("2025-02-10 10:00", "2025-02-10 11:00", 800),
                  ("2025-03-10 10:00", "2025-03-10 11:00", 800), device="bigquery")
        c = clean_flows(f, readings=self.readings())
        self.assertTrue(np.allclose(c["liters"], 1000))           # 0.8 m³ counted, 1 m³ on the meter
        self.assertTrue(np.allclose(c["meter_factor"], 1.25))

    def test_factor_is_smoothed_over_neighbours(self):
        f = flows(("2025-01-10 10:00", "2025-01-10 11:00", 1000), ("2025-02-10 10:00", "2025-02-10 11:00", 800),
                  ("2025-03-10 10:00", "2025-03-10 11:00", 1000), device="bigquery")
        k = reconcile_factors(f, self.readings())
        self.assertAlmostEqual(k["ratio"][1], 1.25)
        self.assertAlmostEqual(k["factor"][1], 3000 / 2800)

    def test_new_firmware_and_unmetered_flows_untouched(self):
        new = clean_flows(flows(("2025-01-10 10:00", "2025-01-10 10:10", 8000)), readings=self.readings())
        old = clean_flows(flows(("2024-06-10 10:00", "2024-06-10 10:10", 50), device="bigquery"),
                          readings=self.readings())
        self.assertEqual((new["liters"].iloc[0], old["liters"].iloc[0]), (8000, 50))

    def test_invoice_rows_parsed_without_anything_else(self):
        # Made-up invoice text in the three wordings seen (and a sub-meter row that must be skipped)
        text = ("Faktura nr X Jan Kowalski, ul. Przykładowa 1 Odczyt radiowy 11111111   - odcz. poprz.: 2031-01-02 "
                "  100.00 m3, bież.: 2031-02-01   111.00 m3, zużycie: 11.00 Sprzedaż wody 11 m3 99,99 "
                "Odczyt z licznika 22222 (podlicznik)  - odcz. poprz.: 2031-01-02   7.00 m3, bież.: 2031-02-01   8.00 m3 "
                "Odczyt radiowy 11111111    Odcz.poprz: 2030-11-03     80.0m3  Bieżący: 2030-12-02    90.0m3 "
                "odcz. z licznika nadrzędnego nr 11111111: poprz. 2031-02-01 111.00 m3, bież. 2031-03-03 120.00 m3")
        self.assertEqual(parse_text(text), {("2031-01-02", 100.0, "2031-02-01", 111.0),
                                            ("2030-11-03", 80.0, "2030-12-02", 90.0),
                                            ("2031-02-01", 111.0, "2031-03-03", 120.0)})


class WeightTest(unittest.TestCase):
    def test_age_bands(self):
        end = local("2026-09-29")
        ts = pd.DatetimeIndex([end - pd.Timedelta(days=d) for d in (10, 700, 800, 1200, 1500, 1900, 2100)])
        self.assertEqual(list(age_weights(ts, end, 0.5)), [1, 1, 0.5, 0.25, 0.125, 0, 0])
        self.assertEqual(list(age_weights(ts, end, 0.0)), [1, 1, 0, 0, 0, 0, 0])         # last 2 years only
        self.assertEqual(list(age_weights(ts, end, 1.0, max_years=None)), [1] * 7)      # unweighted reference


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
        real = clean_flows(flows(("2025-05-06 03:10", "2025-05-06 03:15", 5)), readings=None)
        s = local("2025-05-06 03:15:03")  # 3 s after the real flow: the firmware sees one flow
        merged, absorbed = inject(real, [(s, s + pd.Timedelta(minutes=15), 90.0)])
        self.assertEqual(len(merged), 1)
        self.assertEqual(absorbed, [0])
        self.assertAlmostEqual(merged[0]["liters"], 95.0)
        self.assertEqual(merged[0]["stop"] - merged[0]["start"], pd.Timedelta(minutes=20, seconds=3))

    def test_separate_flow_stays_separate(self):
        real = clean_flows(flows(("2025-05-06 03:10", "2025-05-06 03:15", 5)), readings=None)
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


def fake_thresholds(dur=600, night_dur=180):
    table = []
    for p in ("mon", "tue", "wed", "thu", "fri", "sat", "sun"):
        for h in range(24):
            table.append({"profile": p, "hour": h, "alarm_flow_duration_s": night_dur if h < 6 else dur,
                          "alarm_flow_liters": 20.0 if h < 6 else 400.4, "liters_mean": 12.34, "liters_p90": 30.0,
                          "alarm_hour_liters": 50.0 if h < 6 else 300.0})
    table += [{"profile": "holiday", "hour": h, "alarm_flow_duration_s": dur, "alarm_flow_liters": 400,
               "liters_mean": 10, "liters_p90": 25, "alarm_hour_liters": 200.0} for h in range(24)]
    return {"generated": "2026-09-29 23:00:00", "choice": {"night_flows": {"threshold": 10}}, "table": table}


class PublishTest(unittest.TestCase):
    def test_config_matches_the_firmware_format(self):
        c = build_config(fake_thresholds(), "2026-09-29")
        self.assertEqual(c["v"], 1)
        for key in ("dur_s", "vol_l", "exp_l", "p90_l"):
            self.assertEqual(len(c[key]), 168)
            self.assertTrue(all(isinstance(v, int) for v in c[key]))
        self.assertEqual(c["dur_s"][3], 180)          # Monday 03:00
        self.assertEqual(c["vol_l"][24 + 12], 400)    # Tuesday 12:00, rounded
        self.assertEqual(c["exp_l"][0], 123)          # liters x10
        self.assertEqual(c["night_flows"], 10)
        self.assertLess(len(json.dumps(c, separators=(",", ":"))), 4096)   # firmware LEARNED_JSON_MAX

    def test_guard(self):
        good = {"test": {"fa_total_per_month": 0.95}}
        self.assertEqual(guard(build_config(fake_thresholds(), "x"), good), [])
        self.assertTrue(guard(build_config(fake_thresholds(night_dur=70), "x"), good))
        self.assertTrue(guard(build_config(fake_thresholds(), "x"), {"test": {"fa_total_per_month": 3.1}}))


class ScoreTest(unittest.TestCase):
    def test_hour_alarms_use_the_right_profile(self):
        t = fake_thresholds()["table"]
        idx = pd.DatetimeIndex([local("2026-09-29 03:00"), local("2026-09-29 14:00"), local("2026-12-25 14:00")])
        liters = pd.Series([60.0, 250.0, 250.0], index=idx)   # night over 50; day under 300; Christmas over 200
        hits = hour_alarms(liters, t)
        self.assertEqual([h[0] for h in hits], [idx[0], idx[2]])
        self.assertEqual(hits[1][2], 200.0)


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
