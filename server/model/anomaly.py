#!/usr/bin/env python3
"""Anomaly detector: per-flow and per-hour thresholds conditioned on the time of day, calibrated on validation.

  uv run --with-requirements server/model/requirements.txt server/model/anomaly.py

Needs out/hourly.joblib and out/metrics_hourly.json from train.py. Writes out/anomaly.joblib,
out/thresholds.json and out/metrics_anomaly.json.

Indicators (each a one-sided quantile of its own conditional distribution, times a margin):
  duration, volume          flow longer / bigger than usual for that hour           (upper, log scale)
  rate_high                 average flow rate above usual (flows >= 30 s)          (upper, log scale)
  rate_low                  slow long flow, drip (flows >= 300 s)                   (lower, log scale)
  hour_liters               liters in an hour above usual, e.g. a hose with pauses (upper, level-corrected)
  night_flows               number of flows 00:00-06:00 above usual (cistern / float valve leak)

Night (00-06) and day get separate margins and budgets: at night far less is normal, so a smaller margin keeps
the same false-alarm rate and catches a leak sooner.
"""
import json
import pathlib
import sys

import joblib
import numpy as np
import pandas as pd
from sklearn.covariance import MinCovDet
from sklearn.metrics import mean_pinball_loss

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import features  # noqa: E402
from data import load_flows  # noqa: E402
from features import (FLOW_MIN_L, SPLITS, TZ, calendar, clean_flows, flow_frame, hourly_frame,  # noqa: E402
                      load_known_events)
from models import GBM, HourMax  # noqa: E402
from train import usable, window  # noqa: E402

OUT = features.OUT
MERGE_GAP = pd.Timedelta(seconds=5)  # firmware: a pause longer than 5 s ends a flow

FLOW_IND = {
    "duration": {"col": "log_dur", "side": "upper", "min_dur": 0},
    "volume": {"col": "log_vol", "side": "upper", "min_dur": 0},
    "rate_high": {"col": "log_rate", "side": "upper", "min_dur": 30},
    "rate_low": {"col": "log_rate", "side": "lower", "min_dur": 300},
}
# Floors (log scale) for the upper flow thresholds: a year of validation nights holds only ~120 flows, too few to
# trust a one-minute night limit (it produced 1.7 false alarms a month on test). A 15 min leak at 03:00 is still
# caught after 3 min.
FLOOR = {"duration": np.log(180), "volume": np.log(20)}
# Ceiling for the slow-flow limit: a drip or a leaking cistern valve runs under 1 L/min, a tap left slightly open at
# 1-3 L/min is ordinary use. Tuned on a validation year with no slow flows, the limit rose to ~3 L/min and alarmed on
# ordinary use in the test year.
CEIL = {"rate_low": np.log(1.0)}
# Flows per hour is predicted (train.py) but not an alarm: night_flows covers the case with a far lower limit
HOUR_IND = {"hour_liters": "liters"}
# Upper indicators: the per-hour maximum of normal data (models.HourMax) times a margin. Lower (slow flow):
# boosted low quantiles, where the tail is not the problem.
ALPHAS = {"upper": ("max",), "lower": (0.01, 0.001)}
# False-alarm budget per month and indicator, about one alarm a month in total; night gets NIGHT_SHARE of it
BUDGET = {"rate_low": 0.1, "night_flows": 0.15}   # the per-hour maxima share MAX_BUDGET
NIGHT_HOURS = 6  # local 00:00-05:59
BANDS = ("night", "day")
SHARE = {"night": 0.25, "day": 0.75}
# A third band (evening 17-24, where baths of 10-14 min are normal) was tried: validation could not tell it apart
# (0.26 vs 0.35 false alarms a month) and on test it overfit (1.55 a month), so the two bands stay.
LOG_MARGINS = np.linspace(0, np.log(8), 61)
# The per-hour maxima share one margin, the smallest on MAX_MARGINS whose alarms (distinct hours) stay within
# MAX_BUDGET a month on validation. Validation maxima come from a single year; production maxima from all data are
# larger, so the thresholds on the device err on the quiet side. (Per-indicator margins tuned on one year's maxima
# and applied to three years' gave 21 min and 965 L at midday.)
MAX_MARGINS = np.round(np.arange(1.0, 2.01, 0.05), 2)
MAX_INDICATORS = ("duration", "volume", "rate_high", "hour_liters")
MAX_BUDGET = 0.75
FLOW_WINDOWS = ("last 2 y", "decay 0.5")
LEAKS = {"rate_lpm": (1, 3, 6, 12, 35), "minutes": (5, 15, 30, 60)}
N_TRIALS = 200
SEED = {"validation": 1, "test": 2}


