#!/usr/bin/env python3
"""UI development without reflashing: serves firmware/web/*.html locally and forwards /api and /admin to a device.

Usage: tools/dev_proxy.py <device-ip> [--port 8080]      then open http://localhost:8080/
The page is chosen like on the device (login without a session, app with one). Host and Origin are rewritten
to the device, so its guards accept the requests. Development only: never expose this proxy.

       tools/dev_proxy.py --mock [--flowing] [--port 8080]
No device: the API is answered from tools/mock/*.json (anonymised production status) with the state kept in memory,
so settings, the valve, snooze and diagnostics can be clicked through. --flowing reports a running flow;
/?tab=settings (or history) opens that tab, for screenshots.
"""
import argparse
import copy
import datetime
import http.client
import http.server
import json
import pathlib
import sys
import time
import urllib.parse

ap = argparse.ArgumentParser()
ap.add_argument("device", nargs="?")
ap.add_argument("--port", type=int, default=8080)
ap.add_argument("--mock", action="store_true", help="answer the API from tools/mock instead of a device")
ap.add_argument("--flowing", action="store_true", help="with --mock: water is flowing")
args = ap.parse_args()
if not args.mock and not args.device:
    ap.error("a device address is required without --mock")
WEB = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "web"
# The sign-in page comes from home-idf and is rendered with the device name and accent at build time.
LOGIN = WEB.parent / "build" / "esp-idf" / "main" / "login_page" / "login.html"
TYPES = {".png": "image/png", ".webmanifest": "application/manifest+json"}
# The app page is rendered with the home-idf app shell on every request, so edits show up on reload.
HOME_IDF = next(p for p in (WEB.parent.parent.parent / "home-idf", WEB.parent / "managed_components" / "home-idf")
                if (p / "tools" / "render_page.py").exists())
sys.path.insert(0, str(HOME_IDF / "tools"))
import render_page  # noqa: E402


class Mock:
    """In-memory device for --mock: the status and events from tools/mock, changed by the POST routes."""
    DIR = pathlib.Path(__file__).resolve().parent / "mock"
    SETTINGS = ("tier1_limit_s", "pulses_per_liter", "max_event_liters", "burst_lpm", "burst_s", "night_max_liters",
                "leak_notify_min", "vacation", "vacation_max_liters")

    def __init__(self, flowing):
        self.status = json.loads((self.DIR / "status.json").read_text())
        self.events = json.loads((self.DIR / "events.json").read_text())["events"]
        self.learned = json.loads((self.DIR / "learned.json").read_text())
        self.started = time.time()
        self.flow_start = time.time() - 250 if flowing else None
        self.snooze_until = self.diag_until = 0
        self.status["valve"]["changed_at"] = int(self.started) - 30000
        for e in self.events:
            e["ts"] = int(self.started) - e.pop("ago_s")
        self.caps()

    def caps(self):
        s = self.status["settings"]
        s["tier2_max_s"] = s["tier1_limit_s"] * 3 // 4
        s["tier2_max_l"] = 12 * s["tier2_max_s"] // 60
        for k in ("max_event_liters", "night_max_liters"):
            s[k] = min(s[k], s["tier2_max_l"])
        s["vacation_max_liters"] = max(1, min(s["vacation_max_liters"], s["tier2_max_l"]))
        s["burst_s"] = min(s["burst_s"], s["tier2_max_s"])

    def get_status(self):
        st = copy.deepcopy(self.status)
        now = time.time()
        st["system"]["uptime_s"] = int(now - self.started) + 4282
        st["system"]["up_since"] = time.ctime(self.started - 4282)
        st["tier2"]["snooze_left_s"] = max(0, int(self.snooze_until - now))
        st["diag"].update(running=now < self.diag_until, left_s=max(0, int(self.diag_until - now)))
        if self.flow_start and st["valve"]["state"] == "open":
            secs = int(now - self.flow_start)
            st["flow"].update(flowing=True, lpm=8.4, seconds=secs, liters=round(secs * 8.4 / 60, 1), max_lpm=9.1,
                              tier1_remaining_s=max(0, st["settings"]["tier1_limit_s"] - secs))
        return st

    def post(self, path, body):
        st = self.status
        if path == "/api/settings":
            for k in self.SETTINGS:
                if k in body:
                    st["settings"][k] = body[k]
            for k, target in (("learned_notify", "notify"), ("learned_close_night", "close_night")):
                if k in body:
                    st["learned"][target] = bool(body[k])
            self.caps()
        elif path == "/api/valve":
            st["valve"].update(state=body.get("state", "open"), reason="user", detail="192.168.1.20",
                               changed_at=int(time.time()))
            self.events.insert(0, {"ts": int(time.time()), "type": "valve", "state": st["valve"]["state"],
                                   "reason": "user", "detail": "192.168.1.20"})
        elif path == "/api/snooze":
            self.snooze_until = time.time() + 60 * int(body.get("minutes", 0))
        elif path == "/api/diag":
            self.diag_until = time.time() + 60 * int(body.get("minutes", 0))
        elif path == "/api/logout":
            return {}
        return self.get_status()

    def get(self, path):
        if path == "/api/session":
            return {"authenticated": True, "csrf": "mock"}
        if path in ("/api/status", "/admin/status"):
            return self.get_status()
        if path == "/api/events":
            return {"events": self.events}
        if path == "/api/learned":
            hour = datetime.datetime.now().hour
            used = [v if h <= hour else 0 for h, v in enumerate(self.learned["used_l"])]
            return dict(self.learned, hour_now=hour, used_l=used)
        return None


