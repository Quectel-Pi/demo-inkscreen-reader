#!/usr/bin/env python3
"""Eye-control preview UI: serves the latest annotated camera frame over HTTP.

Enabled only when the environment variable INK_PREVIEW is set to something
other than "0".  Port comes from INK_PREVIEW_PORT (default 8088).

Endpoints
    /             HTML page (live MJPEG stream)
    /stream       multipart/x-mixed-replace MJPEG stream
    /snapshot.jpg one annotated JPEG
    /state        current tracking state as JSON

The main loop only stores references (no copy, no encoding), so the cost when
nobody is watching is essentially zero.  Drawing + JPEG encoding happens in a
worker thread and only while at least one client is connected.
"""
import json
import os
import signal
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2
import numpy as np

LEFT_EYE = [33, 133, 160, 159, 158, 144, 145, 153]
RIGHT_EYE = [362, 263, 387, 386, 385, 373, 374, 380]
LEFT_IRIS = [474, 475, 476, 477]
RIGHT_IRIS = [469, 470, 471, 472]

BOUNDARY = "qwenpavewpreview"


class Preview:
    def __init__(self, port):
        self.port = port
        self.lock = threading.Lock()
        self.frame = None
        self.result = None
        self.state = {}
        self.flash_msg = ""
        self.flash_time = 0.0
        self.clients = 0
        self.jpeg = None
        self.seq = 0
        self.state_json = {}
        self._flash_cache = ("", 99.0)
        self.gui_enabled = False
        self.gui_mode = "window"
        self.fit_mode = "cover"
        self._last_rect = None
        self._invisible = 0
        self._frames_shown = 0
        self.window_name = "Eye Control Preview"

    # ---- called by the eye tracking loop -------------------------------
    def update(self, frame, result):
        with self.lock:
            self.frame = frame
            self.result = result

    def set_state(self, **kw):
        with self.lock:
            self.state.update(kw)

    def flash(self, msg):
        with self.lock:
            self.flash_msg = msg
            self.flash_time = time.time()

    # ---- rendering ------------------------------------------------------
    def _snapshot(self):
        with self.lock:
            frame = None if self.frame is None else self.frame.copy()
            result = self.result
            state = dict(self.state)
            flash = (self.flash_msg, time.time() - self.flash_time)
        if frame is None:
            frame = np.zeros((240, 320, 3), np.uint8)
            cv2.putText(frame, "waiting for camera", (10, 125),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
            return frame, state, flash
        return self._draw(frame, result, state), state, flash

    def _draw(self, frame, result, state):
        f = frame
        h, w = f.shape[:2]
        face = False
        if result is not None and result.multi_face_landmarks:
            face = True
            lm = result.multi_face_landmarks[0].landmark
            for i in range(0, len(lm), 7):
                cv2.circle(f, (int(lm[i].x * w), int(lm[i].y * h)), 1, (90, 90, 90), -1)
            for idxs in (LEFT_EYE, RIGHT_EYE):
                pts = np.array([[int(lm[i].x * w), int(lm[i].y * h)] for i in idxs], np.int32)
                cv2.polylines(f, [pts.reshape(-1, 1, 2)], True, (0, 220, 0), 1)
            for idxs in (LEFT_IRIS, RIGHT_IRIS):
                cx = int(sum(lm[i].x for i in idxs) / len(idxs) * w)
                cy = int(sum(lm[i].y for i in idxs) / len(idxs) * h)
                cv2.circle(f, (cx, cy), 9, (0, 60, 255), 1)
                cv2.circle(f, (cx, cy), 2, (0, 60, 255), -1)

        # HUD
        cv2.rectangle(f, (0, 0), (w, 22), (0, 0, 0), -1)
        head = "FACE %s" % ("YES" if face else "NO")
        if state.get("screen_off"):
            head += "  |  E-INK OFF"
        if not state.get("initialized", True):
            head += "  |  INIT %.1fs" % state.get("init_left", 0.0)
        cv2.putText(f, head, (6, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1)

        band_h = 74
        cv2.rectangle(f, (0, h - band_h), (w, h), (0, 0, 0), -1)
        sm = state.get("smooth_iris_y")
        ref = state.get("reference_iris_y")
        rel = state.get("relative_change")
        thr = state.get("threshold", 0.008)
        buf = "".join({"DOWN": "v", "UP": "^", "NEUTRAL": "."}.get(x, "?")
                      for x in state.get("buffer", []))
        lines = [
            "irisY %s  ref %s  delta %s  thr %.4f" % (
                "-" if sm is None else "%.4f" % sm,
                "-" if ref is None else "%.4f" % ref,
                "-" if rel is None else "%+.4f" % rel,
                thr),
            "buf %-10s bottom=%s  cooldown=%.1fs" % (
                buf or "-", state.get("read_to_bottom", False),
                max(0.0, state.get("cooldown", 1.0) - (time.time() - state.get("last_action", 0.0)))),
        ]
        if rel is not None:
            bar = int(max(-0.03, min(0.03, rel)) / 0.03 * 100)
            lines.append("delta bar: " + self._bar(bar))
        for i, text in enumerate(lines):
            cv2.putText(f, text, (6, h - band_h + 16 + i * 18),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.42, (0, 255, 180), 1)

        msg, age = self._flash_cache
        if msg and age < 0.8:
            cv2.putText(f, ">> %s" % msg, (w - 210, h - band_h - 14),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
        return f

    @staticmethod
    def _bar(pos):
        n = 21
        idx = int((pos + 100) / 200 * (n - 1))
        idx = max(0, min(n - 1, idx))
        return "".join("|" if i == idx else ("-" if i == n // 2 else " ") for i in range(n))

    # ---- native window (no browser needed) ------------------------------
    @staticmethod
    def _panel_size():
        """Native resolution of the DSI/DP panel, e.g. (1280, 800)."""
        import glob
        for path in sorted(glob.glob('/sys/class/drm/card*-*/modes')):
            try:
                with open(path) as fh:
                    first = fh.readline().strip()
                if 'x' in first:
                    w, h = first.split('x')[:2]
                    return int(w), int(h)
            except Exception:
                continue
        return 1280, 800

    def enable_gui(self, mode="window", fit="cover", size=None, pos=""):
        """Open a native OpenCV window (windowed only - no fullscreen).

        mode: "0"/"off"/"none" -> no window; anything else -> windowed
        fit : "cover" (fill, crop) | "contain" (letterbox) | "stretch"
        """
        mode = (mode or "window").lower()
        self.fit_mode = (fit or "cover").lower()
        if mode in ("0", "off", "none", "false"):
            self.gui_enabled = False
            return False
        try:
            pw, ph = self._panel_size()
            w, h = size if size else (720, 480)
            self._window_size = (w, h)
            # WINDOW_AUTOSIZE: the window matches the image -> exact pixel size
            cv2.namedWindow(self.window_name, cv2.WINDOW_AUTOSIZE)
            cv2.imshow(self.window_name, np.zeros((h, w, 3), np.uint8))
            cv2.waitKey(50)
            if pos:
                try:
                    px, py = [int(v) for v in pos.replace(",", "x").split("x")[:2]]
                except Exception:
                    px, py = (pw - w) // 2, (ph - h) // 2
            else:
                px, py = (pw - w) // 2, (ph - h) // 2
            cv2.moveWindow(self.window_name, px, py)
            self.gui_mode = "window"
            self._last_rect = (w, h)
            self._target = (w, h)
            self.gui_enabled = True
            print("[INFO] Native preview window opened (%dx%d at %d,%d, fit=%s; "
                  "q/Esc = stop)" % (w, h, px, py, self.fit_mode), flush=True)
        except Exception as exc:
            self.gui_enabled = False
            print(f"[WARN] Native preview window unavailable: {exc}", flush=True)
        return self.gui_enabled

    def _fit_to_window(self, frame):
        """Scale the frame to the live window content area."""
        w, h = getattr(self, "_target", None) or (0, 0)
        if w < 2 or h < 2:
            return frame
        if self._last_rect != (w, h):
            self._last_rect = (w, h)
            print("[INFO] Preview content area: %dx%d (fit=%s)" % (w, h, self.fit_mode),
                  flush=True)
        fh, fw = frame.shape[:2]
        if self.fit_mode == "stretch":
            return cv2.resize(frame, (w, h), interpolation=cv2.INTER_LINEAR)
        scale = (max(w / fw, h / fh) if self.fit_mode == "cover"
                 else min(w / fw, h / fh))
        nw, nh = max(1, int(fw * scale)), max(1, int(fh * scale))
        resized = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_LINEAR)
        if self.fit_mode == "contain":
            canvas = np.zeros((h, w, 3), np.uint8)
            x0, y0 = (w - nw) // 2, (h - nh) // 2
            canvas[y0:y0 + nh, x0:x0 + nw] = resized
            return canvas
        x0 = max(0, (nw - w) // 2)
        y0 = max(0, (nh - h) // 2)
        return resized[y0:y0 + h, x0:x0 + w]

    def _window_closed(self):
        """True once the window has been closed (X button / WM close)."""
        try:
            visible = cv2.getWindowProperty(self.window_name, cv2.WND_PROP_VISIBLE)
        except Exception:
            visible = 0
        if self._frames_shown < 10:       # ignore startup transition
            return False
        if visible >= 1:
            self._invisible = 0
            return False
        # imshow would recreate the window on the next frame -> act at once
        print("[INFO] Preview window closed (X button), stopping demo", flush=True)
        self.gui_enabled = False
        self._request_stop()
        return True

    def show(self):
        """Draw the latest frame into the native window (main thread)."""
        if self._window_closed():
            return
        frame, _state, flash = self._snapshot()
        self._flash_cache = flash
        try:
            cv2.imshow(self.window_name, self._fit_to_window(frame))
        except Exception as exc:
            print(f"[WARN] Preview window unusable ({exc}), stopping demo", flush=True)
            self._request_stop()
            return
        self._frames_shown += 1
        key = cv2.waitKey(1) & 0xFF
        if key in (27, ord('q'), ord('Q')):
            print("[INFO] 'q'/Esc pressed in preview window, stopping demo", flush=True)
            self._request_stop()

    def _request_stop(self):
        pid_file = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            '.run', 'build.pid')
        try:
            with open(pid_file) as fh:
                pid = int(fh.read().strip())
            print(f"[INFO] Preview window: stopping demo (PID {pid})", flush=True)
            os.kill(pid, signal.SIGTERM)
            return
        except Exception as exc:
            print(f"[WARN] Stopping via pid file failed ({exc}), exiting", flush=True)
        raise SystemExit(0)

    # ---- worker ---------------------------------------------------------
    def _run_encoder(self):
        while True:
            if self.clients > 0:
                frame, state, flash = self._snapshot()
                self._flash_cache = flash
                ok, buf = cv2.imencode(".jpg", frame,
                                       [int(cv2.IMWRITE_JPEG_QUALITY), 72])
                if ok:
                    with self.lock:
                        self.jpeg = buf.tobytes()
                        self.seq += 1
                        self.state_json = dict(state)
                        self.state_json["flash"] = flash[0]
                time.sleep(0.06)          # ~15 fps
            else:
                time.sleep(0.2)

    def start(self):
        threading.Thread(target=self._run_encoder, daemon=True).start()
        handler = _make_handler(self)
        srv = ThreadingHTTPServer(("0.0.0.0", self.port), handler)
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return self


def _make_handler(preview):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.startswith("/stream"):
                self._stream()
            elif self.path.startswith("/snapshot"):
                self._once()
            elif self.path.startswith("/state"):
                body = json.dumps(getattr(preview, "state_json", {})).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                html = (b"<!doctype html><meta charset=utf-8>"
                        b"<title>Eye control preview</title>"
                        b"<body style='background:#111;color:#ddd;font-family:monospace;text-align:center'>"
                        b"<h3>Quectel-Pi H1 - eye control preview</h3>"
                        b"<img src='/stream' style='max-width:96vw'>"
                        b"<p>green = eye contours, red = irises, bottom bar = iris-Y delta vs reference</p>"
                        b"</body>")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(html)))
                self.end_headers()
                self.wfile.write(html)

        def _once(self):
            frame, _s, flash = preview._snapshot()
            preview._flash_cache = flash
            ok, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
            if not ok:
                self.send_error(500)
                return
            data = buf.tobytes()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _stream(self):
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=" + BOUNDARY)
            self.end_headers()
            preview.clients += 1
            last = -1
            try:
                while True:
                    with preview.lock:
                        jpeg, seq = preview.jpeg, preview.seq
                    if jpeg and seq != last:
                        last = seq
                        self.wfile.write(b"--" + BOUNDARY.encode() + b"\r\n")
                        self.wfile.write(b"Content-Type: image/jpeg\r\n")
                        self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(jpeg))
                        self.wfile.write(jpeg)
                        self.wfile.write(b"\r\n")
                    time.sleep(0.05)
            except Exception:
                pass
            finally:
                preview.clients -= 1

    return Handler


def is_enabled():
    return os.environ.get("INK_PREVIEW", "0") != "0"


def start(port=8088):
    return Preview(port).start()