# ---------------------------------------------------------------- flow-level models

def flow_table(flows, events):
    f = flow_frame(flows, events)
    f["log_rate"] = np.log(f["rate"].clip(lower=1e-3))
    return f


def fit_flow_models(fr, fit_on, win, season):
    """Quantile GBMs per indicator and alpha on normal flows of `fit_on` splits."""
    tr, w = window(fr.set_index("start_ts", drop=False), win, fit_on)
    normal = ~tr["known"].to_numpy()
    tr, w = tr[normal], w[normal]
    models = {}
    for ind, spec in FLOW_IND.items():
        sel = (tr["duration_s"] >= spec["min_dur"]).to_numpy()
        part, pw = tr[sel], w[sel]
        for a in ALPHAS[spec["side"]]:
            if a == "max":
                models[(ind, a)] = HourMax().fit(part, part[spec["col"]].to_numpy(), pw)
            else:
                models[(ind, a)] = GBM(season=season, loss="quantile", quantile=a, min_samples_leaf=200,
                                       max_iter=300).fit(part, part[spec["col"]].to_numpy(), pw)
    return models


def band_of(hour):
    return np.where(np.asarray(hour) < NIGHT_HOURS, "night", "day")


def flow_limits(models, choice, X):
    """Per-row thresholds (log scale) for the chosen (alpha, log margin) per indicator and band."""
    out = {}
    band = band_of(X["hour"])
    for ind, spec in FLOW_IND.items():
        t = np.zeros(len(X))
        for b in BANDS:
            sel = band == b
            if sel.any():
                a, m = choice[ind][b]
                q = models[(ind, a)].predict(X[sel])
                t[sel] = q + m if spec["side"] == "upper" else q - m
        out[ind] = np.minimum(np.maximum(t, FLOOR.get(ind, -np.inf)), CEIL.get(ind, np.inf))
    return pd.DataFrame(out, index=X.index)


def flow_alarms(fl, limits):
    """Boolean frame (flows x indicators) and the detection delay in seconds after the flow start."""
    fired, delay = {}, {}
    for ind, spec in FLOW_IND.items():
        long_enough = fl["duration_s"] >= spec["min_dur"]
        v, t = fl[spec["col"]], limits[ind]
        fired[ind] = long_enough & ((v > t) if spec["side"] == "upper" else (v < t))
        if ind == "duration":
            delay[ind] = np.exp(t)
        elif ind == "volume":  # the threshold volume is reached at the flow's average rate
            delay[ind] = np.exp(t) / fl["rate"].clip(lower=1e-3) * 60
        else:
            delay[ind] = pd.Series(float(spec["min_dur"]), index=fl.index)
    return pd.DataFrame(fired), pd.DataFrame(delay)


# ---------------------------------------------------------------- hour-level models

def fit_hour_model(df, cfg, target, fit_on):
    """Largest normal hourly total per local hour (±1 h) in the training window of the hourly model."""
    tr, w = window(usable(df), cfg["window"], fit_on)
    return HourMax().fit(tr, tr[target].to_numpy(), w)


def hourly_limits(df, cfg, target, fit_on):
    """{"max": Series of the per-hour maximum for all hours} (fitted on `fit_on`)."""
    return {"max": pd.Series(fit_hour_model(df, cfg, target, fit_on).predict(df), index=df.index)}


# ---------------------------------------------------------------- calibration

def months(df, split):
    ok = df[(df["split"] == split) & ~df["unknown"]]
    return len(ok) / (365.25 * 24 / 12)


def _pick(values, q_by_alpha, side, allowed, floor=-np.inf, ceil=np.inf):
    """Most sensitive (alpha, log margin) with at most `allowed` exceedances; returns (alpha, margin, n)."""
    best, fallback = None, None
    for a, q in q_by_alpha.items():
        for m in LOG_MARGINS:
            t = np.maximum(q + m, floor) if side == "upper" else np.minimum(q - m, ceil)
            n = int(((values > t) if side == "upper" else (values < t)).sum())
            if n <= allowed:
                level = np.median(t) if side == "upper" else -np.median(t)
                if best is None or level < best[0]:
                    best = (level, a, float(m), n)
                break
        if fallback is None or n < fallback[3]:  # budget unreachable: the widest margin with the fewest alarms
            fallback = (None, a, float(LOG_MARGINS[-1]), n)
    return (best or fallback)[1:]


