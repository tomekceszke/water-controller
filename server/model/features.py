"""Hourly series, coverage mask, calendar features and the train/validation/test split.

Everything here is pure pandas on a flows DataFrame (see data.py), so it is unit-tested without the database.
Times: flows carry tz-aware UTC timestamps; the hourly grid is built in UTC (no DST holes or duplicates) and the
calendar features come from Europe/Warsaw local time.
"""
import os
import pathlib

import holidays
import numpy as np
import pandas as pd

TZ = "Europe/Warsaw"
HERE = pathlib.Path(__file__).resolve().parent
# Cache and outputs; on hc-data the service points this at /var/lib/wc-model (the code directory is read-only)
STATE = pathlib.Path(os.environ.get("WC_MODEL_STATE", HERE))
OUT = STATE / "out"

# Firmware 3.2.1+ publishes only flows of at least 0.1 L (FLOW_EVENT_MIN_ML); legacy published everything
# (27 % of its flows are smaller). Flow counts and per-flow models use the same cut on both sides.
FLOW_MIN_L = 0.1

# Legacy firmware counter wrap (docs/INVENTORY.md #6): each wrap added ~32767 pulses, ~80 L at 410 pulses/L. The
# import fixed only flows above the meter maximum (49.5 L/min); firmware 3.x never averages more than ~26 L/min
# (max_lpm peak 26.4), so a legacy flow averaging more carries a wrap: remove the fewest that bring it under.
WRAP_L = 32767 / 410
RATE_MAX = 26.0

# A stretch this long with no flow at all is a telemetry outage (legacy BigQuery gaps), not zero usage.
GAP_UNKNOWN = pd.Timedelta(hours=24)

# Time-ordered split (local dates, end exclusive). No shuffling: hours are autocorrelated.
SPLITS = {
    "train": ("2020-06-19", "2025-01-01"),
    "validation": ("2025-01-01", "2026-01-01"),
    "test": ("2026-01-01", "2026-09-29"),
}

FEATURES_BASE = ["hour", "dow", "holiday", "bridge"]
FEATURES_SEASON = ["doy_sin", "doy_cos"]


def clean_flows(flows):
    """Copy with `liters` wrap-fixed for legacy rows and a `wrap_fixed` flag (volume still uncertain there)."""
    f = flows.copy()
    minutes = ((f["stop_ts"] - f["start_ts"]).dt.total_seconds().clip(lower=1)) / 60
    legacy = f["device"].eq("bigquery") if "device" in f else pd.Series(False, index=f.index)
    over = legacy & (f["liters"] >= WRAP_L) & (f["liters"] / minutes > RATE_MAX)
    wraps = np.ceil((f["liters"] - RATE_MAX * minutes) / WRAP_L).clip(lower=0)
    wraps = np.minimum(wraps, np.floor(f["liters"] / WRAP_L))
    f.loc[over, "liters"] = f.loc[over, "liters"] - wraps[over] * WRAP_L
    f["wrap_fixed"] = over
    return f


def load_known_events(path=None):
    """Known unusual periods (lawn establishment, pool fills, hose failure): kept out of 'normal' training.

    The real list (known_events.csv) holds exact dates of household events and stays out of git; without it the
    example file with month-level placeholder dates is used, which masks the events only approximately.
    """
    if path is None:
        path = HERE / "known_events.csv"
        if not path.exists():
            path = HERE / "known_events.example.csv"
    ev = pd.read_csv(path, comment="#")
    ev["start"] = pd.to_datetime(ev["start_local"]).dt.tz_localize(TZ).dt.tz_convert("UTC")
    ev["end"] = pd.to_datetime(ev["end_local"]).dt.tz_localize(TZ).dt.tz_convert("UTC")
    return ev[["start", "end", "kind", "note"]]


def mark_known(flows, events):
    """Boolean Series: the flow starts inside a known event."""
    known = pd.Series(False, index=flows.index)
    for ev in events.itertuples():
        known |= (flows["start_ts"] >= ev.start) & (flows["start_ts"] < ev.end)
    return known


def calendar(ts_utc):
    """Calendar features for a DatetimeIndex (tz-aware), computed in local time."""
    local = pd.DatetimeIndex(ts_utc).tz_convert(TZ)
    years = range(local.year.min() - 1, local.year.max() + 2)
    pl = holidays.Poland(years=years)
    day = local.tz_localize(None).normalize()
    days = pd.DatetimeIndex(day.unique())
    hol = pd.Series([d in pl for d in days.date], index=days)
    prev_hol = pd.Series([(d - pd.Timedelta(days=1)) in pl for d in days], index=days)
    next_hol = pd.Series([(d + pd.Timedelta(days=1)) in pl for d in days], index=days)
    prev_off = prev_hol | ((days - pd.Timedelta(days=1)).dayofweek >= 5)
    next_off = next_hol | ((days + pd.Timedelta(days=1)).dayofweek >= 5)
    # Bridge day: a working day squeezed between a holiday and a weekend (or another holiday)
    bridge = (days.dayofweek < 5) & ~hol & ((prev_hol & next_off) | (next_hol & prev_off))
    doy = 2 * np.pi * (local.dayofyear - 1) / 365.25
    return pd.DataFrame({
        "hour": local.hour,
        "dow": local.dayofweek,           # 0 = Monday
        "holiday": hol.reindex(day).to_numpy().astype(int),
        "bridge": bridge.reindex(day).to_numpy().astype(int),
        "doy_sin": np.sin(doy),
        "doy_cos": np.cos(doy),
    }, index=pd.DatetimeIndex(ts_utc))


