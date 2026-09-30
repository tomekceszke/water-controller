#!/usr/bin/env python3
"""End-to-end hardware test for a spare board running a WATER_TEST_PULSES build (about 8 minutes for everything).

Never point it at production: it closes and opens the valve, changes settings and reboots the device.

Usage: WC_PASSWORD=... WC_ADMIN=... tools/hw_test.py 192.168.11.152 [--only tier1,caps] [--list]
  WC_PASSWORD  web password of the test build
  WC_ADMIN     Authorization header value (HEADER_AUTHORIZATION_VALUE)
  optional, for the "learned" section (needs paho-mqtt: uv run --with paho-mqtt tools/hw_test.py ...):
  WC_MODEL_MQTT_PASS  server/secrets.env MQTT_MODEL_PASS
  WC_DEVICE_ID        the board's MQTT id (lowercase STA MAC); spare builds publish under water-spare/

The test build shortens the clock-bound limits so the same logic runs in less time: Tier 0 at 45 s instead of
60 min, Tier 1 settable from 20 s. The order stays that of production (Tier 1 < Tier 0), and every section ends
with the settings restored and the valve open, also after a failure.
"""
import argparse
import http.cookiejar
import json
import os
import sys
import time
import urllib.error
import urllib.request

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument("host")
ap.add_argument("--only", help="comma-separated sections (see --list)")
ap.add_argument("--list", action="store_true", help="list the sections and exit")
args = ap.parse_args()

BASE = f"http://{args.host}"
K = 477                 # pulses per liter set for the test (round numbers below)
TIER0_S = 45            # WATER_TEST_TIER0_S of the test build
TIER1_S = 20            # shortest Tier 1 limit of the test build

jar = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
csrf = ""
failures = 0
PASSWORD = ADMIN = ""


def request(method, path, body=None, headers=None, auth=True, use_opener=True, timeout=15):
    h = {"Content-Type": "application/json"} if body is not None else {}
    if auth and csrf:
        h["X-CSRF-Token"] = csrf
    h.update(headers or {})
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
    try:
        with (opener if use_opener else urllib.request.build_opener()).open(req, timeout=timeout) as r:
            raw = r.read()
            return r.status, json.loads(raw) if raw[:1] in (b"{", b"[") else raw.decode()
    except urllib.error.HTTPError as e:
        raw = e.read()
        return e.code, json.loads(raw) if raw[:1] == b"{" else raw.decode()