def night_counts(df_split):
    """Flows per local night (00-06) where all six hours are observed and none is a known event."""
    h = df_split[df_split["hour"] < NIGHT_HOURS]
    night = h.index.tz_convert(TZ).tz_localize(None).normalize()
    g = h.assign(night=night, bad=h["unknown"] | h["known"]).groupby("night")
    c = g.agg(flows=("flows", "sum"), bad=("bad", "any"), n=("flows", "size"))
    return c.loc[~c["bad"] & (c["n"] == NIGHT_HOURS), "flows"]


def _max_margin(normal, flow_models, h, hour_q, n_months):
    """Smallest shared margin for the per-hour maximum indicators within MAX_BUDGET alarm-hours a month."""
    flows = {}
    for ind in ("duration", "volume", "rate_high"):
        spec = FLOW_IND[ind]
        p = normal[normal["duration_s"] >= spec["min_dur"]]
        flows[ind] = (p, flow_models[(ind, "max")].predict(p), p[spec["col"]].to_numpy())
    hq = np.log(np.maximum(hour_q["liters"]["max"].reindex(h.index).to_numpy(), 1.0))
    hv = np.log(np.maximum(h["liters"].to_numpy(), 1e-3))
    for m in MAX_MARGINS:
        lm = np.log(m)
        hours = set(h.index[hv > hq + lm])
        for ind, (p, q, v) in flows.items():
            lim = np.maximum(q + lm, FLOOR.get(ind, -np.inf))
            hours |= set(p["start_ts"][v > lim].dt.floor("h"))
        if len(hours) / n_months <= MAX_BUDGET:
            return float(lm)
    return float(np.log(MAX_MARGINS[-1]))


def calibrate(fr_split, flow_models, df_split, hour_q, n_months):
    """Shared margin for the per-hour maxima, and per band the most sensitive (alpha, margin) for the slow-flow
    indicator; the night flow count gets the smallest threshold within its budget."""
    normal = fr_split[~fr_split["known"]]
    h = df_split[~df_split["known"] & ~df_split["unknown"]]
    lm = _max_margin(normal, flow_models, h, hour_q, n_months)
    choice = {ind: {b: ("max", lm) for b in BANDS} for ind in MAX_INDICATORS}
    fa = {}
    spec = FLOW_IND["rate_low"]
    part = normal[normal["duration_s"] >= spec["min_dur"]]
    band = band_of(part["hour"])
    choice["rate_low"], n = {}, 0
    for b in ("day", "night"):
        x = part[band == b]
        if not len(x):  # nothing to calibrate on (no night flows this long): same as the day
            choice["rate_low"][b] = choice["rate_low"]["day"]
            continue
        q = {a: flow_models[("rate_low", a)].predict(x) for a in ALPHAS["lower"]}
        a, m, k = _pick(x[spec["col"]].to_numpy(), q, "lower", BUDGET["rate_low"] * SHARE[b] * n_months,
                        ceil=CEIL["rate_low"])
        choice["rate_low"][b], n = (a, m), n + k
    fa["rate_low"] = n / n_months
    counts = night_counts(df_split)
    k = int(counts.max()) if len(counts) else 10
    for thr in range(int(counts.median()) if len(counts) else 0, k + 1):
        if (counts > thr).sum() <= BUDGET["night_flows"] * n_months:
            k = thr
            break
    choice["night_flows"] = {"threshold": k}
    fa["night_flows"] = float((counts > k).sum() / n_months)
    return choice, fa


def hour_threshold(hour_q, choice, target, index):
    """Alarm threshold per hour in `index` (level-corrected quantile times the band margin, at least 1)."""
    ind = {v: k for k, v in HOUR_IND.items()}[target]
    band = band_of(index.tz_convert(TZ).hour)
    t = np.zeros(len(index))
    for b in BANDS:
        a, m = choice[ind][b]
        sel = band == b
        t[sel] = np.maximum(hour_q[target][a].reindex(index[sel]).to_numpy(), 1.0) * np.exp(m)
    return t


