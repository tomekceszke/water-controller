#!/usr/bin/env python3
"""UI development without reflashing: serves firmware/web/*.html locally and forwards /api and /admin to a device.

Usage: tools/dev_proxy.py <device-ip> [--port 8080]      then open http://localhost:8080/
The page is chosen like on the device (login without a session, app with one). Host and Origin are rewritten
to the device, so its guards accept the requests. Development only: never expose this proxy.
"""
import argparse
import http.client
import http.server
import pathlib
import sys

ap = argparse.ArgumentParser()
ap.add_argument("device")
ap.add_argument("--port", type=int, default=8080)
args = ap.parse_args()
WEB = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "web"
# The sign-in page comes from home-idf and is rendered with the device name and accent at build time.
LOGIN = WEB.parent / "build" / "esp-idf" / "main" / "login_page" / "login.html"
TYPES = {".png": "image/png", ".webmanifest": "application/manifest+json"}
# The app page is rendered with the home-idf app shell on every request, so edits show up on reload.
HOME_IDF = next(p for p in (WEB.parent.parent.parent / "home-idf", WEB.parent / "managed_components" / "home-idf")
                if (p / "tools" / "render_page.py").exists())
sys.path.insert(0, str(HOME_IDF / "tools"))
import render_page  # noqa: E402


class Proxy(http.server.BaseHTTPRequestHandler):
    def forward(self):
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
        conn = http.client.HTTPConnection(args.device, 80, timeout=10)
        conn.request("GET", "/api/session", headers={"Host": args.device, "Cookie": self.headers.get("Cookie", "")})
        authenticated = b'"authenticated":true' in conn.getresponse().read()
        if authenticated:
            body = render_page.render((WEB / "app.html").read_text("utf-8"), "w-controller").encode()
        elif LOGIN.exists():
            body = LOGIN.read_bytes()
        else:
            raise SystemExit(f"{LOGIN} is missing: run firmware/build.sh once, the sign-in page is rendered there")
        self.static(body, "text/html; charset=utf-8")

    def do_POST(self):
        self.forward()

    def static(self, data, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *a):
        pass


print(f"http://localhost:{args.port}/ -> {args.device}")
http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Proxy).serve_forever()
