#!/usr/bin/env python3
"""MQTT -> PostgreSQL bridge for water-controller telemetry.

Subscribes to water/<device>/{flow,sample,valve,rule,alert,status} with a persistent session and manual acks:
a message is acked only after it is committed, so the broker redelivers anything lost to a crash or DB outage.
Primary keys absorb duplicates. Malformed messages are logged, acked and dropped.
"""
import json
import logging
import math
import os
import re
import sys
import time

import paho.mqtt.client as mqtt

MQTT_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = "wc-ingest"
PG_DSN = os.environ.get("PG_DSN", "dbname=water")     # peer auth over the local socket

DEVICE_RE = re.compile(r"^[0-9a-f]{12}$")
MIN_TS = 1577836800                                    # 2020-01-01: older means the clock was not synced
MAX_FUTURE_S = 86400
KINDS = ("flow", "sample", "valve", "rule", "alert", "status")

SQL = {
    "flow": """
        INSERT INTO flow_event (device, start_ts, stop_ts, pulses, pulses_per_liter, max_lpm, closed)
        VALUES (%(device)s, to_timestamp(%(start)s), to_timestamp(%(stop)s), %(pulses)s, %(pulses_per_liter)s,
                %(max_lpm)s, %(closed)s)
        ON CONFLICT (device, start_ts) DO NOTHING""",
    "sample": """
        INSERT INTO flow_sample (device, ts, pulses, period_ms, lpm, flow_pulses)
        VALUES (%(device)s, to_timestamp(%(ts)s), %(pulses)s, %(period_ms)s, %(lpm)s, %(flow_pulses)s)
        ON CONFLICT (device, ts) DO NOTHING""",
    "valve": """
        INSERT INTO valve_event (device, ts, state, reason, detail)
        VALUES (%(device)s, to_timestamp(%(ts)s), %(state)s, %(reason)s, %(detail)s)
        ON CONFLICT (device, ts, state) DO NOTHING""",
    "rule": """
        INSERT INTO rule_event (device, ts, rule, close, pulses, detail)
        VALUES (%(device)s, to_timestamp(%(ts)s), %(rule)s, %(close)s, %(pulses)s, %(detail)s)
        ON CONFLICT (device, ts, rule) DO NOTHING""",
    "alert": """
        INSERT INTO alert_event (device, ts, detail)
        VALUES (%(device)s, to_timestamp(%(ts)s), %(detail)s)
        ON CONFLICT (device, ts) DO NOTHING""",
    "status": """
        INSERT INTO device_status (device, status, updated_at) VALUES (%(device)s, %(status)s, now())
        ON CONFLICT (device) DO UPDATE SET status = EXCLUDED.status, updated_at = now()""",
}

log = logging.getLogger("wc-ingest")


def _ts(data, key):
    v = data.get(key)
    if not isinstance(v, int) or isinstance(v, bool):
        raise ValueError(f"{key} not an int")
    if v < MIN_TS or v > time.time() + MAX_FUTURE_S:
        raise ValueError(f"{key} out of range: {v}")
    return v


def _int(data, key, lo=0, hi=2**31 - 1, required=True):
    v = data.get(key)
    if v is None and not required:
        return None
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        raise ValueError(f"{key} not an int in [{lo}, {hi}]")
    return v


def _num(data, key, lo=0.0, hi=1e6, required=False):
    v = data.get(key)
    if v is None and not required:
        return None
    if not isinstance(v, (int, float)) or isinstance(v, bool) or not math.isfinite(v) or not lo <= v <= hi:
        raise ValueError(f"{key} not a number in [{lo}, {hi}]")
    return float(v)


def _str(data, key, maxlen=64):
    v = data.get(key, "")
    if not isinstance(v, str) or len(v) > maxlen:
        raise ValueError(f"{key} not a short string")
    return v


def _bool(data, key):
    v = data.get(key)
    if not isinstance(v, bool):
        raise ValueError(f"{key} not a bool")
    return v


def parse(topic, payload):
    """Returns (kind, row dict) or raises ValueError."""
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "water" or parts[2] not in KINDS:
        raise ValueError("unexpected topic")
    device, kind = parts[1], parts[2]
    if not DEVICE_RE.match(device):
        raise ValueError("bad device id")
    if kind == "status":
        status = payload.decode("utf-8", "replace")
        if status not in ("online", "offline"):
            raise ValueError("bad status")
        return kind, {"device": device, "status": status}

    data = json.loads(payload)
    if not isinstance(data, dict):
        raise ValueError("payload not an object")
    row = {"device": device}
    if kind == "flow":
        row.update(start=_ts(data, "start"), stop=_ts(data, "stop"), pulses=_int(data, "pulses"),
                   pulses_per_liter=_int(data, "pulses_per_liter", 1, 10000, required=False),
                   max_lpm=_num(data, "max_lpm", hi=1000), closed=_bool(data, "closed"))
        if row["stop"] < row["start"]:
            raise ValueError("stop before start")
    elif kind == "sample":
        row.update(ts=_ts(data, "ts"), pulses=_int(data, "pulses"), period_ms=_int(data, "period_ms", 1, 3600000),
                   lpm=_num(data, "lpm", hi=1000), flow_pulses=_int(data, "flow_pulses", required=False))
    elif kind == "valve":
        state = _str(data, "state")
        if state not in ("open", "closed"):
            raise ValueError("bad valve state")
        row.update(ts=_ts(data, "ts"), state=state, reason=_str(data, "reason"), detail=_str(data, "detail"))
    elif kind == "rule":
        row.update(ts=_ts(data, "ts"), rule=_str(data, "rule"), close=_bool(data, "close"),
                   pulses=_int(data, "pulses", required=False), detail=_str(data, "detail"))
    elif kind == "alert":
        row.update(ts=_ts(data, "ts"), detail=_str(data, "detail"))
    return kind, row


class Ingest:
    def __init__(self):
        import psycopg                                  # imported here so parse() is testable without it
        self.psycopg = psycopg
        self.db = psycopg.connect(PG_DSN, autocommit=True)
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="wc-ingest",
            clean_session=False,
            protocol=mqtt.MQTTv311,
            manual_ack=True,
        )
        self.client.username_pw_set(MQTT_USER, os.environ["MQTT_INGEST_PASS"])
        self.client.reconnect_delay_set(min_delay=1, max_delay=60)
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("MQTT connect failed: %s", reason_code)
            return
        log.info("MQTT connected (session present: %s)", flags.session_present)
        client.subscribe([(f"water/+/{kind}", 1) for kind in KINDS])

    def on_disconnect(self, client, userdata, flags, reason_code, properties):
        log.warning("MQTT disconnected: %s", reason_code)

    def on_message(self, client, userdata, msg):
        try:
            kind, row = parse(msg.topic, msg.payload)
        except ValueError as e:                         # json.JSONDecodeError is a ValueError
            log.warning("Dropping %s %r: %s", msg.topic, msg.payload[:120], e)
            client.ack(msg.mid, msg.qos)
            return
        try:
            self.db.execute(SQL[kind], row)
        except self.psycopg.Error as e:
            # Leave unacked; exit so systemd restarts us and the broker redelivers
            log.error("DB insert failed, exiting: %s", e)
            client.disconnect()
            sys.exit(1)
        client.ack(msg.mid, msg.qos)
        if kind in ("valve", "rule", "alert", "status"):
            log.info("%s %s", msg.topic, row)

    def run(self):
        self.client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
        self.client.loop_forever(retry_first_connection=True)


def main():
    logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"), format="%(levelname)s %(message)s")
    Ingest().run()


if __name__ == "__main__":
    main()