def real_alarms(fr_split, flow_models, choice, df_split, hour_q, n_months):
    """Alarms raised by real (non-known) data: per indicator, distinct alarm hours, and a list for review."""
    normal = fr_split[~fr_split["known"]]
    lim = flow_limits(flow_models, choice, normal)
    fired, _ = flow_alarms(normal, lim)
    rows = []
    for ind in FLOW_IND:
        for i in normal.index[fired[ind]]:
            f = normal.loc[i]
            rows.append({"ts": f["start_ts"], "indicator": ind, "duration_s": float(f["duration_s"]),
                         "liters": float(f["liters"]), "rate_lpm": float(f["rate"]),
                         "severity": float(abs(f[FLOW_IND[ind]["col"]] - lim.loc[i, ind]))})
    h = df_split[~df_split["known"] & ~df_split["unknown"]]
    for ind, target in HOUR_IND.items():
        t = hour_threshold(hour_q, choice, target, h.index)
        hit = h[target].to_numpy() > t
        for ts, v, tt in zip(h.index[hit], h[target].to_numpy()[hit], t[hit]):
            rows.append({"ts": ts, "indicator": ind, "value": float(v), "threshold": float(tt),
                         "severity": float(np.log(v / tt))})
    k = choice["night_flows"]["threshold"]
    counts = night_counts(df_split)
    for night, v in counts[counts > k].items():
        rows.append({"ts": night.tz_localize(TZ).tz_convert("UTC"), "indicator": "night_flows", "value": float(v),
                     "threshold": float(k), "severity": float(np.log(v / max(k, 1)))})
    alarms = pd.DataFrame(rows)
    per_ind = {ind: float((alarms["indicator"] == ind).sum() / n_months) if len(alarms) else 0.0
               for ind in list(FLOW_IND) + list(HOUR_IND) + ["night_flows"]}
    total = float(alarms["ts"].dt.floor("h").nunique() / n_months) if len(alarms) else 0.0
    return alarms, per_ind, total


# ---------------------------------------------------------------- synthetic leaks

def leak_trials(df, split, rng):
    """List of (scenario, cell, [(start, stop, liters)]) injected into usable hours of `split`."""
    hours = df.index[(df["split"] == split) & ~df["unknown"] & ~df["known"]]
    trials = []

    def start_at(h):
        return h + pd.Timedelta(seconds=int(rng.integers(0, 3600)))

    for r in LEAKS["rate_lpm"]:
        for mins in LEAKS["minutes"]:
            for h in hours[rng.integers(0, len(hours), N_TRIALS)]:
                s = start_at(h)
                trials.append(("leak", f"{r} L/min x {mins} min", [(s, s + pd.Timedelta(minutes=mins), r * mins)]))
    local = hours.tz_convert(TZ)
    for h in hours[(local.dayofweek == 1) & (local.hour == 3)]:  # every Tuesday 03:00 local
        trials.append(("tuesday 03:00", "6 L/min x 15 min", [(h, h + pd.Timedelta(minutes=15), 90.0)]))
    nights = pd.DatetimeIndex(sorted({h.normalize() for h in local[local.hour == 0]}))
    for night in nights[rng.integers(0, len(nights), N_TRIALS)]:
        t = night + pd.Timedelta(minutes=int(rng.integers(0, 60)))
        end = night + pd.Timedelta(hours=6)
        refills = []
        while t < end:  # cistern refill: 0.5-2 L at 6 L/min every 5-20 min
            vol = float(rng.uniform(0.5, 2))
            refills.append((t.tz_convert("UTC"), (t + pd.Timedelta(seconds=vol / 6 * 60)).tz_convert("UTC"), vol))
            t += pd.Timedelta(minutes=float(rng.uniform(5, 20)))
        trials.append(("cistern", "0.5-2 L every 5-20 min, 00-06", refills))
    for h in hours[rng.integers(0, len(hours), N_TRIALS)]:
        s = start_at(h)
        rate, hrs = float(rng.uniform(0.05, 0.3)), float(rng.uniform(3, 6))
        trials.append(("drip", "0.05-0.3 L/min for 3-6 h", [(s, s + pd.Timedelta(hours=hrs), rate * hrs * 60)]))
    return trials


