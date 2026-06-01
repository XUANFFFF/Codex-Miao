import argparse
import json
import os
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.request import Request, urlopen


DEFAULT_AUTH_PATH = Path(os.environ.get("USERPROFILE", "")) / ".codex" / "auth.json"
DEFAULT_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"
DEFAULT_MODE_PATH = Path(__file__).with_name("bridge_mode.json")


def request_json(url: str, headers: dict) -> dict:
    req = Request(url, headers=headers)
    with urlopen(req, timeout=20) as resp:
        charset = resp.headers.get_content_charset() or "utf-8"
        return json.loads(resp.read().decode(charset, errors="replace"))


def read_access_token(auth_path: Path) -> str:
    data = json.loads(auth_path.read_text(encoding="utf-8"))
    token = (data.get("tokens") or {}).get("access_token")
    if not token:
        raise RuntimeError(f"access_token not found in {auth_path}")
    return token


def fetch_wham_usage(auth_path: Path) -> dict:
    token = read_access_token(auth_path)
    headers = {
        "Authorization": f"Bearer {token}",
        "User-Agent": "cc-island-c3-wifi-bridge/1.0",
        "Accept": "application/json",
    }
    return request_json(DEFAULT_USAGE_URL, headers)


def find_window(rate_limit: dict, target_seconds: int, fallback_key: str) -> dict:
    for key in ("primary_window", "secondary_window"):
        window = rate_limit.get(key) or {}
        if int(window.get("limit_window_seconds") or 0) == target_seconds:
            return window
    return rate_limit.get(fallback_key) or {}


def remaining_percent(used_percent) -> int:
    try:
        value = float(used_percent)
    except Exception:
        return 0
    return max(0, min(100, int(round(100 - value))))


def minutes_until(reset_at) -> int:
    try:
        import time

        seconds = float(reset_at) - time.time()
    except Exception:
        return 0
    return max(0, int(seconds // 60))


def format_reset_time(reset_at) -> str:
    try:
        dt = datetime.fromtimestamp(float(reset_at)).astimezone()
        return dt.strftime("%H:%M")
    except Exception:
        return "--:--"


def build_payload_from_usage(payload: dict) -> dict:
    rate_limit = payload.get("rate_limit") or {}
    primary = find_window(rate_limit, 18000, "primary_window")
    secondary = find_window(rate_limit, 604800, "secondary_window")
    return {
        "window_pct": remaining_percent(primary.get("used_percent")),
        "week_pct": remaining_percent(secondary.get("used_percent")),
        "reset_min": minutes_until(primary.get("reset_at")),
        "reset_text": format_reset_time(primary.get("reset_at")),
    }


def read_bridge_mode(mode_path: Path) -> str:
    try:
        data = json.loads(mode_path.read_text(encoding="utf-8-sig"))
    except FileNotFoundError:
        return "live"
    except Exception:
        return "live"
    mode = str(data.get("mode") or "live").strip().lower()
    return mode if mode else "live"


def make_handler(auth_path: Path, mode_path: Path):
    class UsageHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/health":
                payload = {
                    "ok": True,
                    "bridge_state": read_bridge_mode(mode_path),
                }
                body = json.dumps(payload, ensure_ascii=True).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if self.path != "/usage":
                self.send_response(404)
                self.end_headers()
                return
            try:
                if read_bridge_mode(mode_path) == "offline":
                    payload = {
                        "display_mode": "face",
                        "bridge_state": "offline",
                    }
                else:
                    payload = build_payload_from_usage(fetch_wham_usage(auth_path))
                body = json.dumps(payload, ensure_ascii=True).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as exc:
                body = json.dumps({"error": str(exc)}, ensure_ascii=True).encode("utf-8")
                self.send_response(500)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        def log_message(self, format, *args):
            return

    return UsageHandler


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve Codex usage JSON over local HTTP.")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host")
    parser.add_argument("--port", type=int, default=8765, help="Bind port")
    parser.add_argument("--auth-path", type=Path, default=DEFAULT_AUTH_PATH, help="Path to auth.json")
    parser.add_argument("--mode-path", type=Path, default=DEFAULT_MODE_PATH, help="Path to bridge mode json")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), make_handler(args.auth_path, args.mode_path))
    print(f"Serving usage bridge on http://{args.host}:{args.port}/usage")
    server.serve_forever()


if __name__ == "__main__":
    main()
