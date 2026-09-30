#!/usr/bin/env python3
"""Hourly usage models (liters per hour, flows per hour): selection on validation, one test evaluation, final fit.

  uv run --with-requirements server/model/requirements.txt server/model/train.py

Writes out/hourly.joblib (final models refitted on all data) and out/metrics_hourly.json.
"""
import itertools
import json
import pathlib
import sys

import joblib
import numpy as np
import pandas as pd
from sklearn.metrics import mean_absolute_error, mean_pinball_loss, mean_poisson_deviance, root_mean_squared_error

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import features  # noqa: E402
from data import load_flows  # noqa: E402
from features import TZ, age_weights, hourly_frame, load_known_events  # noqa: E402
from models import GBM, HourOfWeek, PoissonGLM  # noqa: E402

OUT = features.OUT
TARGETS = ("liters", "flows")
# No 0.999: the quantile loss gradient below the prediction is 1 - alpha, so boosting barely moves off the global
# 0.999 quantile in quiet hours (it predicted ~30 flows for 03:00, where 4 is the most ever seen).
QUANTILES = (0.5, 0.9, 0.99)
# Level correction: scale predictions by actual / predicted over the preceding N days (0 = off). It follows level
# changes a calendar cannot know. (The apparent growth of about 10 % a year in the legacy data was lost telemetry,
# not use: the utility meter shows flat use since mid-2023; features.reconcile scales legacy volumes to the meter.)
LEVEL_DAYS = (0, 28, 56)
LEVEL_CLIP = (0.5, 2.0)
# Recency weights (features.age_weights, owner decision 2026-09-30): the last 2 years count fully, each further year
# back by the decay, nothing beyond 5 years; the decay is a hyperparameter. "all data" is the unweighted reference.
WINDOWS = {"last 2 y": (0.0, 5), "decay 0.25": (0.25, 5), "decay 0.5": (0.5, 5), "decay 0.75": (0.75, 5),
           "all data": (1.0, None)}


def candidates():
    """Name -> factory(season) for the mean models."""
    yield "B0 hour-of-week mean", False, lambda: HourOfWeek()
    yield "B1 + holidays as Sunday", False, lambda: HourOfWeek(holidays_as_sunday=True)
    for season in (False, True):
        s = " + season" if season else ""
        yield f"M1 Poisson GLM{s}", season, lambda season=season: PoissonGLM(season=season)
        for leaves, iters in ((15, 300), (31, 300), (15, 600)):
            yield (f"M2 GBM{s} ({leaves} leaves, {iters} it)", season,
                   lambda season=season, leaves=leaves, iters=iters: GBM(season=season, max_leaf_nodes=leaves,
                                                                         max_iter=iters))


def usable(df):
    """Hours that count as normal, observed data."""
    return df[~df["unknown"] & ~df["known"] & df["split"].notna()]


def window(df, name, until):
    """Rows of the `until` splits with a positive recency weight, and those weights (age from the last row)."""
    decay, max_years = WINDOWS[name]
    part = df[df["split"].isin(until)]
    w = age_weights(part.index, part.index.max(), decay, max_years=max_years)
    keep = w > 0
    return part[keep], w[keep]


def scores(y, p, index):
    p = np.clip(p, 1e-6, None)
    local_day = index.tz_convert(TZ).tz_localize(None).normalize()
    daily = pd.DataFrame({"y": y, "p": p, "n": 1}, index=local_day).groupby(level=0).sum()
    daily = daily[daily["n"] >= 23]  # whole days only (23/25 on DST days)
    hod = pd.DataFrame({"e": np.abs(y - p), "h": index.tz_convert(TZ).hour}).groupby("h")["e"].mean()
    return {
        "mae": float(mean_absolute_error(y, p)),
        "rmse": float(root_mean_squared_error(y, p)),
        "poisson_deviance": float(mean_poisson_deviance(y, p)),
        "daily_mae": float(mean_absolute_error(daily["y"], daily["p"])) if len(daily) else None,
        "mae_by_hour": [round(float(v), 3) for v in hod.reindex(range(24)).to_numpy()],
        "mean_actual": float(np.mean(y)),
        "mean_predicted": float(np.mean(p)),
    }