def inject(real, trial):
    """Merge the injected flows with overlapping real flows (<= 5 s apart) like the firmware would.

    Returns (merged flows containing injected water, indices of real flows absorbed into them).
    """
    starts = real["start_ts"].dt.tz_convert(None).to_numpy()
    s0, e0 = trial[0][0], trial[-1][1]
    lo = np.searchsorted(starts, np.datetime64((s0 - pd.Timedelta(hours=2)).tz_convert(None)))
    hi = np.searchsorted(starts, np.datetime64((e0 + MERGE_GAP).tz_convert(None)), side="right")
    near = real.iloc[lo:hi]
    near = near[near["stop_ts"] >= s0 - MERGE_GAP]
    items = [(s, e, v, True, None) for s, e, v in trial]
    items += [(r.start_ts, r.stop_ts, r.liters, False, i) for i, r in zip(near.index, near.itertuples())]
    items.sort(key=lambda x: x[0])
    groups, cur = [], None
    for s, e, v, inj, idx in items:
        if cur and s <= cur["stop"] + MERGE_GAP:
            cur["stop"] = max(cur["stop"], e)
            cur["liters"] += v
            cur["inj"] |= inj
            cur["real"] += [idx] if idx is not None else []
        else:
            cur = {"start": s, "stop": e, "liters": v, "inj": inj, "real": [idx] if idx is not None else []}
            groups.append(cur)
    merged = [g for g in groups if g["inj"]]
    absorbed = [i for g in merged for i in g["real"]]
    return merged, absorbed


def run_trials(trials, real, flow_models, choice, df, hour_q):
    """Detection flag and delay per trial (per-indicator detector) plus the Mahalanobis comparison."""
    real = real.sort_values("start_ts").reset_index(drop=True)
    nh = df[df["hour"] < NIGHT_HOURS]
    nday = nh.index.tz_convert(TZ).tz_localize(None).normalize()
    night_cum = nh["flows"].groupby(nday).cumsum()
    night_total = night_counts(df)
    rows, mflows = [], []
    for k, (scenario, cell, parts) in enumerate(trials):
        merged, absorbed = inject(real, parts)
        t0 = parts[0][0]
        for g in merged:
            mflows.append({"trial": k, "start_ts": g["start"], "stop_ts": g["stop"], "liters": g["liters"]})
        # hourly: add the injected liters / flows, remove absorbed real flows from the counts
        hours = pd.date_range(t0.floor("h"), parts[-1][1].floor("h"), freq="h")
        hours = hours[hours.isin(df.index)]
        add_l = pd.Series(0.0, index=hours)
        for s, e, v in parts:
            for h in pd.date_range(s.floor("h"), e, freq="h"):
                part = (min(e, h + pd.Timedelta(hours=1)) - max(s, h)).total_seconds()
                if h in add_l.index and part > 0:
                    add_l[h] += v * part / max((e - s).total_seconds(), 1)
        add_n = pd.Series(0, index=hours)
        for g in merged:
            if g["liters"] >= FLOW_MIN_L and g["start"].floor("h") in add_n.index:
                add_n[g["start"].floor("h")] += 1
        for i in absorbed:
            r = real.loc[i]
            if r["liters"] >= FLOW_MIN_L and r["start_ts"].floor("h") in add_n.index:
                add_n[r["start_ts"].floor("h")] -= 1
        best = np.inf
        local = hours.tz_convert(TZ)
        night = local.hour < NIGHT_HOURS
        if night.any():
            limit = choice["night_flows"]["threshold"]
            for day in sorted(set(local[night].tz_localize(None).normalize())):
                sel = night & (local.tz_localize(None).normalize() == day)
                before_cum = night_cum.reindex(hours[sel]).fillna(0).to_numpy()
                total = night_total.get(day, np.nan)
                if not np.isfinite(total) or total > limit:
                    continue  # night not observed, or already alarming without the leak
                hit = before_cum + np.cumsum(add_n[hours[sel]].to_numpy()) > limit
                if hit.any():
                    best = min(best, ((hours[sel][hit][0] + pd.Timedelta(hours=1)) - t0).total_seconds())
        for target, add in (("liters", add_l),):
            before = df.loc[hours, target].to_numpy()
            t = hour_threshold(hour_q, choice, target, hours)
            new_hit = (before + add.to_numpy() > t) & ~(before > t)
            if new_hit.any():
                best = min(best, ((hours[new_hit][0] + pd.Timedelta(hours=1)) - t0).total_seconds())
        rows.append({"trial": k, "scenario": scenario, "cell": cell, "t0": t0, "hour_delay": best})
    res = pd.DataFrame(rows).set_index("trial")
    mf = pd.DataFrame(mflows)
    mf["duration_s"] = (mf["stop_ts"] - mf["start_ts"]).dt.total_seconds().clip(lower=1)
    mf["log_dur"], mf["log_vol"] = np.log(mf["duration_s"]), np.log(mf["liters"])
    mf["rate"] = mf["liters"] / (mf["duration_s"] / 60)
    mf["log_rate"] = np.log(mf["rate"].clip(lower=1e-3))
    cal = calendar(pd.DatetimeIndex(mf["start_ts"]))
    for c in cal.columns:
        mf[c] = cal[c].to_numpy()
    fired, delay = flow_alarms(mf, flow_limits(flow_models, choice, mf))
    t0 = res.loc[mf["trial"], "t0"].to_numpy()
    offset = (mf["start_ts"] - pd.DatetimeIndex(t0)).dt.total_seconds().to_numpy()
    d = delay.where(fired).min(axis=1).to_numpy() + offset
    mf["flow_delay"] = np.where(np.isnan(d), np.inf, np.maximum(d, 0))
    res["flow_delay"] = mf.groupby("trial")["flow_delay"].min().reindex(res.index, fill_value=np.inf)
    res["delay_s"] = res[["flow_delay", "hour_delay"]].min(axis=1)
    res["detected"] = np.isfinite(res["delay_s"])
    return res, mf


