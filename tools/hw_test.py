#!/usr/bin/env python3
"""End-to-end hardware test for a spare board running a WATER_TEST_PULSES build.

Never point it at production: it closes and opens the valve, changes settings and reboots the device.

Usage: WC_PASSWORD=... WC_ADMIN=... tools/hw_test.py 192.168.11.140
  WC_PASSWORD  web password of the test build
  WC_ADMIN     Authorization header value (HEADER_AUTHORIZATION_VALUE)
"""
import http.cookiejar
import json
import os
import sys
import time
import urllib.error
import urllib.request

HOST = sys.argv[1]
BASE = f"http://{HOST}"
PASSWORD = os.environ["WC_PASSWORD"]
ADMIN = os.environ["WC_ADMIN"]
K = 477

jar = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
csrf = ""
failures = 0


def request(method, path, body=None, headers=None, auth=True, use_opener=True):
    h = {"Content-Type": "application/json"} if body is not None else {}
    if auth and csrf:
        h["X-CSRF-Token"] = csrf
    h.update(headers or {})
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
    try:
        with (opener if use_opener else urllib.request.build_opener()).open(req, timeout=15) as r:
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


def pulses(hz, seconds):
    return request("POST", "/admin/test/pulses", {"hz": hz, "seconds": seconds}, headers={"Authorization": ADMIN})


def settings(**kw):
    code, body = request("POST", "/api/settings", kw)
    assert code == 200, (code, body)
    return body


def valve(state):
    return request("POST", "/api/valve", {"state": state})


