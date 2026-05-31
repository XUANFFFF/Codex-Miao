import argparse
import json
import os
import time
from datetime import datetime
from pathlib import Path
from urllib.request import Request, urlopen

import serial


DEFAULT_AUTH_PATH = Path(os.environ.get("USERPROFILE", "")) / ".codex" / "auth.json"
DEFAULT_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"


def build_demo_payload() -> dict:
    now = int(time.time())
    base = 35 + (now // 5) % 45
    return {
        "window_pct": min(base, 99),
        "week_pct": 12 + (now // 17) % 25,
        "cost_today": round(0.6 + ((now // 9) % 180) / 100, 2),
        "tokens_today": 180000 + ((now // 3) % 420) * 1000,
        "reset_min": max(5, 180 - ((now // 11) % 150)),
    }


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
        "User-Agent": "cc-island-c3-bridge/1.0",
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


def infer_cost_today(payload: dict) -> float:
    for key in ("cost_today", "today_cost", "daily_cost", "spend_today"):
        value = payload.get(key)
        if isinstance(value, (int, float)):
            return round(float(value), 2)
    return -1.0


def infer_tokens_today(payload: dict, primary_window: dict) -> int:
    for key in ("tokens_used", "used_tokens", "total_tokens"):
        value = payload.get(key)
        if isinstance(value, (int, float)):
            return int(value)
        value = primary_window.get(key)
        if isinstance(value, (int, float)):
            return int(value)
    return 0


def build_payload_from_usage(payload: dict) -> dict:
    rate_limit = payload.get("rate_limit") or {}
    primary = find_window(rate_limit, 18000, "primary_window")
    secondary = find_window(rate_limit, 604800, "secondary_window")

    return {
        "window_pct": remaining_percent(primary.get("used_percent")),
        "week_pct": remaining_percent(secondary.get("used_percent")),
        "cost_today": infer_cost_today(payload),
        "tokens_today": infer_tokens_today(payload, primary),
        "reset_min": minutes_until(primary.get("reset_at")),
        "reset_text": format_reset_time(primary.get("reset_at")),
    }


def load_payload(args) -> dict:
    if args.mode == "demo":
        return build_demo_payload()
    if args.json_file:
        return json.loads(args.json_file.read_text(encoding="utf-8"))
    usage = fetch_wham_usage(args.auth_path)
    return build_payload_from_usage(usage)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Send Codex card data to ESP32-C3 over serial."
    )
    parser.add_argument("--port", default="COM3", help="Serial port, e.g. COM3")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument(
        "--interval", type=float, default=60.0, help="Send interval in seconds"
    )
    parser.add_argument(
        "--mode",
        choices=("auth", "demo"),
        default="auth",
        help="Use local auth.json + official usage API, or demo data",
    )
    parser.add_argument(
        "--auth-path",
        type=Path,
        default=DEFAULT_AUTH_PATH,
        help="Path to auth.json",
    )
    parser.add_argument(
        "--json-file",
        type=Path,
        help="Optional JSON file to send instead of live auth/demo data",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="Send one payload and exit",
    )
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=2) as ser:
        time.sleep(2.0)
        print(f"Connected to {args.port} @ {args.baud}")

        while True:
            payload = load_payload(args)
            line = json.dumps(payload, ensure_ascii=True)
            ser.write((line + "\n").encode("utf-8"))
            ser.flush()

            reply = ser.readline().decode("utf-8", errors="ignore").strip()
            print(f"sent: {line}")
            if reply:
                print(f"recv: {reply}")

            if args.once:
                break

            time.sleep(args.interval)


if __name__ == "__main__":
    main()