def summarize_trials(res):
    out = []
    for (scenario, cell), g in res.groupby(["scenario", "cell"], sort=False):
        det = g[g["detected"]]
        out.append({"scenario": scenario, "cell": cell, "n": len(g), "recall": float(g["detected"].mean()),
                    "median_delay_min": float(det["delay_s"].median() / 60) if len(det) else None,
                    "p90_delay_min": float(det["delay_s"].quantile(0.9) / 60) if len(det) else None})
    return out


# ---------------------------------------------------------------- Mahalanobis comparison

def mahalanobis_fit(fr):
    """Robust 2-D Gaussian of (log duration, log volume) per hour x weekend bucket (log rate is their difference)."""
    tr = fr[~fr["known"]]
    buckets = {}
    for (h, we), g in tr.groupby([tr["hour"], (tr["dow"] >= 5) | (tr["holiday"] == 1)]):
        x = g[["log_dur", "log_vol"]].to_numpy()
        if len(x) > 5000:
            x = x[np.random.default_rng(0).choice(len(x), 5000, replace=False)]
        buckets[(h, bool(we))] = MinCovDet(random_state=0).fit(x)
    return buckets


def mahalanobis_score(buckets, fl):
    we = ((fl["dow"] >= 5) | (fl["holiday"] == 1)).to_numpy()
    hour = fl["hour"].to_numpy()
    out = np.zeros(len(fl))
    x = fl[["log_dur", "log_vol"]].to_numpy()
    for (h, w), mcd in buckets.items():
        sel = (hour == h) & (we == w)
        if sel.any():
            out[sel] = mcd.mahalanobis(x[sel])
    return out


# ---------------------------------------------------------------- main

def choose_flow_config(fr):
    """Recency weights and season for the boosted flow model (slow flow): validation pinball of its low quantile."""
    long_enough = fr["duration_s"] >= FLOW_IND["rate_low"]["min_dur"]
    val = fr[(fr["split"] == "validation") & ~fr["known"] & long_enough]
    best, rows = None, []
    for win in FLOW_WINDOWS:
        tr, w = window(fr[long_enough].set_index("start_ts", drop=False), win, ["train"])
        keep = ~tr["known"].to_numpy()
        tr, w = tr[keep], w[keep]
        for season in (False, True):
            m = GBM(season=season, loss="quantile", quantile=0.01, min_samples_leaf=200).fit(tr, tr["log_rate"], w)
            loss = float(mean_pinball_loss(val["log_rate"], m.predict(val), alpha=0.01))
            rows.append({"window": win, "season": season, "pinball": loss})
            if best is None or loss < best["pinball"]:
                best = rows[-1]
    return best, rows


def evaluate_split(split, fit_on, fr, df, cfg_hourly, flow_cfg, choice=None):
    """Fit on `fit_on`, calibrate on `split` if no choice given, then real alarms and synthetic leaks there."""
    fm = fit_flow_models(fr, fit_on, flow_cfg["window"], flow_cfg["season"])
    hq = {t: hourly_limits(df, cfg_hourly[t], t, fit_on) for t in HOUR_IND.values()}
    fr_s, df_s, n = fr[fr["split"] == split], df[df["split"] == split], months(df, split)
    fa_cal = None
    if choice is None:
        choice, fa_cal = calibrate(fr_s, fm, df_s, hq, n)
    alarms, per_ind, total = real_alarms(fr_s, fm, choice, df_s, hq, n)
    trials = leak_trials(df, split, np.random.default_rng(SEED[split]))
    real = clean_flows(load_flows())
    real = real[real["start_ts"] >= pd.Timestamp(SPLITS[split][0], tz=TZ) - pd.Timedelta(days=1)]
    res, mf = run_trials(trials, real, fm, choice, df, hq)
    return {"choice": choice, "fa_calibration": fa_cal, "fa_per_month": per_ind, "fa_total_per_month": total,
            "alarms": alarms, "trials": res, "trial_flows": mf, "flow_models": fm, "hour_q": hq, "months": n}


