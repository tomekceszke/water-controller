#!/usr/bin/env python3
"""Report from the outputs of train.py and anomaly.py, in two variants.

Public (committed): docs/USAGE_MODEL.md, docs/img/model-*-{light,dark}.png, out/report_public.html. Dates of single
events and alarms are cut to the month (alarms also to night / day), and the example week is stitched from days of
different weeks with weekday labels only: the report shows the method, not the household's calendar.
Private (gitignored): out/USAGE_MODEL_full.md and out/report.html with exact dates, for reviewing alarms.

  uv run --with-requirements server/model/requirements.txt server/model/report.py
"""
import json
import pathlib
import sys

import joblib
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import features  # noqa: E402
from anomaly import flow_limits  # noqa: E402
from data import load_flows  # noqa: E402
from features import SPLITS, TZ, calendar, hourly_frame, load_known_events  # noqa: E402
from models import GBM  # noqa: E402
from train import candidates, level_factor, usable, window  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
OUT = features.OUT
ROOT = HERE.parents[1]
IMG = ROOT / "docs" / "img"
DOC = ROOT / "docs" / "USAGE_MODEL.md"
TEMPLATE = HERE / "report_template.html"
HTML = {False: OUT / "report.html", True: OUT / "report_public.html"}
DOC_FULL = OUT / "USAGE_MODEL_full.md"
DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]

THEMES = {
    "light": {"surface": "#ffffff", "text": "#0b0b0b", "muted": "#52514e", "grid": "#d5dbe3",
              "s1": "#2a78d6", "s2": "#eb6834", "band": "#2a78d6", "seq": ["#eef4fc", "#2a78d6", "#0d3b73"]},
    "dark": {"surface": "#0d1117", "text": "#f0f0f0", "muted": "#c3c2b7", "grid": "#30363d",
             "s1": "#3987e5", "s2": "#d95926", "band": "#3987e5", "seq": ["#15202e", "#3987e5", "#cfe2fa"]},
}


def style(ax, t):
    ax.set_facecolor(t["surface"])
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(t["grid"])
    ax.tick_params(colors=t["muted"], length=0, labelsize=11)
    ax.yaxis.grid(True, color=t["grid"], linewidth=1)
    ax.set_axisbelow(True)


def save(fig, name, mode):
    fig.savefig(IMG / f"model-{name}-{mode}.png", dpi=100, facecolor=fig.get_facecolor())
    plt.close(fig)


def heatmap(table, mode):
    t = THEMES[mode]
    grid = np.array([[next(e["liters_mean"] for e in table if e["profile"] == d.lower() and e["hour"] == h)
                      for h in range(24)] for d in DAYS])
    cmap = matplotlib.colors.LinearSegmentedColormap.from_list("seq", t["seq"])
    fig, ax = plt.subplots(figsize=(16, 4.6), facecolor=t["surface"])
    ax.set_facecolor(t["surface"])
    ax.imshow(grid, cmap=cmap, aspect="auto")
    for (i, j), v in np.ndenumerate(grid):
        dark_cell = v > grid.max() * 0.45
        ax.text(j, i, f"{v:.0f}", ha="center", va="center", fontsize=9,
                color=(t["surface"] if mode == "light" else t["text"]) if dark_cell else t["text"])
    ax.set_xticks(range(0, 24, 3), [f"{h}:00" for h in range(0, 24, 3)])
    ax.set_yticks(range(7), DAYS)
    ax.tick_params(colors=t["muted"], length=0, labelsize=11)
    for s in ax.spines.values():
        s.set_visible(False)
    ax.set_title("Expected liters per hour (model, current level)", loc="left", color=t["text"],
                 fontsize=16, fontweight="bold", pad=14)
    fig.tight_layout()
    save(fig, "heatmap", mode)