MOCK = Mock(args.flowing) if args.mock else None


class Proxy(http.server.BaseHTTPRequestHandler):
    def mock(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(length) or b"{}") if length else {}
        data = MOCK.post(self.path, body) if self.command == "POST" else MOCK.get(self.path)
        if data is None:
            data = {"error": "not in the mock"}
        self.static(json.dumps(data).encode(), "application/json", 200 if "error" not in data else 404)

    def forward(self):
        if MOCK:
            return self.mock()
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "origin", "referer", "connection")}
        headers["Host"] = args.device
        if self.headers.get("Origin"):
            headers["Origin"] = f"http://{args.device}"
        conn = http.client.HTTPConnection(args.device, 80, timeout=20)
        conn.request(self.command, self.path, body=body, headers=headers)
        r = conn.getresponse()
        data = r.read()
        self.send_response(r.status)
        for k, v in r.getheaders():
            if k.lower() not in ("transfer-encoding", "connection", "content-length"):
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        return r.status, data

    def do_GET(self):
        if self.path.startswith(("/api/", "/admin/")):
            return self.forward()
        if self.path in ("/apple-touch-icon.png", "/manifest.webmanifest"):
            f = WEB / self.path.lstrip("/")
            return self.static(f.read_bytes(), TYPES[f.suffix])
        # Same page selection as the firmware: ask the device whether the cookie is a valid session
        if MOCK:
            authenticated = True
        else:
            conn = http.client.HTTPConnection(args.device, 80, timeout=10)
            conn.request("GET", "/api/session",
                         headers={"Host": args.device, "Cookie": self.headers.get("Cookie", "")})
            authenticated = b'"authenticated":true' in conn.getresponse().read()
        if authenticated:
            page = render_page.render((WEB / "app.html").read_text("utf-8"), "w-controller")
            tab = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get("tab", [""])[0]
            if MOCK and tab.isalpha():  # ?tab=settings opens that tab (screenshots)
                page = page.replace("</body>", f"<script>addEventListener('load', () => setTimeout(() => "
                                    f"document.querySelector('[data-tab={tab}]').click(), 300));</script></body>")
            body = page.encode()
        elif LOGIN.exists():
            body = LOGIN.read_bytes()
        else:
            raise SystemExit(f"{LOGIN} is missing: run firmware/build.sh once, the sign-in page is rendered there")
        self.static(body, "text/html; charset=utf-8")

    def do_POST(self):
        self.forward()

    def static(self, data, ctype, status=200):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *a):
        pass


print(f"http://localhost:{args.port}/ -> {args.device or 'mock (tools/mock)'}")
http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Proxy).serve_forever()