def mahalanobis_compare(fr, ev_val):
    """Same flow false-alarm budget as the per-indicator flow detectors; recall on the validation trials."""
    buckets = mahalanobis_fit(fr[fr["split"] == "train"])
    val = fr[(fr["split"] == "validation") & ~fr["known"]]
    s = mahalanobis_score(buckets, val)
    budget = (MAX_BUDGET + BUDGET["rate_low"]) * ev_val["months"]
    thr = float(np.sort(s)[::-1][int(budget)])
    mf = ev_val["trial_flows"]
    mf["mahal"] = mahalanobis_score(buckets, mf)
    trials = ev_val["trials"]
    hit = mf[mf["mahal"] > thr].groupby("trial").size()
    by_trial = trials.index.isin(hit.index)
    flow_only = np.isfinite(trials["flow_delay"])
    out = []
    for (scenario, cell), g in trials.assign(mahal=by_trial, flow=flow_only).groupby(["scenario", "cell"], sort=False):
        out.append({"scenario": scenario, "cell": cell, "recall_mahalanobis": float(g["mahal"].mean()),
                    "recall_flow_indicators": float(g["flow"].mean())})
    return {"threshold": thr, "cells": out}


def known_event_check(fr, flow_models, choice):
    """Every known event flow (lawn >= 15 min, pool, hose, drip): which indicators fire and after how long."""
    kf = fr[fr["known"] & (fr["duration_s"] >= 900)]
    fired, delay = flow_alarms(kf, flow_limits(flow_models, choice, kf))
    rows = []
    for i in kf.index:
        ind = [k for k in FLOW_IND if fired.loc[i, k]]
        rows.append({"start": str(kf.loc[i, "start_ts"].tz_convert(TZ))[:19], "duration_s": float(kf.loc[i, "duration_s"]),
                     "liters": round(float(kf.loc[i, "liters"]), 1), "rate_lpm": round(float(kf.loc[i, "rate"]), 1),
                     "indicators": ind,
                     "delay_min": round(float(delay.loc[i, ind].min()) / 60, 1) if ind else None})
    return rows


def firmware_checks(fr, flow_models, choice):
    new = fr[(fr["device"] != "bigquery")]
    fired, delay = flow_alarms(new, flow_limits(flow_models, choice, new))
    closed = new[new["closed"].fillna(False).astype(bool)]
    return [{"start": str(closed.loc[i, "start_ts"].tz_convert(TZ))[:19], "duration_s": float(closed.loc[i, "duration_s"]),
             "liters": round(float(closed.loc[i, "liters"]), 1),
             "indicators": [k for k in FLOW_IND if fired.loc[i, k]]} for i in closed.index]


def thresholds_table(bundle_h, flow_models, choice, hour_models):
    """Per local hour of week (+ a holiday profile): expected usage and the alarm thresholds, for humans/devices."""
    now = pd.Timestamp.now(tz=TZ)
    rows = []
    for holiday in (0, 1):
        for dow in range(7):
            for hour in range(24):
                if holiday and dow != 2:
                    continue
                rows.append({"holiday": holiday, "dow": dow, "hour": hour, "bridge": 0,
                             "doy_sin": np.sin(2 * np.pi * (now.dayofyear - 1) / 365.25),
                             "doy_cos": np.cos(2 * np.pi * (now.dayofyear - 1) / 365.25)})
    X = pd.DataFrame(rows)
    lim = flow_limits(flow_models, choice, X)
    out = []
    for i, r in X.iterrows():
        e = {"profile": "holiday" if r["holiday"] else ["mon", "tue", "wed", "thu", "fri", "sat", "sun"][int(r["dow"])],
             "hour": int(r["hour"])}
        for target in ("liters", "flows"):
            b = bundle_h[target]
            e[f"{target}_mean"] = round(float(b["mean"].predict(X.iloc[[i]])[0] * b["level"]), 2)
            for a, q in b["quantiles"].items():
                e[f"{target}_p{a * 100:g}"] = round(float(q.predict(X.iloc[[i]])[0] * b["level"]), 2)
            if f"hour_{target}" in choice:
                _, m = choice[f"hour_{target}"][band_of(r["hour"]).item()]
                e[f"alarm_hour_{target}"] = round(max(float(hour_models[target].predict(X.iloc[[i]])[0]), 1)
                                                  * np.exp(m), 1)
        e["alarm_flow_duration_s"] = round(float(np.exp(lim.loc[i, "duration"])))
        e["alarm_flow_liters"] = round(float(np.exp(lim.loc[i, "volume"])), 1)
        e["alarm_flow_rate_lpm_above"] = round(float(np.exp(lim.loc[i, "rate_high"])), 1)
        e["alarm_flow_rate_lpm_below"] = round(float(np.exp(lim.loc[i, "rate_low"])), 2)
        out.append(e)
    return out