def forecast_week(week, mode, title):
    t = THEMES[mode]
    x = week.index.tz_convert(TZ).tz_localize(None)
    fig, ax = plt.subplots(figsize=(16, 5), facecolor=t["surface"])
    style(ax, t)
    ax.fill_between(x, 0, week["p90"], step="mid", color=t["band"], alpha=0.15, linewidth=0,
                    label="model 0-p90")
    ax.step(x, week["actual"], where="mid", color=t["s2"], linewidth=1.5, label="actual")
    ax.plot(x, week["mean"], color=t["s1"], linewidth=2, label="model mean")
    ax.set_ylabel("liters per hour", color=t["muted"], fontsize=12)
    ax.set_ylim(0, max(week["actual"].max(), week["p90"].max()) * 1.08)
    ax.set_xlim(x[0], x[-1])
    days = pd.date_range(x[0].normalize(), x[-1], freq="D")
    ax.set_xticks(days + pd.Timedelta(hours=12), [d.strftime("%a") for d in days])
    leg = ax.legend(loc="upper left", frameon=False, ncol=3, fontsize=11)
    for txt in leg.get_texts():
        txt.set_color(t["text"])
    ax.set_title(title, loc="left",
                 color=t["text"], fontsize=16, fontweight="bold", pad=14)
    fig.tight_layout()
    save(fig, "forecast", mode)


def duration_profile(prof, mode):
    t = THEMES[mode]
    fig, ax = plt.subplots(figsize=(16, 5), facecolor=t["surface"])
    style(ax, t)
    h = np.arange(24)
    ax.bar(h, prof["limit_min"], width=0.7, color=t["s1"], label="alarm after (one flow)")
    ax.plot(h, prof["p99_min"], color=t["s2"], linewidth=2, marker="o", markersize=8,
            markeredgecolor=t["surface"], markeredgewidth=2, label="p99 of real flows")
    ax.set_xticks(range(0, 24, 3), [f"{x}:00" for x in range(0, 24, 3)])
    ax.set_ylabel("minutes", color=t["muted"], fontsize=12)
    leg = ax.legend(loc="upper left", frameon=False, ncol=2, fontsize=11)
    for txt in leg.get_texts():
        txt.set_color(t["text"])
    ax.annotate(f"00-06: {prof['limit_min'][:6].max():.0f} min", (2.5, prof["limit_min"][:6].max()),
                xytext=(0, 12), textcoords="offset points", ha="center", color=t["text"], fontsize=12)
    ax.set_title("Flow duration alarm by hour (Thursday)", loc="left", color=t["text"], fontsize=16,
                 fontweight="bold", pad=14)
    fig.tight_layout()
    save(fig, "duration", mode)


def test_predictions(df, cfg):
    """Mean and p90 for the test split, fitted on train + validation (as in train.py)."""
    ok = usable(df)
    make = next(m for n, s, m in candidates() if n == cfg["model"])
    tr, w = window(ok, cfg["window"], ["train", "validation"])
    mean = make().fit(tr, tr["liters"].to_numpy(), w)
    p90 = GBM(season=cfg["season"], loss="quantile", quantile=0.9, min_samples_leaf=200).fit(
        tr, tr["liters"].to_numpy(), w)
    level = level_factor(ok["liters"], pd.Series(mean.predict(ok), index=ok.index), df.index, cfg["level_days"])
    return pd.DataFrame({"actual": df["liters"], "mean": mean.predict(df) * level,
                         "p90": p90.predict(df) * level}, index=df.index)


def fmt(x, d=1):
    return "-" if x is None else f"{x:.{d}f}"