def check(name, ok, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'} {name} {detail}")
    if not ok:
        failures += 1


def status():
    return request("GET", "/api/status")[1]


def login():
    global csrf
    code, body = request("POST", "/api/login", {"password": PASSWORD}, auth=False)
    if code != 200:
        sys.exit(f"login failed: {code} {body}")
    csrf = body["csrf"]


def admin(path, body):
    return request("POST", path, body, headers={"Authorization": ADMIN})


def pulses(hz, seconds):
    return admin("/admin/test/pulses", {"hz": hz, "seconds": seconds})


def settings(**kw):
    code, body = request("POST", "/api/settings", kw)
    assert code == 200, (code, body)
    return body


def valve(state):
    return request("POST", "/api/valve", {"state": state})


def wait_for(pred, timeout):
    """Seconds until pred() holds (polled every 0.5 s), or None after timeout."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return time.time() - t0
        time.sleep(0.5)
    return None


def closed():
    return status()["valve"]["state"] == "closed"


def wait_idle(timeout=60):
    if wait_for(lambda: not status()["flow"]["flowing"], timeout) is not None:
        time.sleep(6)       # Tier 0 joins flows separated by less than 5 s: let this one end for it too


def wait_online(timeout=90):
    def up():
        try:
            return request("GET", "/api/session", auth=False, timeout=3)[0] == 200
        except OSError:
            return False
    return wait_for(up, timeout)


def reboot_and_login():
    request("POST", "/api/reboot", {})
    time.sleep(4)
    wait_online()
    login()


def last_event(kind, since=0, **match):
    """Newest event of `kind` (at or after `since`, unix s, 1 s slack for the two clocks) matching the fields."""
    events = request("GET", "/api/events")[1]["events"]
    return next((e for e in events if e["type"] == kind and e.get("ts", 0) >= since - 1
                 and all(e.get(k) == v for k, v in match.items())), None)


def restore():
    pulses(0, 0)
    admin("/admin/test/tier2", {"suspend": False})
    admin("/admin/test/hour", {"hour": -1})
    request("POST", "/api/snooze", {"minutes": 0})
    wait_idle()
    settings(tier1_limit_s=1200, max_event_liters=0, pulses_per_liter=K, learned_close_night=False)
    if closed():
        valve("open")


# ---------------------------------------------------------------- sections

def security():
    check("status without session -> 401", request("GET", "/api/status", auth=False, use_opener=False)[0] == 401)
    check("valve without CSRF -> 403", request("POST", "/api/valve", {"state": "closed"}, auth=False)[0] == 403)
    check("foreign Host -> 403", request("GET", "/api/status", headers={"Host": "evil.example"})[0] == 403)
    check("foreign Origin -> 403",
          request("POST", "/api/valve", {"state": "closed"}, headers={"Origin": "http://evil.example"})[0] == 403)
    check("admin valve without header -> 401",
          request("POST", "/admin/valve", {"state": "closed"}, auth=False)[0] == 401)
    check("admin hw-status public", request("GET", "/admin/hw-status", auth=False)[0] == 200)


def counting():
    print("  100 Hz for 20 s (2000 pulses)")
    request("POST", "/api/diag", {"minutes": 2})
    pulses(100, 20)
    time.sleep(8)
    s = status()
    check("flowing during pulses", s["flow"]["flowing"], f'lpm={s["flow"]["lpm"]}')
    check("rate ~12.6 L/min", abs(s["flow"]["lpm"] - 100 * 60 / K) < 0.6, f'lpm={s["flow"]["lpm"]}')
    wait_idle()
    e = last_event("flow")
    expected_l = 2000 / K
    check("flow event liters", e is not None and abs(e["liters"] - expected_l) / expected_l < 0.02,
          f'got {e and e["liters"]:.3f} expected {expected_l:.3f}')
    check("flow event duration ~20 s", e is not None and 18 <= e["seconds"] <= 21, f'{e and e["seconds"]} s')
    d = status()["diag"]
    check("diag edges ~2000", abs(d["edges"] - 2000) <= 20, f'edges={d["edges"]}')
    check("diag min interval ~10 ms", 9000 <= d["min_interval_us"] <= 10100, f'{d["min_interval_us"]} us')
    check("diag no short intervals", d["short_intervals"] == 0, f'{d["short_intervals"]}')

    print("  16-bit accumulation: 1000 Hz for 36 s (36000 pulses > 32767)")
    before = status()["flow"]["counter"]
    pulses(1000, 36)
    time.sleep(37)
    wait_idle()
    after = status()["flow"]["counter"]
    e = last_event("flow")
    check("counter +36000", abs((after - before) - 36000) <= 180, f"delta={after - before}")
    check("event pulses not wrapped", e is not None and abs(e["liters"] * K - 36000) <= 180,
          f'liters={e and e["liters"]}')


def tier1():
    print(f"  limit {TIER1_S} s, 50 Hz for {TIER1_S + 20} s (the alert needs 15 s of flow after the close)")
    settings(tier1_limit_s=TIER1_S)
    since = time.time()
    pulses(50, TIER1_S + 20)
    t = wait_for(closed, TIER1_S + 8)
    s = status()
    check("valve closed by tier1", s["valve"]["state"] == "closed" and s["valve"]["reason"] == "tier1",
          f'{s["valve"]["state"]} {s["valve"]["reason"]}')
    check(f"closed after ~{TIER1_S} s", t is not None and TIER1_S - 1 <= t <= TIER1_S + 4, f"{t and round(t)} s")
    alert = wait_for(lambda: last_event("alert", since) is not None, 25)
    check("alert: still flowing after close", alert is not None)


def reboot():
    settings(tier1_limit_s=TIER1_S)
    valve("closed")
    reboot_and_login()
    s = status()
    check("still closed after reboot", s["valve"]["state"] == "closed", f'{s["valve"]["state"]} {s["valve"]["reason"]}')
    check("tier1 limit persisted", s["settings"]["tier1_limit_s"] == TIER1_S)
    check("reset reason software", s["system"]["reset_reason"] == "software")
    valve("open")
    reboot_and_login()
    s = status()
    check("still open after reboot", s["valve"]["state"] == "open" and s["valve"]["reason"] == "user",
          f'{s["valve"]["state"]} {s["valve"]["reason"]}')


def tier2():
    print("  max volume 2 L at 100 Hz (2 L after ~10 s)")
    settings(tier1_limit_s=1200, max_event_liters=2)
    pulses(100, 20)
    t = wait_for(closed, 18)
    s = status()
    check("closed by tier2", s["valve"]["reason"] == "tier2", s["valve"]["detail"])
    check("closed after ~10 s", t is not None and 8 <= t <= 13, f"{t and round(t)} s")
    wait_idle()
    print("  snooze: the same flow does not close")
    valve("open")
    request("POST", "/api/snooze", {"minutes": 5})
    pulses(100, 15)
    time.sleep(16)
    wait_idle()
    check("snoozed rule did not close", not closed(), status()["valve"]["reason"])


def caps():
    check("tier1_limit_s 14400 clamped to 2700", settings(tier1_limit_s=14400)["settings"]["tier1_limit_s"] == 2700)
    check("Tier 2 volume capped inside Tier 1",
          settings(tier1_limit_s=1200, max_event_liters=5000)["settings"]["max_event_liters"] == 180)
    check("Tier 2 cap follows a lower Tier 1", settings(tier1_limit_s=600)["settings"]["max_event_liters"] == 90)
    check("status reports tier0_limit_s", status()["flow"]["tier0_limit_s"] in (TIER0_S, 3600))


def tier0():
    print(f"  ceiling {TIER0_S} s, Tier 1 at its 45 min maximum, 50 Hz")
    settings(tier1_limit_s=2700, max_event_liters=0)
    pulses(50, 2 * TIER0_S + 20)
    t = wait_for(closed, TIER0_S + 8)
    s = status()
    check("closed by tier0", s["valve"]["reason"] == "tier0", f'{s["valve"]["reason"]} {s["valve"]["detail"]}')
    check(f"closed after ~{TIER0_S} s", t is not None and TIER0_S - 1 <= t <= TIER0_S + 4, f"{t and round(t)} s")
    print("  reopened while water still runs: a fresh limit")
    valve("open")
    t = wait_for(closed, TIER0_S + 8)
    s = status()
    check("closed by tier0 again", s["valve"]["reason"] == "tier0", s["valve"]["reason"])
    check(f"again after ~{TIER0_S} s from the reopen", t is not None and TIER0_S - 2 <= t <= TIER0_S + 4,
          f"{t and round(t)} s")


def wifi():
    print(f"  limit {TIER1_S} s, 50 Hz for {TIER1_S + 10} s, WiFi off for 40 s")
    settings(tier1_limit_s=TIER1_S, max_event_liters=0)
    pulses(50, TIER1_S + 10)
    try:    # the board drops WiFi at once, often before its answer is out: a timeout here is expected
        admin("/admin/test/wifi-off", {"seconds": 40})
    except OSError:
        pass
    time.sleep(40)
    wait_online()
    s = status()
    check("closed by tier1 while offline", s["valve"]["state"] == "closed" and s["valve"]["reason"] == "tier1",
          f'{s["valve"]["state"]} {s["valve"]["reason"]}')
    check("no reboot during outage", s["system"]["uptime_s"] > 40, f'uptime {s["system"]["uptime_s"]} s')


def hang():
    print(f"  Tier 2 suspended (max 1 L would fire at ~10 s), limit {TIER1_S} s, 50 Hz")
    settings(tier1_limit_s=TIER1_S, max_event_liters=1)
    admin("/admin/test/tier2", {"suspend": True})
    pulses(50, TIER1_S + 10)
    t = wait_for(closed, TIER1_S + 8)
    s = status()
    check("closed by tier1, not tier2", s["valve"]["reason"] == "tier1", s["valve"]["reason"])
    check(f"closed after ~{TIER1_S} s", t is not None and TIER1_S - 1 <= t <= TIER1_S + 4, f"{t and round(t)} s")


def learned():
    model_pass, device = os.environ.get("WC_MODEL_MQTT_PASS"), os.environ.get("WC_DEVICE_ID")
    if not (model_pass and device):
        print("  skipped: set WC_MODEL_MQTT_PASS and WC_DEVICE_ID")
        return
    import paho.mqtt.client as mqtt
    topic = f"water-spare/{device}/config"
    # A volume limit, not a duration: the shortest learned duration (60 s) is above the test build's Tier 0 (45 s)
    print(f"  retained {topic}: 5 L at every hour; 100 Hz (5 L after ~24 s)")

    def publish_config(payload):
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="wc-hwtest")
        c.username_pw_set("wc-model", model_pass)
        c.connect("192.168.11.16", 1883, keepalive=30)
        c.loop_start()
        c.publish(topic, payload, qos=1, retain=True).wait_for_publish(10)
        c.loop_stop()
        c.disconnect()

    cfg = {"v": 1, "generated": "2031-01-01", "model": "hwtest", "dur_s": [3600] * 168, "vol_l": [5] * 168,
           "night_flows": 200, "exp_l": [100] * 168, "p90_l": [300] * 168}
    publish_config(json.dumps(cfg))
    # Learned limits need the local hour: right after a flash or reboot the clock may not be synced yet
    wait_for(lambda: status()["system"]["time_synced"], 60)
    applied = wait_for(lambda: status().get("learned", {}).get("generated") == "2031-01-01", 20)
    check("learned config applied", applied is not None, str(status().get("learned")))
    settings(tier1_limit_s=1200, max_event_liters=0, learned_notify=True)
    since = time.time()
    pulses(100, 30)
    t = wait_for(lambda: last_event("rule", since, rule="learned_volume") is not None, 32)
    hit = last_event("rule", since, rule="learned_volume")
    check("learned volume notice after ~24 s", hit is not None and hit.get("limit") == 5 and not hit.get("closed")
          and t is not None and 22 <= t <= 28, f"{t and round(t)} s {hit}")
    check("learned limit never moves the valve", not closed(), status()["valve"]["state"])
    wait_idle()

    # Tier 3 night shut-off: only with the option on, only for flows starting 1:00-5:59
    def night_flow(hour, close_night):
        admin("/admin/test/hour", {"hour": hour})
        settings(learned_close_night=close_night)
        since = time.time()
        pulses(100, 30)
        wait_for(lambda: last_event("rule", since, rule="learned_volume") is not None, 32)
        time.sleep(1)
        hit, shut = last_event("rule", since, rule="learned_volume"), closed()
        pulses(0, 0)
        reason = status()["valve"]["reason"]
        if shut:
            valve("open")
        wait_idle()
        return hit, shut, reason

    hit, shut, _ = night_flow(3, False)
    check("03:00, option off: notice only", hit is not None and not hit.get("closed") and not shut, str(hit))
    hit, shut, reason = night_flow(3, True)
    check("03:00, option on: closed by tier3", hit is not None and hit.get("closed") and shut and reason == "tier3",
          f"{hit} {reason}")
    hit, shut, _ = night_flow(12, True)
    check("12:00, option on: notice only", hit is not None and not hit.get("closed") and not shut, str(hit))
    hit, shut, _ = night_flow(0, True)
    check("00:00, option on: notice only", hit is not None and not hit.get("closed") and not shut, str(hit))
    admin("/admin/test/hour", {"hour": -1})
    settings(learned_close_night=False)
    reboot_and_login()
    publish_config("")      # a deleted retained message: the device keeps the last limits
    time.sleep(3)
    check("learned limits survive a reboot and a deleted config",
          status().get("learned", {}).get("generated") == "2031-01-01", str(status().get("learned")))


def today():
    """Today's totals and hourly counters (RTC memory) survive a reboot; the since-restart numbers start over."""
    pulses(100, 10)
    wait_for(lambda: status()["flow"]["flowing"], 5)     # the flow starts with the next 1 s sample
    wait_idle()
    before, day_before = status()["usage"], request("GET", "/api/learned")[1].get("used_l")
    check("flow counted today", before["liters_today"] > 2, str(before))
    reboot_and_login()
    wait_for(lambda: status()["system"]["time_synced"], 30)
    time.sleep(3)
    after, day_after = status()["usage"], request("GET", "/api/learned")[1].get("used_l")
    check("liters today kept across the reboot", abs(after["liters_today"] - before["liters_today"]) < 0.01,
          f"{before['liters_today']} -> {after['liters_today']}")
    check("since restart starts over", after["liters_since_boot"] < 0.01 and after["flows_since_boot"] == 0, str(after))
    if day_before is not None:
        check("hourly counters kept across the reboot", day_after == day_before, f"{day_before} -> {day_after}")


SECTIONS = {"security": security, "counting": counting, "tier1": tier1, "reboot": reboot, "tier2": tier2,
            "caps": caps, "tier0": tier0, "wifi": wifi, "hang": hang, "learned": learned,
            "today": today}

if args.list:
    print(" ".join(SECTIONS))
    sys.exit(0)
PASSWORD, ADMIN = os.environ["WC_PASSWORD"], os.environ["WC_ADMIN"]
chosen = args.only.split(",") if args.only else list(SECTIONS)
unknown = [s for s in chosen if s not in SECTIONS]
if unknown:
    sys.exit(f"unknown section(s): {unknown}; see --list")

t_start = time.time()
login()
restore()
for name in chosen:
    print(f"{name}")
    try:
        SECTIONS[name]()
    except Exception as e:      # a broken section must not leave the valve shut or a setting changed
        check(f"{name} ran to the end", False, repr(e))
    finally:
        try:
            restore()
        except Exception as e:
            check("restore after the section", False, repr(e))

print(f"\n{failures} failure(s) in {round(time.time() - t_start)} s")
sys.exit(1 if failures else 0)