def level_factor(y, p, index, days):
    """Per-hour factor sum(actual) / sum(predicted) over the `days` before that hour's local day.

    y, p: Series on the hourly index of usable hours (gaps simply contribute nothing). days = 0 gives 1.
    """
    if not days:
        return pd.Series(1.0, index=index)
    day = y.index.tz_convert(TZ).tz_localize(None).normalize()
    daily = pd.DataFrame({"y": y.to_numpy(), "p": p.to_numpy()}, index=day).groupby(level=0).sum()
    daily = daily.asfreq("D", fill_value=0.0)
    ys = daily["y"].rolling(days, min_periods=1).sum().shift(1)
    ps = daily["p"].rolling(days, min_periods=1).sum().shift(1)
    f = (ys / ps).clip(*LEVEL_CLIP).fillna(1.0)
    return pd.Series(f.reindex(index.tz_convert(TZ).tz_localize(None).normalize()).fillna(1.0).to_numpy(),
                     index=index)


def current_level(y, p, days):
    """The factor for "now": over the last `days` days of y/p."""
    if not days:
        return 1.0
    cut = y.index.max() - pd.Timedelta(days=days)
    ratio = y[y.index > cut].sum() / max(p[p.index > cut].sum(), 1e-9)
    return float(np.clip(ratio, *LEVEL_CLIP))


def quantile_models(season):
    """alpha -> (name, factory) pairs compared on validation pinball loss."""
    for a in QUANTILES:
        yield a, "empirical hour-of-week", lambda a=a: HourOfWeek(holidays_as_sunday=True, quantile=a)
        yield a, "GBM quantile", lambda a=a: GBM(season=season, loss="quantile", quantile=a, min_samples_leaf=200)


def quantile_scores(y, p, a):
    return {"pinball": float(mean_pinball_loss(y, p, alpha=a)), "coverage": float(np.mean(y <= p))}


def with_lags(df, target):
    """Comparison only: same hour a week ago and the previous 24 h (NaN where unknown)."""
    s = df[target].where(~df["unknown"])
    x = df.copy()
    x["lag_week"] = s.shift(168)
    x["prev_24h"] = s.shift(1).rolling(24, min_periods=20).sum()
    return x


def select(df, target):
    train_ok, val = usable(df), None
    val = train_ok[train_ok["split"] == "validation"]
    rows = []
    for (name, season, make), win in itertools.product(list(candidates()), WINDOWS):
        tr, w = window(train_ok, win, ["train"])
        model = make().fit(tr, tr[target].to_numpy(), w)
        p_all = pd.Series(model.predict(train_ok), index=train_ok.index)
        for days in LEVEL_DAYS:
            f = level_factor(train_ok[target], p_all, val.index, days)
            rows.append({"model": name, "season": season, "window": win, "level_days": days,
                         **scores(val[target].to_numpy(), p_all[val.index].to_numpy() * f.to_numpy(), val.index)})
            print(f"  {target:6} {win:17} {name:40} level {days:2} dev {rows[-1]['poisson_deviance']:.3f} "
                  f"mae {rows[-1]['mae']:.3f}")
    return rows


META = ("model", "window", "season", "level_days")


def fit_eval(make, qmakes, ok, target, win, fit_on, eval_split, days):
    """Fit the mean and quantile models on `fit_on`, predict `eval_split` with the level correction."""
    tr, w = window(ok, win, fit_on)
    mean = make().fit(tr, tr[target].to_numpy(), w)
    p_all = pd.Series(mean.predict(ok), index=ok.index)
    ev = ok[ok["split"] == eval_split]
    f = level_factor(ok[target], p_all, ev.index, days).to_numpy()
    out = {"mean": scores(ev[target].to_numpy(), p_all[ev.index].to_numpy() * f, ev.index), "quantiles": {}}
    for a, qname, qmake in qmakes:
        q = qmake().fit(tr, tr[target].to_numpy(), w)
        out["quantiles"].setdefault(str(a), []).append(
            {"model": qname, **quantile_scores(ev[target].to_numpy(), q.predict(ev) * f, a)})
    return out