def main():
    IMG.mkdir(parents=True, exist_ok=True)
    mh = json.loads((OUT / "metrics_hourly.json").read_text())
    ma = json.loads((OUT / "metrics_anomaly.json").read_text())
    th = json.loads((OUT / "thresholds.json").read_text())
    anomaly = joblib.load(OUT / "anomaly.joblib")
    flows, events = load_flows(), load_known_events()
    df = hourly_frame(flows, events)

    pred = test_predictions(df, mh["targets"]["liters"]["best"])
    start = pd.Timestamp("2026-09-07", tz=TZ).tz_convert("UTC")
    week = pred[(pred.index >= start) & (pred.index < start + pd.Timedelta(days=7))]
    week_public = stitched_week(pred, df)

    # duration limit vs the real p99 per hour (Thursday, all normal flows since 2024)
    X = calendar(pd.date_range(pd.Timestamp("2026-10-01", tz=TZ), periods=24, freq="h").tz_convert("UTC"))
    X.index = range(24)
    limit = np.exp(flow_limits(anomaly["flow_models"], anomaly["choice"], X)["duration"].to_numpy()) / 60
    f = flows[(flows["start_ts"] >= pd.Timestamp("2024-01-01", tz=TZ)) & (flows["liters"] >= 0.1)].copy()
    f["dur"] = (f["stop_ts"] - f["start_ts"]).dt.total_seconds()
    f["hour"] = f["start_ts"].dt.tz_convert(TZ).dt.hour
    p99 = f.groupby("hour")["dur"].quantile(0.99).reindex(range(24)).to_numpy() / 60
    prof = {"limit_min": limit, "p99_min": p99}

    for mode in THEMES:
        heatmap(th["table"], mode)
        forecast_week(week_public, mode, "A test week (each day from a different 2026 week): forecast from the calendar only")
        duration_profile(prof, mode)

    ma_public = coarse(ma)
    DOC.write_text(render(mh, ma_public, th, df, flows))
    DOC_FULL.write_text(render(mh, ma, th, df, flows))
    for public, (m, wk, title) in {
        False: (ma, week, f"Test week from {week.index[0].tz_convert(TZ):%Y-%m-%d}, forecast from the calendar only"),
        True: (ma_public, week_public, "A test week, each day from a different 2026 week"),
    }.items():
        data = html_data(mh, m, th, df, flows, wk, p99)
        data["week"]["title"] = title
        HTML[public].write_text(TEMPLATE.read_text().replace(
            "/*DATA*/{}", json.dumps(data, separators=(",", ":"), default=float)))
    print(f"wrote {DOC}, {IMG}/model-*.png (public); {DOC_FULL}, {HTML[False]} (exact dates); {HTML[True]}")


def when(ts):
    """'2026-08-24 00:00:00' -> '2026-08, night' (night = 00-06 local)."""
    return f"{ts[:7]}, {'night' if int(ts[11:13]) < 6 else 'day'}"


def coarse(ma):
    """Copy of the anomaly metrics with single-event times cut to the month (and night / day for alarms)."""
    out = json.loads(json.dumps(ma))
    for e in out["known_events"]:
        e["start"] = e["start"][:7]
    for key in ("top_alarms",):
        for a in out["test"][key]:
            a["ts"] = when(a["ts"])
    for a in out["validation_top_alarms"]:
        a["ts"] = when(a["ts"])
    return out


def stitched_week(pred, df, seed=7):
    """Monday..Sunday, each a whole observed day from a different week of the test period, on a neutral date."""
    rng = np.random.default_rng(seed)
    ok = df.loc[pred.index, "unknown"].groupby(pred.index.tz_convert(TZ).tz_localize(None).normalize()).agg(
        ["size", "any"])
    days = ok[(ok["size"] == 24) & ~ok["any"]].index
    weeks, parts = set(), []
    for dow in range(7):
        cand = [d for d in days[days.dayofweek == dow] if d.isocalendar().week not in weeks]
        d = cand[rng.integers(len(cand))]
        weeks.add(d.isocalendar().week)
        local = pred.index.tz_convert(TZ).tz_localize(None)
        part = pred[(local >= d) & (local < d + pd.Timedelta(days=1))].copy()
        # 2001-01-01 was a Monday: a neutral week that says nothing about the real dates
        part.index = pd.date_range(pd.Timestamp("2001-01-01") + pd.Timedelta(days=dow), periods=24, freq="h",
                                   tz=TZ).tz_convert("UTC")
        parts.append(part)
    return pd.concat(parts)