def wait_idle(timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if not status()["flow"]["flowing"]:
            return True
        time.sleep(1)
    return False


def reboot_and_login():
    request("POST", "/api/reboot", {})
    time.sleep(8)
    for _ in range(40):
        try:
            if request("GET", "/api/session", auth=False)[0] == 200:
                break
        except OSError:
            pass
        time.sleep(1)
    time.sleep(2)
    login()


def last_event(kind):
    events = request("GET", "/api/events")[1]["events"]
    return next((e for e in events if e["type"] == kind), None)


print("security guards")
check("status without session -> 401", request("GET", "/api/status", auth=False, use_opener=False)[0] == 401)
login()
check("valve without CSRF -> 403", request("POST", "/api/valve", {"state": "closed"}, auth=False)[0] == 403)
check("foreign Host -> 403", request("GET", "/api/status", headers={"Host": "evil.example"})[0] == 403)
check("foreign Origin -> 403",
      request("POST", "/api/valve", {"state": "closed"}, headers={"Origin": "http://evil.example"})[0] == 403)
check("admin valve without header -> 401", request("POST", "/admin/valve", {"state": "closed"}, auth=False)[0] == 401)
check("admin hw-status public", request("GET", "/admin/hw-status", auth=False)[0] == 200)

print("baseline")
settings(tier1_limit_s=1200, max_event_liters=0, pulses_per_liter=K)
if status()["valve"]["state"] != "open":
    valve("open")
check("valve open", status()["valve"]["state"] == "open")

print("pulse counting: 100 Hz for 20 s (2000 pulses)")
request("POST", "/api/diag", {"minutes": 2})
pulses(100, 20)
time.sleep(10)
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

print("fast pulses: 400 Hz for 30 s (~50 L/min, 12000 pulses, crosses no 16-bit limit)")
before = status()["flow"]["counter"]
pulses(400, 30)
time.sleep(33)
wait_idle()
after = status()["flow"]["counter"]
check("counter +12000", abs((after - before) - 12000) <= 60, f"delta={after - before}")

print("16-bit accumulation: 600 Hz for 60 s (36000 pulses > 32767)")
before = status()["flow"]["counter"]
pulses(600, 60)
time.sleep(63)
wait_idle()
after = status()["flow"]["counter"]
e = last_event("flow")
check("counter +36000", abs((after - before) - 36000) <= 180, f"delta={after - before}")
check("event pulses not wrapped", e is not None and abs(e["liters"] * K - 36000) <= 180, f'liters={e and e["liters"]}')

print("Tier 1: limit 60 s, continuous 50 Hz for 100 s")
settings(tier1_limit_s=60)
t0 = time.time()
pulses(50, 100)
closed_at = None
while time.time() - t0 < 95:
    s = status()
    if s["valve"]["state"] == "closed" and closed_at is None:
        closed_at = time.time() - t0
    time.sleep(1)
s = status()
check("valve closed by tier1", s["valve"]["state"] == "closed" and s["valve"]["reason"] == "tier1",
      f'{s["valve"]["state"]} {s["valve"]["reason"]}')
check("closed after ~60 s", closed_at is not None and 58 <= closed_at <= 64, f"{closed_at and round(closed_at)} s")
check("alert: still flowing after close", last_event("alert") is not None)
wait_idle()

print("valve state survives reboot (closed)")
reboot_and_login()
s = status()
check("still closed after reboot", s["valve"]["state"] == "closed" and s["valve"]["reason"] == "tier1",
      f'{s["valve"]["state"]} {s["valve"]["reason"]}')
check("tier1 limit persisted", s["settings"]["tier1_limit_s"] == 60)
check("reset reason software", s["system"]["reset_reason"] == "software")

print("valve state survives reboot (open)")
valve("open")
reboot_and_login()
s = status()
check("still open after reboot", s["valve"]["state"] == "open" and s["valve"]["reason"] == "user",
      f'{s["valve"]["state"]} {s["valve"]["reason"]}')

print("Tier 2: max volume 5 L, 100 Hz (5 L after ~24 s)")
settings(tier1_limit_s=1200, max_event_liters=5)
t0 = time.time()
pulses(100, 45)
closed_at = None
while time.time() - t0 < 45:
    if status()["valve"]["state"] == "closed" and closed_at is None:
        closed_at = time.time() - t0
    time.sleep(1)
s = status()
check("closed by tier2", s["valve"]["reason"] == "tier2", s["valve"]["detail"])
check("closed after ~24 s", closed_at is not None and 22 <= closed_at <= 27, f"{closed_at and round(closed_at)} s")
wait_idle()

print("Tier 2 snooze")
valve("open")
request("POST", "/api/snooze", {"minutes": 5})
pulses(100, 35)
time.sleep(37)
wait_idle()
s = status()
check("snoozed rule did not close", s["valve"]["state"] == "open", s["valve"]["reason"])
request("POST", "/api/snooze", {"minutes": 0})

print("Tier 1 without WiFi: limit 60 s, 50 Hz for 100 s, WiFi off for 90 s")
settings(tier1_limit_s=60, max_event_liters=0)
if status()["valve"]["state"] != "open":
    valve("open")
pulses(50, 100)
time.sleep(2)
request("POST", "/admin/test/wifi-off", {"seconds": 90}, headers={"Authorization": ADMIN})
time.sleep(100)
for _ in range(60):
    try:
        if request("GET", "/api/session", auth=False)[0] == 200:
            break
    except OSError:
        pass
    time.sleep(1)
s = status()
check("closed by tier1 while offline", s["valve"]["state"] == "closed" and s["valve"]["reason"] == "tier1",
      f'{s["valve"]["state"]} {s["valve"]["reason"]}')
check("no reboot during outage", s["system"]["uptime_s"] > 100, f'uptime {s["system"]["uptime_s"]} s')
wait_idle()

print("Tier 1 with Tier 2 hung: Tier 2 suspended, limit 60 s, 50 Hz for 100 s")
valve("open")
settings(tier1_limit_s=60, max_event_liters=5)
request("POST", "/admin/test/tier2", {"suspend": True}, headers={"Authorization": ADMIN})
t0 = time.time()
pulses(50, 100)
closed_at = None
while time.time() - t0 < 80:
    if status()["valve"]["state"] == "closed" and closed_at is None:
        closed_at = time.time() - t0
    time.sleep(1)
s = status()
check("closed by tier1, not tier2", s["valve"]["reason"] == "tier1", s["valve"]["reason"])
check("closed after ~60 s", closed_at is not None and 58 <= closed_at <= 64, f"{closed_at and round(closed_at)} s")
request("POST", "/admin/test/tier2", {"suspend": False}, headers={"Authorization": ADMIN})
pulses(0, 0)
wait_idle()

print("restore defaults")
settings(tier1_limit_s=1200, max_event_liters=0)
if status()["valve"]["state"] != "open":
    valve("open")

print(f"\n{failures} failure(s)")
sys.exit(1 if failures else 0)