def main():
    OUT.mkdir(exist_ok=True)
    df = hourly_frame(load_flows(), load_known_events())
    ok = usable(df)
    report = {"rows": {s: int((ok["split"] == s).sum()) for s in ("train", "validation", "test")},
              "masked": {"unknown": int(df["unknown"].sum()), "known": int(df["known"].sum()),
                         "vol_bad": int(df["vol_bad"].sum())},
              "targets": {}}
    bundle = {}
    for target in TARGETS:
        rows = select(df, target)
        best = min(rows, key=lambda r: r["poisson_deviance"])
        b0 = next(r for r in rows if r["model"].startswith("B0") and r["window"] == "all data" and not r["level_days"])
        make = next(m for n, s, m in candidates() if n == best["model"])
        season, win, days = best["season"], best["window"], best["level_days"]

        # Quantiles: choose per alpha on validation, with the winner's window and level correction
        val = fit_eval(make, list(quantile_models(season)), ok, target, win, ["train"], "validation", days)
        qbest = {a: min(v, key=lambda r: r["pinball"])["model"] for a, v in val["quantiles"].items()}
        chosen = [(a, n, m) for a, n, m in quantile_models(season) if n == qbest[str(a)]]

        # Test: refit on train + validation, evaluate once
        test = fit_eval(make, chosen, ok, target, win, ["train", "validation"], "test", days)
        base = fit_eval(lambda: HourOfWeek(), [], ok, target, "all data", ["train", "validation"], "test", 0)
        lag = {}
        lagged = usable(with_lags(df, target))
        for split_eval, fit_on in (("validation", ["train"]), ("test", ["train", "validation"])):
            tr_l, w_l = window(lagged, win, fit_on)
            ev = lagged[lagged["split"] == split_eval]
            m = GBM(season=season, extra=("lag_week", "prev_24h")).fit(tr_l, tr_l[target].to_numpy(), w_l)
            lag[split_eval] = scores(ev[target].to_numpy(), m.predict(ev), ev.index)

        # Production: refit on everything usable; the level factor is the one for "now"
        allw, w = window(ok, win, ["train", "validation", "test"])
        mean = make().fit(allw, allw[target].to_numpy(), w)
        level = current_level(ok[target], pd.Series(mean.predict(ok), index=ok.index), days)
        bundle[target] = {
            "mean": mean,
            "quantiles": {a: m().fit(allw, allw[target].to_numpy(), w) for a, _, m in chosen},
            "level": level,
            "config": {k: best[k] for k in META},
        }
        report["targets"][target] = {
            "selection": rows,
            "best": {k: best[k] for k in META},
            "level_now": level,
            "validation": {"best": {k: v for k, v in best.items() if k not in META},
                           "b0": {k: v for k, v in b0.items() if k not in META},
                           "quantiles": val["quantiles"], "with_lags": lag["validation"]},
            "test": {"best": test["mean"], "b0": base["mean"],
                     "quantiles": {a: v[0] for a, v in test["quantiles"].items()}, "with_lags": lag["test"]},
        }
        t = report["targets"][target]["test"]
        print(f"{target}: best {best['model']} / {win} / level {days}; test dev {t['best']['poisson_deviance']:.3f} "
              f"vs B0 {t['b0']['poisson_deviance']:.3f}; coverage "
              + ", ".join(f"p{float(a) * 100:g} {v['coverage']:.3f}" for a, v in t["quantiles"].items()))

    joblib.dump(bundle, OUT / "hourly.joblib")
    (OUT / "metrics_hourly.json").write_text(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
