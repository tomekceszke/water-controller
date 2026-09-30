#!/usr/bin/env python3
"""Phone screenshots of the app for the README: 390x844 CSS px at 2x, through the Chrome DevTools protocol (a plain
headless --window-size lays the page out wider than a phone and crops it).

Usage: tools/dev_proxy.py --mock --port 8765 &
       uv run --with websocket-client tools/screenshot.py "http://127.0.0.1:8765/?tab=settings" docs/img/app-settings.png
Options: --light (light theme), --full (the whole page instead of one screen).
"""
import argparse
import base64
import json
import subprocess
import tempfile
import time
import urllib.request

import websocket

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
PORT = 9333

ap = argparse.ArgumentParser()
ap.add_argument("url")
ap.add_argument("out")
ap.add_argument("--light", action="store_true")
ap.add_argument("--full", action="store_true")
args = ap.parse_args()

with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as profile:
    chrome = subprocess.Popen([CHROME, "--headless=new", f"--remote-debugging-port={PORT}", "--disable-gpu",
                               "--hide-scrollbars", f"--remote-allow-origins=http://127.0.0.1:{PORT}",
                               f"--user-data-dir={profile}", "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json"))
                break
            except OSError:
                time.sleep(0.2)
        ws = websocket.create_connection(next(t for t in tabs if t["type"] == "page")["webSocketDebuggerUrl"])
        seq = 0

        def call(method, **params):
            global seq
            seq += 1
            ws.send(json.dumps({"id": seq, "method": method, "params": params}))
            while True:
                msg = json.loads(ws.recv())
                if msg.get("id") == seq:
                    return msg.get("result", {})

        call("Emulation.setDeviceMetricsOverride", width=390, height=844, deviceScaleFactor=2, mobile=True)
        call("Emulation.setEmulatedMedia",
             features=[{"name": "prefers-color-scheme", "value": "light" if args.light else "dark"}])
        call("Page.enable")
        call("Page.navigate", url=args.url)
        time.sleep(2.5)     # the app loads its data and switches tabs after load
        shot = call("Page.captureScreenshot", format="png", captureBeyondViewport=args.full)
        with open(args.out, "wb") as f:
            f.write(base64.b64decode(shot["data"]))
    finally:
        chrome.terminate()
        chrome.wait(10)