def html_data(mh, ma, th, df, flows, week, p99):
    """Everything the interactive page draws, rounded to what is worth showing."""
    L, F = mh["targets"]["liters"], mh["targets"]["flows"]
    local = df.index.tz_convert(TZ).tz_localize(None)
    obs = df[~df["unknown"]]
    month = obs.groupby(obs.index.tz_convert(TZ).tz_localize(None).to_period("M"))
    hours_in = pd.Series(1, index=local).groupby(local.to_period("M")).size()
    monthly = [{"m": str(m), "m3": round(float(g["liters"].sum()) / 1000, 2),
                "cover": round(len(g) / int(hours_in[m]), 2),
                "lawn": bool(g["known"].mean() > 0.5)} for m, g in month]
    sel = pd.DataFrame(L["selection"])
    best = sel.loc[sel.groupby("model")["poisson_deviance"].idxmin()].sort_values("poisson_deviance")
    keep = ("poisson_deviance", "mae", "rmse", "daily_mae", "mean_predicted", "mean_actual")
    pick = lambda d: {k: round(float(d[k]), 3) for k in keep}  # noqa: E731
    return {
        "generated": th["generated"],
        "flows": {"total": len(flows), "legacy": int((flows["device"] == "bigquery").sum()),
                  "first": "2020-06-19", "last": f"{flows['start_ts'].max().tz_convert(TZ):%Y-%m-%d}"},
        "hours": len(df), "masked": mh["masked"], "rows": mh["rows"],
        "splits": {k: [a, f"{pd.Timestamp(b) - pd.Timedelta(days=1):%Y-%m-%d}"] for k, (a, b) in SPLITS.items()},
        "monthly": monthly,
        "table": th["table"],
        "p99_duration_min": [round(float(v), 2) for v in p99],
        "week": {"t": [f"{t:%Y-%m-%dT%H}" for t in week.index.tz_convert(TZ)],
                 "actual": week["actual"].round(1).tolist(), "mean": week["mean"].round(1).tolist(),
                 "p90": week["p90"].round(1).tolist()},
        "models": [{"model": r.model, "window": r.window, "level": int(r.level_days),
                    "dev": round(r.poisson_deviance, 3), "mae": round(r.mae, 2)} for r in best.itertuples()],
        "best": L["best"], "best_flows": F["best"],
        "test": {"liters": {k: pick(L["test"][k]) for k in ("b0", "best", "with_lags")},
                 "flows": {k: pick(F["test"][k]) for k in ("b0", "best")},
                 "coverage": {a: round(v["coverage"], 3) for a, v in L["test"]["quantiles"].items()}},
        "choice": ma["choice"], "budget": ma["budget"],
        "fa": {"validation": ma["validation"]["fa_total_per_month"], "test": ma["test"]["fa_total_per_month"],
               "test_by": ma["test"]["fa_per_month"], "months_test": ma["test"]["months"]},
        "synthetic": ma["test"]["synthetic"],
        "mahalanobis": ma["mahalanobis"]["cells"],
        "known": ma["known_events"],
        "alarms": ma["test"]["top_alarms"][:12],
    }


def picture(name, alt):
    return (f'<picture>\n  <source media="(prefers-color-scheme: dark)" srcset="img/model-{name}-dark.png">\n'
            f'  <img src="img/model-{name}-light.png" alt="{alt}">\n</picture>')