def split_of(ts_utc):
    """Split name per timestamp (None outside all splits)."""
    local = pd.DatetimeIndex(ts_utc).tz_convert(TZ).tz_localize(None)
    out = np.full(len(local), None, dtype=object)
    for name, (a, b) in SPLITS.items():
        out[(local >= pd.Timestamp(a)) & (local < pd.Timestamp(b))] = name
    return out


def spread_hourly(flows, index):
    """Liters per UTC hour, a flow crossing an hour boundary split in proportion to time."""
    liters = pd.Series(0.0, index=index)
    start, stop, vol = flows["start_ts"], flows["stop_ts"], flows["liters"].fillna(0.0)
    h0 = start.dt.floor("h")
    single = stop <= h0 + pd.Timedelta(hours=1)
    liters = liters.add(vol[single].groupby(h0[single]).sum(), fill_value=0.0)
    for s, e, v in zip(start[~single], stop[~single], vol[~single]):
        total = (e - s).total_seconds()
        for h in pd.date_range(s.floor("h"), e, freq="h"):
            part = (min(e, h + pd.Timedelta(hours=1)) - max(s, h)).total_seconds()
            if part > 0 and h in liters.index:
                liters[h] += v * part / total
    return liters.reindex(index, fill_value=0.0)


def unknown_hours(flows, index, gap=GAP_UNKNOWN):
    """Hours with no data at all: inside telemetry gaps longer than `gap`, before the first or after the last flow."""
    unknown = pd.Series(False, index=index)
    f = flows.sort_values("start_ts")
    stops = f["stop_ts"].cummax()
    prev_stop = stops.shift()
    gaps = f["start_ts"] - prev_stop
    for a, b in zip(prev_stop[gaps > gap], f["start_ts"][gaps > gap]):
        unknown[(index >= a.ceil("h")) & (index < b.floor("h"))] = True
    if len(f):
        unknown[index < f["start_ts"].iloc[0].floor("h")] = True
        unknown[index >= stops.iloc[-1].ceil("h")] = True
    return unknown


def unreliable_volume_hours(flows, index):
    """Hours holding a flow whose volume is uncertain (suspect or wrap-fixed legacy rows): counts stay valid."""
    bad = pd.Series(False, index=index)
    f = flows.loc[flows["suspect"].astype(bool) | flows.get("wrap_fixed", False)]
    for s, e in zip(f["start_ts"].dt.floor("h"), f["stop_ts"]):
        bad[(index >= s) & (index < e)] = True
    return bad


def hourly_frame(flows, events, end=None):
    """One row per UTC hour: liters, flows (>= FLOW_MIN_L starting in the hour), masks, split, calendar.

    Masks: `unknown` (no telemetry: both targets invalid), `vol_bad` (liters invalid, count valid),
    `known` (inside a known unusual event: not "normal", kept for evaluating the detector).
    """
    flows = clean_flows(flows)
    start = flows["start_ts"].min().floor("h")
    end = end or flows["stop_ts"].max().ceil("h")
    index = pd.date_range(start, end, freq="h", inclusive="left", tz="UTC")
    counted = flows.loc[flows["liters"] >= FLOW_MIN_L]
    df = calendar(index)
    df["liters"] = spread_hourly(flows, index)
    df["flows"] = counted.groupby(counted["start_ts"].dt.floor("h")).size().reindex(index, fill_value=0)
    df["unknown"] = unknown_hours(flows, index)
    df["vol_bad"] = unreliable_volume_hours(flows, index)
    known = pd.Series(False, index=index)
    for ev in events.itertuples():
        known |= (index >= ev.start.floor("h")) & (index < ev.end)
    df["known"] = known
    df["split"] = split_of(index)
    return df


def flow_frame(flows, events):
    """Per-flow table for the flow-level models: flows >= FLOW_MIN_L with start-time calendar features."""
    f = clean_flows(flows)
    f = f.loc[f["liters"] >= FLOW_MIN_L].copy()
    f["duration_s"] = (f["stop_ts"] - f["start_ts"]).dt.total_seconds().clip(lower=1)
    f["log_dur"] = np.log(f["duration_s"])
    f["log_vol"] = np.log(f["liters"])
    f["rate"] = f["liters"] / (f["duration_s"] / 60)
    f["known"] = mark_known(f, events)
    f["split"] = split_of(f["start_ts"])
    cal = calendar(pd.DatetimeIndex(f["start_ts"]))
    for c in cal.columns:
        f[c] = cal[c].to_numpy()
    return f.reset_index(drop=True)