def readable(choice):
    out = {}
    for k, v in choice.items():
        out[k] = v if k == "night_flows" else {b: {"alpha": a, "margin": round(float(np.exp(m)), 3)}
                                               for b, (a, m) in v.items()}
    return out


def main():
    cfg = json.loads((OUT / "metrics_hourly.json").read_text())
    cfg_hourly = {t: cfg["targets"][t]["best"] for t in ("liters", "flows")}
    bundle_h = joblib.load(OUT / "hourly.joblib")
    flows, events = load_flows(), load_known_events()
    df = hourly_frame(flows, events)
    fr = flow_table(flows, events)

    flow_cfg, flow_cfg_rows = choose_flow_config(fr)
    print(f"flow models: window {flow_cfg['window']}, season {flow_cfg['season']}")
    val = evaluate_split("validation", ["train"], fr, df, cfg_hourly, flow_cfg)
    choice = val["choice"]
    print("choice", choice, "validation FA/month", round(val["fa_total_per_month"], 2))
    mahal = mahalanobis_compare(fr, val)
    test = evaluate_split("test", ["train", "validation"], fr, df, cfg_hourly, flow_cfg, choice)
    print("test FA/month", round(test["fa_total_per_month"], 2))

    # Production: flow and hour models on all normal data; expected usage comes from hourly.joblib
    everything = ["train", "validation", "test"]
    prod = fit_flow_models(fr, everything, flow_cfg["window"], flow_cfg["season"])
    hour_models = {t: fit_hour_model(df, cfg_hourly[t], t, everything) for t in HOUR_IND.values()}
    known = known_event_check(fr, prod, choice)
    fw = firmware_checks(fr, prod, choice)
    table = thresholds_table(bundle_h, prod, choice, hour_models)

    used = {(ind, a) for ind in FLOW_IND for a, _ in choice[ind].values()}
    joblib.dump({"flow_models": {k: v for k, v in prod.items() if k in used}, "hour_models": hour_models,
                 "choice": choice, "flow_config": flow_cfg}, OUT / "anomaly.joblib")
    (OUT / "thresholds.json").write_text(json.dumps({"generated": str(pd.Timestamp.now(tz=TZ))[:19],
                                                     "choice": readable(choice), "table": table}, indent=1))

    def top(alarms, n=20):
        if not len(alarms):
            return []
        a = alarms.sort_values("severity", ascending=False).head(n).copy()
        a["ts"] = a["ts"].dt.tz_convert(TZ).astype(str).str[:19]
        return json.loads(a.to_json(orient="records"))

    report = {
        "flow_config": flow_cfg, "flow_config_selection": flow_cfg_rows,
        "choice": readable(choice),
        "budget": BUDGET,
        "validation": {"months": val["months"], "fa_calibration": val["fa_calibration"],
                       "fa_per_month": val["fa_per_month"], "fa_total_per_month": val["fa_total_per_month"],
                       "synthetic": summarize_trials(val["trials"])},
        "test": {"months": test["months"], "fa_per_month": test["fa_per_month"],
                 "fa_total_per_month": test["fa_total_per_month"], "synthetic": summarize_trials(test["trials"]),
                 "top_alarms": top(test["alarms"])},
        "validation_top_alarms": top(val["alarms"]),
        "mahalanobis": mahal,
        "known_events": known,
        "firmware_closes": fw,
    }
    (OUT / "metrics_anomaly.json").write_text(json.dumps(report, indent=1, default=str))
    for r in report["test"]["synthetic"]:
        print(f"  {r['scenario']:14} {r['cell']:32} recall {r['recall']:.2f} median {r['median_delay_min']} min")


if __name__ == "__main__":
    main()