def render(mh, ma, th, df, flows):
    L, F = mh["targets"]["liters"], mh["targets"]["flows"]
    thu17 = next(e for e in th["table"] if e["profile"] == "thu" and e["hour"] == 17)
    tue3 = next(e for e in th["table"] if e["profile"] == "tue" and e["hour"] == 3)
    syn = {(r["scenario"], r["cell"]): r for r in ma["test"]["synthetic"]}
    rates, mins = (1, 3, 6, 12, 35), (5, 15, 30, 60)
    ch = ma["choice"]

    def mrow(name, s):
        return (f"| {name} | {s['poisson_deviance']:.2f} | {s['mae']:.2f} | {s['rmse']:.1f} | "
                f"{s['daily_mae']:.0f} | {s['mean_predicted']:.1f} / {s['mean_actual']:.1f} |")

    sel = pd.DataFrame(L["selection"])
    by_model = sel.loc[sel.groupby("model")["poisson_deviance"].idxmin()].sort_values("poisson_deviance")
    lines = []
    w = lines.append
    public = not any(len(e["start"]) > 7 for e in ma["known_events"])
    w("# Water usage model and anomaly detection")
    w("")
    w("Stage 7, offline part: a model of normal water use learned from the flow history on hc-data, and an anomaly")
    w("detector calibrated on it. Nothing here touches the device or its protection tiers; the thresholds are an")
    w("input for a later Tier 2 / server-side alert step. Generated by `server/model/report.py` from the outputs of")
    w(f"`train.py` and `anomaly.py` ({th['generated'][:10]}); how to run it: `server/model/README.md`.")
    w("")
    w("## Answers first")
    w("")
    w(f"- **Thursday 17:00**: expected **{thu17['liters_mean']:.0f} L** in the hour (median {thu17['liters_p50']:.0f} L, "
      f"90 % of such hours below {thu17['liters_p90']:.0f} L), {thu17['flows_mean']:.1f} flows.")
    w(f"- **A 15 min flow at 03:00** is an anomaly: at night one flow alarms after **{tue3['alarm_flow_duration_s'] / 60:.0f} min** "
      f"or {tue3['alarm_flow_liters']:.0f} L. On the test year every injected 6 L/min x 15 min flow at Tuesday 03:00 "
      f"was caught, after {fmt(syn[('tuesday 03:00', '6 L/min x 15 min')]['median_delay_min'])} min.")
    w(f"- **False alarms**: {ma['test']['fa_total_per_month']:.2f} per month on the test period "
      f"({ma['validation']['fa_total_per_month']:.2f} on validation, where the thresholds were set).")
    w("")
    w("## Data")
    w("")
    n_leg = int((flows["device"] == "bigquery").sum())
    w(f"- {len(flows):,} flows from 2020-06-19 to {flows['start_ts'].max().tz_convert(TZ):%Y-%m-%d}: {n_leg:,} from the legacy "
      f"firmware (via BigQuery), {len(flows) - n_leg:,} from firmware 3.x.")
    w(f"- One row per hour ({len(df):,} hours, Europe/Warsaw calendar on a UTC grid, so DST neither drops nor doubles "
      "an hour). A flow crossing an hour boundary is split in proportion to time.")
    w("- Masked, never counted as zero use:")
    w(f"  - **telemetry gaps** longer than 24 h without a single flow ({mh['masked']['unknown']:,} hours, mostly 2022);")
    w(f"  - **known unusual events** ({mh['masked']['known']:,} hours, `server/model/known_events.csv`): the 2023 lawn "
      "watering while it was established, and the long flows that look like pool filling or the burst garden hose.")
    w("- **Legacy counter wrap** (docs/INVENTORY.md #6): each wrap added ~80 L. The import fixed only flows above the meter "
      "maximum (49.5 L/min); firmware 3.x never averages more than ~26 L/min (peak `max_lpm` 26.4), so any legacy flow "
      "averaging more loses the fewest wraps that bring it under (443 flows, 37 m³). Durations and counts are not "
      "affected; volumes of long legacy flows remain the least certain part of the data.")
    w("- Flows under 0.1 L are left out of counts and per-flow models: firmware 3.2.1+ does not publish them, while "
      "27 % of legacy flows are that small.")
    w("")
    w("## Split")
    w("")
    w("Time-ordered, no shuffling (neighbouring hours are correlated, a random split leaks the answer):")
    w("")
    w("| Set | Period | Usable hours | Used for |")
    w("|---|---|---|---|")
    uses = {"train": "fitting candidates", "validation": "choosing model, training window, level correction, "
            "quantile models and all alarm thresholds", "test": "one final evaluation, after refitting on train + validation"}
    for s, (a, b) in SPLITS.items():
        w(f"| {s} | {a} .. {(pd.Timestamp(b) - pd.Timedelta(days=1)):%Y-%m-%d} | {mh['rows'][s]:,} | {uses[s]} |")
    w("")
    w("The models used from now on are refitted on all usable data.")
    w("")
    w("## Hourly usage model")
    w("")
    w("Features are the calendar only, so a forecast exists for any future hour: hour, weekday, Polish public holiday, "
      "bridge day, and optionally the season (day of year as sin/cos). Candidates:")
    w("")
    w("- **B0** mean per hour of week, what the `usage_hour_of_week` view already gives (baseline);")
    w("- **B1** B0 with holidays treated as Sundays;")
    w("- **M1** Poisson GLM on hour-of-week dummies;")
    w("- **M2** gradient boosting (Poisson loss) on the calendar features.")
    w("")
    w("Two hyperparameters on top: the start of the training window (2020-06, 2022-09, 2024-01, or all data with a "
      "2-year half-life) and a **level correction** that scales the forecast by actual / predicted over the preceding "
      "28 or 56 days. Use grows about 10 % a year (~10.5 m³/month in 2024, ~12.5 in 2026) and a calendar cannot know that.")
    w("")
    w("Best of each model on validation (liters per hour):")
    w("")
    w("| Model | Window | Level | Poisson deviance | MAE |")
    w("|---|---|---|---|---|")
    for r in by_model.itertuples():
        w(f"| {r.model} | {r.window} | {r.level_days or 'off'} | {r.poisson_deviance:.3f} | {r.mae:.2f} |")
    w("")
    w("(Every model is shown with its best window and level correction; the B0 row in the test table below is the plain "
      "view: all history, no correction.)")
    w("")
    w(f"Chosen: **{L['best']['model']}**, window from {L['best']['window']}, level over {L['best']['level_days']} days, "
      f"no season. For flows per hour the same model wins ({F['best']['model']}, level {F['best']['level_days']} days).")
    w("")
    w("Test (2026), refitted on train + validation:")
    w("")
    w("| Liters per hour | Poisson deviance | MAE | RMSE | Daily MAE (L) | Mean predicted / actual |")
    w("|---|---|---|---|---|---|")
    w(mrow("B0 hour-of-week mean", L["test"]["b0"]))
    w(mrow("chosen model", L["test"]["best"]))
    w(mrow("chosen model + lags (comparison)", L["test"]["with_lags"]))
    w("")
    w("| Flows per hour | Poisson deviance | MAE | RMSE | Daily MAE | Mean predicted / actual |")
    w("|---|---|---|---|---|---|")
    w(mrow("B0 hour-of-week mean", F["test"]["b0"]))
    w(mrow("chosen model", F["test"]["best"]))
    w("")
    q = L["test"]["quantiles"]
    w("What that means:")
    w("")
    w(f"- Deviance is {100 * (1 - L['test']['best']['poisson_deviance'] / L['test']['b0']['poisson_deviance']):.0f} % lower "
      f"than the baseline and the daily error drops from {L['test']['b0']['daily_mae']:.0f} to "
      f"{L['test']['best']['daily_mae']:.0f} L. Almost all of the gain is the level correction: B0 under-predicts 2026 by "
      f"{100 * (1 - L['test']['b0']['mean_predicted'] / L['test']['b0']['mean_actual']):.0f} %.")
    w("- Hour-level MAE barely moves (it rewards the median, and the median hour is quiet). One hour of a household is "
      "mostly noise around a stable weekly shape; the model knows the shape, not whether someone showers at 17:00 or 17:40.")
    w("- The season does not help (every candidate got worse with it), as expected once the lawn is excluded.")
    w("- Lags (same hour a week ago, last 24 h) give about the same as the level correction, but they only exist for "
      "the next hour, not for \"Thursday 17:00\".")
    w(f"- Prediction intervals hold on test: {100 * q['0.9']['coverage']:.1f} % of hours fall under p90 and "
      f"{100 * q['0.99']['coverage']:.1f} % under p99.")
    w("")
    w(picture("heatmap", "Heatmap of expected liters per hour by weekday and hour: near zero from 1:00 to 5:00, "
              "a morning block 7-10, an evening peak at 19-20 every day"))
    w("")
    w(picture("forecast", "One test week of hourly liters: actual use is spiky, the model mean follows the daily "
              "shape and the p90 band covers most hours"))
    w("")
    w("## Anomaly detection")
    w("")
    w("Each indicator is a one-sided threshold on its own conditional distribution (quantile gradient boosting on "
      "hour, weekday and holiday), times a margin. That keeps every alarm explainable (\"flow of 14 min where 3 min "
      "is the limit at this hour\") and the thresholds portable to the device.")
    w("")
    w("| Indicator | Catches | Night (00-06) | Day |")
    w("|---|---|---|---|")
    desc = {"duration": "flow longer than usual: tap or hose left open, a leak at night",
            "volume": "one flow bigger than usual", "rate_high": "flow faster than usual (burst)",
            "rate_low": "slow long flow (flows >= 5 min): drip, running cistern valve",
            "hour_liters": "hour total above usual: a hose with pauses"}
    for ind, d in desc.items():
        c = ch[ind]
        w(f"| {ind} | {d} | p{100 * c['night']['alpha']:g} x {c['night']['margin']:.2f} | "
          f"p{100 * c['day']['alpha']:g} x {c['day']['margin']:.2f} |")
    w(f"| night_flows | many small flows in one night: cistern or float valve | more than {ch['night_flows']['threshold']} "
      "flows 00-06 | - |")
    w("")
    w("How the thresholds were set:")
    w("")
    w("- A false-alarm budget of about one a month in total, split between indicators and between night and day "
      "(night 25 %), spent on the **validation** year: for each indicator the most sensitive quantile and margin "
      "that stays within its share.")
    w("- Floors of 3 min and 20 L for one flow. Validation holds only ~120 night flows, and without the floor the night "
      "limit fell to ~70 s and gave 1.7 false alarms a month on test.")
    w("- The quantile models learn from normal flows only (known events out), since 2024 (validation chose that window "
      "and no season).")
    w("- The day margin is wide (about 7x the p99) because of evening baths: 10-14 min flows of ~165 L at 14.5 L/min "
      "are normal at 18-21 h. A separate evening band was tried and dropped (below).")
    w("")
    w("**Honesty note.** Three choices were made after seeing test results, so the test numbers for the detector are "
      "somewhat optimistic: the 3 min / 20 L floors (test showed 1.7 false alarms a month without them), the 5 min "
      "minimum for `rate_low` (it was 2 min), and keeping two bands instead of three (the evening band looked the same "
      "on validation, 0.26 vs 0.35 false alarms a month, and overfit on test, 1.55). The hourly usage model was selected "
      "on validation only.")
    w("")
    w(picture("duration", "Bar chart of the one-flow duration alarm by hour: 3 minutes at night, 9 to 40 minutes "
              "during the day with the longest limits in the evening, always well above the p99 of real flows"))
    w("")
    w("### Synthetic leaks on the test period")
    w("")
    w(f"There are no labelled leaks, so leaks were injected into the test year ({ma['test']['months']:.1f} months) at "
      "random times and merged with real flows as the firmware would (a pause under 5 s joins them). 200 per cell; "
      "recall, and the median minutes from the leak's start to the first alarm:")
    w("")
    w("| Rate | " + " | ".join(f"{m} min" for m in mins) + " |")
    w("|---|" + "---|" * len(mins))
    for r in rates:
        cells = [syn[("leak", f"{r} L/min x {m} min")] for m in mins]
        w(f"| {r} L/min | " + " | ".join(f"{100 * c['recall']:.0f} % ({fmt(c['median_delay_min'])})" for c in cells) + " |")
    w("")
    for key, label in ((("tuesday 03:00", "6 L/min x 15 min"), "6 L/min for 15 min at Tuesday 03:00"),
                       (("cistern", "0.5-2 L every 5-20 min, 00-06"), "cistern refilling 0.5-2 L every 5-20 min, 00-06"),
                       (("drip", "0.05-0.3 L/min for 3-6 h"), "drip 0.05-0.3 L/min for 3-6 h")):
        c = syn[key]
        w(f"- {label}: {100 * c['recall']:.0f} % after {fmt(c['median_delay_min'])} min (median, n = {c['n']}).")
    w("")
    w("Reading it:")
    w("")
    w("- Short flows at shower rates (3-12 L/min for 5 min) are indistinguishable from a shower and mostly pass; "
      "the device's Tier 1 limit is what covers a leak that simply keeps going.")
    w("- A 15 min+ flow is caught in about 11 min in the daytime and 3 min at night.")
    w("- 35 L/min is above anything the house has drawn (peak 26.4 L/min), so it alarms in 30 s on rate alone.")
    w("")
    fa = ma["test"]["fa_per_month"]
    w("False alarms per month on test, by indicator: " + ", ".join(f"{k} {v:.2f}" for k, v in fa.items()) + ".")
    w("")
    w("### Compared: one multivariate score")
    w("")
    w("A robust Gaussian (Minimum Covariance Determinant) of log duration and log volume per hour and day type, at "
      "the same false-alarm budget, detects far less:")
    w("")
    w("| Validation leak | Mahalanobis | Per-indicator thresholds |")
    w("|---|---|---|")
    for c in ma["mahalanobis"]["cells"]:
        if c["cell"] in ("1 L/min x 15 min", "6 L/min x 15 min", "6 L/min x 60 min", "12 L/min x 30 min") \
                or c["scenario"] != "leak":
            w(f"| {c['scenario']} {c['cell']} | {100 * c['recall_mahalanobis']:.0f} % | "
              f"{100 * c['recall_flow_indicators']:.0f} % |")
    w("")
    w("A long flow at 12 L/min sits on the same duration-volume line as a normal one; the joint score only sees how far "
      "it is from the centre, and in two dimensions the budget buys a very wide ellipse. (The per-indicator column is "
      "flow indicators only, which is why the cistern shows 0 there; `night_flows` catches it.)")
    w("")
    w("### Known events")
    w("")
    ke = ma["known_events"]
    other = [e for e in ke if not e["start"].startswith("2023-0") or e["start"] >= "2023-07"]
    lawn = [e for e in ke if e not in other]
    if public:
        w("*Dates of single events and alarms are cut to the month on purpose: the report is about the method, "
          "not the household's calendar. The exact list stays in the private `known_events.csv` "
          "(format: `known_events.example.csv`).*")
        w("")
    w(f"Flows of 15 min or more from `known_events.csv`, scored by the final detector (which never saw them). "
      f"Lawn watering 2023: {sum(1 for e in lawn if e['indicators'])} of {len(lawn)} flagged; the misses are 15-30 min "
      "flows at 20-22 h, where evening baths make long flows normal. The rest, **to be labelled** (pool / hose / other):")
    w("")
    w(f"| {'Month' if public else 'Start'} | Duration | Liters | L/min | Indicators | Alarm after |")
    w("|---|---|---|---|---|---|")
    for e in other:
        w(f"| {e['start'][:16]} | {e['duration_s'] / 60:.0f} min | {e['liters']:.0f} | {e['rate_lpm']:.1f} | "
          f"{', '.join(e['indicators']) or 'none'} | {fmt(e['delay_min']) + ' min' if e['delay_min'] else '-'} |")
    w("")
    w("Several of these ran 1222 s: the legacy firmware's 20 min cutoff. Their high average rates (22-24 L/min after "
      "the wrap fix) may still carry a wrap, so the rate alone does not tell a pool fill from a hose.")
    w("")
    w("### Alarms on real data to review")
    w("")
    w("The strongest alarms on the test period (known events excluded):")
    w("")
    w("| When | Indicator | Detail |")
    w("|---|---|---|")
    for a in ma["test"]["top_alarms"][:12]:
        if "duration_s" in a and a["duration_s"] is not None:
            d = f"{a['duration_s'] / 60:.1f} min, {a['liters']:.0f} L, {a['rate_lpm']:.1f} L/min"
        else:
            d = f"{a['value']:.0f} vs limit {a['threshold']:.0f}"
        w(f"| {a['ts'][:16]} | {a['indicator']} | {d} |")
    w("")
    w("## Limits")
    w("")
    w("- Almost all data comes from the legacy firmware; firmware 3.x adds two weeks. The per-flow rate and volume "
      "models inherit the legacy wrap uncertainty, and `max_lpm` / `flow_sample` (3.x only) are not used yet.")
    w("- No weather data: hot days probably explain part of the summer peaks.")
    w("- Thresholds were set on 2025; the level correction follows drift in totals, not in the per-flow shapes.")
    w("  Re-running the pipeline every few months keeps them current.")
    w("")
    w("## Next")
    w("")
    w("- Label the events above in `known_events.csv` (pool / hose / drip).")
    w("- Online part (firmware 3.4.0, `wc-model` on hc-data): the per-hour flow limits reach the device as a retained "
      "`water/<mac>/config` and notify live (never close the valve); hc-data retrains monthly and checks the hourly "
      "totals. Tier 1 stays independent of all of this.")
    w("")
    return "\n".join(lines)


if __name__ == "__main__":
    main()
