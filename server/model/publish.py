#!/usr/bin/env python3
"""Publishes the learned limits to the controller as the retained MQTT water/<mac>/config (firmware learned.c).

  uv run --with-requirements server/model/requirements.txt server/model/publish.py [--dry-run]

Reads out/thresholds.json and out/metrics_anomaly.json (anomaly.py). Publishes only when the new run passes the
guard; otherwise the retained message stays as it was and the owner gets an ntfy notice. Needs MQTT_MODEL_PASS
(and optionally NTFY_URL) in the environment or server/secrets.env.
"""
import argparse
import datetime as dt
import json
import os
import pathlib
import subprocess
import sys
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from data import PRODUCTION, SECRETS  # noqa: E402
from features import OUT  # noqa: E402

BROKER = ("192.168.11.16", 1883)
USER = "wc-model"
DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
# Guard: a run that would alarm more often than this on its own test period, or that let the night limit fall
# under the floor anomaly.py enforces, is not sent to the device.
MAX_TEST_FA_PER_MONTH = 2.0
MIN_NIGHT_DURATION_S = 180


def secret(key):
    if key in os.environ:
        return os.environ[key]
    if SECRETS.exists():
        for line in SECRETS.read_text().splitlines():
            if line.startswith(f"{key}="):
                return line.split("=", 1)[1].strip()
    return None


def build_config(thresholds, generated, model=""):
    """The firmware format: 168 values per array, Monday 00:00 first; expected liters x10 as integers."""
    rows = {(r["profile"], r["hour"]): r for r in thresholds["table"]}
    hours = [rows[(d, h)] for d in DAYS for h in range(24)]
    return {
        "v": 1,
        "generated": generated,
        "model": model,
        "dur_s": [int(round(r["alarm_flow_duration_s"])) for r in hours],
        "vol_l": [int(round(r["alarm_flow_liters"])) for r in hours],
        "night_flows": int(thresholds["choice"]["night_flows"]["threshold"]),
        "exp_l": [int(round(r["liters_mean"] * 10)) for r in hours],
        "p90_l": [int(round(r["liters_p90"] * 10)) for r in hours],
    }


def guard(config, metrics):
    """List of reasons not to publish (empty = fine)."""
    problems = []
    fa = metrics["test"]["fa_total_per_month"]
    if fa > MAX_TEST_FA_PER_MONTH:
        problems.append(f"{fa:.2f} false alarms a month on test (limit {MAX_TEST_FA_PER_MONTH})")
    night = min(config["dur_s"][d * 24 + h] for d in range(7) for h in range(6))
    if night < MIN_NIGHT_DURATION_S:
        problems.append(f"night duration limit {night} s under {MIN_NIGHT_DURATION_S} s")
    for key in ("dur_s", "vol_l", "exp_l", "p90_l"):
        if len(config[key]) != 168:
            problems.append(f"{key} has {len(config[key])} values")
    return problems


def notify(title, message):
    url = secret("NTFY_URL")
    if not url:
        return
    req = urllib.request.Request(url, data=message.encode(), headers={"Title": title, "Tags": "droplet"})
    try:
        urllib.request.urlopen(req, timeout=10).close()
    except OSError as e:
        print(f"ntfy failed: {e}", file=sys.stderr)


def git_sha():
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                              cwd=pathlib.Path(__file__).parent, timeout=5).stdout.strip()
    except OSError:
        return ""


def publish(payload):
    import paho.mqtt.client as mqtt
    password = secret("MQTT_MODEL_PASS")
    if not password:
        raise SystemExit("MQTT_MODEL_PASS missing")
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="wc-model-publish")
    client.username_pw_set(USER, password)
    client.connect(*BROKER, keepalive=30)
    client.loop_start()
    info = client.publish(f"water/{PRODUCTION}/config", payload, qos=1, retain=True)
    info.wait_for_publish(timeout=15)
    client.loop_stop()
    client.disconnect()
    if not info.is_published():
        raise SystemExit("config not acknowledged by the broker")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true", help="print the config instead of publishing it")
    args = ap.parse_args()
    thresholds = json.loads((OUT / "thresholds.json").read_text())
    metrics = json.loads((OUT / "metrics_anomaly.json").read_text())
    config = build_config(thresholds, thresholds["generated"][:10] or dt.date.today().isoformat(), git_sha())
    problems = guard(config, metrics)
    payload = json.dumps(config, separators=(",", ":"))
    if args.dry_run:
        print(payload)
        print(f"{len(payload)} bytes; guard: {'; '.join(problems) or 'ok'}", file=sys.stderr)
        return
    if problems:
        notify("Learned limits not updated", "The monthly retraining was held back: " + "; ".join(problems))
        raise SystemExit("guard: " + "; ".join(problems))
    publish(payload)
    print(f"published {len(payload)} bytes to water/{PRODUCTION}/config (generated {config['generated']})")


if __name__ == "__main__":
    main()
