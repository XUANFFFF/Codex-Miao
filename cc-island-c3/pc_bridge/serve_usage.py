import argparse
import hashlib
import hmac
import ipaddress
import json
import os
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.parse import parse_qs, urlparse


DEFAULT_CODEX_HOME = Path(os.environ.get("USERPROFILE", "")) / ".codex"
DEFAULT_AUTH_PATH = Path(os.environ.get("USERPROFILE", "")) / ".codex" / "auth.json"
DEFAULT_SESSIONS_PATH = DEFAULT_CODEX_HOME / "sessions"
DEFAULT_DISCOVERY_PORT = 8766
DEFAULT_DISCOVERY_INTERVAL = 2.0
DEFAULT_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"
DEFAULT_MODE_PATH = Path(__file__).with_name("bridge_mode.json")
DEFAULT_PERMISSION_STATE_PATH = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local"))) / "CodexMiao" / "permission_state.json"
DEFAULT_SECRETS_PATH = Path(__file__).resolve().parents[1] / "src" / "wifi_secrets.h"
USAGE_CACHE_TTL_SECONDS = 30
USAGE_CACHE_RETRY_SECONDS = 30
DISCOVERY_SERVICE = "codex-miao-bridge"
DISCOVERY_VERSION = 1
ACTIVE_SESSION_STALE_SECONDS = 180
ACTIVE_SIGNAL_HOLD_SECONDS = 180
ACTIVE_TOOL_CALL_LEASE_SECONDS = 30 * 60
PERMISSION_PENDING_MAX_AGE_SECONDS = 24 * 60 * 60
SESSION_TAIL_MAX_LINES = 240
SESSION_TAIL_READ_BYTES = 262144
IS_WINDOWS = os.name == "nt"
BRIDGE_TOKEN_PATTERN = re.compile(
    r'^\s*static\s+constexpr\s+char\s+kBridgeAuthToken\[\]\s*=\s*"([^"]*)"\s*;',
    re.MULTILINE,
)

USAGE_UI_HTML = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Codex Miao 用量看板</title>
  <style>
    :root {
      --bg: #0b1220;
      --panel: rgba(10, 18, 34, 0.82);
      --panel-border: rgba(148, 163, 184, 0.2);
      --text: #f8fafc;
      --muted: #94a3b8;
      --accent: #7dd3fc;
      --accent-2: #f59e0b;
      --track: rgba(148, 163, 184, 0.16);
      --ok: #34d399;
      --idle: #fda4af;
      --danger: #fb7185;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      font-family: "Segoe UI", "PingFang SC", sans-serif;
      color: var(--text);
      background:
        radial-gradient(circle at top left, rgba(125, 211, 252, 0.2), transparent 32%),
        radial-gradient(circle at top right, rgba(245, 158, 11, 0.18), transparent 28%),
        linear-gradient(160deg, #020617 0%, #0f172a 52%, #111827 100%);
      display: grid;
      place-items: center;
      padding: 24px;
    }
    .card {
      width: min(100%, 560px);
      background: var(--panel);
      border: 1px solid var(--panel-border);
      border-radius: 28px;
      padding: 24px;
      box-shadow: 0 30px 80px rgba(0, 0, 0, 0.35);
      backdrop-filter: blur(18px);
    }
    .topbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      margin-bottom: 24px;
    }
    .title {
      font-size: 14px;
      letter-spacing: 0.14em;
      text-transform: uppercase;
      color: var(--muted);
      margin-bottom: 6px;
    }
    .headline {
      font-size: clamp(24px, 4vw, 34px);
      font-weight: 700;
    }
    .status {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 10px 14px;
      border-radius: 999px;
      background: rgba(15, 23, 42, 0.7);
      border: 1px solid rgba(148, 163, 184, 0.18);
      font-size: 14px;
      white-space: nowrap;
    }
    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: var(--ok);
      box-shadow: 0 0 14px currentColor;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 16px;
      margin-bottom: 22px;
    }
    .metric {
      background: rgba(15, 23, 42, 0.7);
      border: 1px solid rgba(148, 163, 184, 0.12);
      border-radius: 22px;
      padding: 18px;
    }
    .metric-label {
      color: var(--muted);
      font-size: 13px;
      margin-bottom: 8px;
    }
    .metric-value {
      font-size: clamp(30px, 5vw, 44px);
      line-height: 1;
      font-weight: 700;
      margin-bottom: 14px;
    }
    .bar {
      width: 100%;
      height: 12px;
      border-radius: 999px;
      background: var(--track);
      overflow: hidden;
    }
    .fill {
      height: 100%;
      border-radius: inherit;
      background: linear-gradient(90deg, var(--accent), #38bdf8);
      transition: width 220ms ease;
    }
    .fill.week {
      background: linear-gradient(90deg, var(--accent-2), #f97316);
    }
    .footer {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      flex-wrap: wrap;
      color: var(--muted);
      font-size: 14px;
    }
    .actions {
      display: flex;
      gap: 10px;
    }
    .signal-row {
      margin-top: 18px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      padding: 14px 16px;
      border-radius: 18px;
      background: rgba(15, 23, 42, 0.7);
      border: 1px solid rgba(148, 163, 184, 0.12);
      color: var(--muted);
      font-size: 14px;
    }
    .signal-value {
      color: var(--text);
      font-weight: 600;
    }
    button {
      border: 0;
      border-radius: 999px;
      padding: 10px 14px;
      background: rgba(125, 211, 252, 0.14);
      color: var(--text);
      cursor: pointer;
      font: inherit;
    }
    button:hover { background: rgba(125, 211, 252, 0.22); }
    .error {
      margin-top: 16px;
      display: none;
      color: #fecdd3;
      background: rgba(127, 29, 29, 0.35);
      border: 1px solid rgba(251, 113, 133, 0.3);
      border-radius: 18px;
      padding: 14px 16px;
      font-size: 14px;
    }
    @media (max-width: 560px) {
      .grid { grid-template-columns: 1fr; }
      .topbar { flex-direction: column; align-items: flex-start; }
      .status { white-space: normal; }
    }
  </style>
</head>
<body>
  <main class="card">
    <div class="topbar">
      <div>
        <div class="title">Codex Miao 桥接</div>
        <div class="headline">用量看板</div>
      </div>
      <div class="status">
        <span class="dot" id="status-dot"></span>
        <span id="status-text">加载中…</span>
      </div>
    </div>

    <section class="grid">
      <article class="metric">
        <div class="metric-label">当前 5 小时窗口</div>
        <div class="metric-value"><span id="window-pct">--</span>%</div>
        <div class="bar"><div class="fill" id="window-bar" style="width:0%"></div></div>
      </article>
      <article class="metric">
        <div class="metric-label">本周预算</div>
        <div class="metric-value"><span id="week-pct">--</span>%</div>
        <div class="bar"><div class="fill week" id="week-bar" style="width:0%"></div></div>
      </article>
    </section>

    <div class="footer">
      <div>
        重置时间 <strong id="reset-text">--:--</strong>
        <span id="reset-minutes"></span>
      </div>
      <div class="actions">
        <button id="refresh-btn" type="button">刷新</button>
      </div>
    </div>

    <div class="signal-row">
      <span>状态信号</span>
      <span class="signal-value" id="agent-signal">--</span>
    </div>

    <div class="error" id="error-box"></div>
  </main>

  <script>
    const els = {
      windowPct: document.getElementById('window-pct'),
      weekPct: document.getElementById('week-pct'),
      windowBar: document.getElementById('window-bar'),
      weekBar: document.getElementById('week-bar'),
      resetText: document.getElementById('reset-text'),
      resetMinutes: document.getElementById('reset-minutes'),
      statusText: document.getElementById('status-text'),
      statusDot: document.getElementById('status-dot'),
      agentSignal: document.getElementById('agent-signal'),
      errorBox: document.getElementById('error-box'),
      refreshBtn: document.getElementById('refresh-btn'),
    };

    function setStatus(text, color) {
      els.statusText.textContent = text;
      els.statusDot.style.color = color;
      els.statusDot.style.background = color;
    }

    function setError(message) {
      els.errorBox.style.display = message ? 'block' : 'none';
      els.errorBox.textContent = message || '';
    }

    function formatAgentSignal(signal) {
      switch ((signal || '').toLowerCase()) {
        case 'idle':
          return 'idle · 空闲';
        case 'thinking':
          return 'thinking · 思考中';
        case 'working':
          return 'working · 执行中';
        case 'permission':
          return 'permission · 等待确认';
        case 'blocked':
          return 'blocked · 已阻塞';
        default:
          return signal || '--';
      }
    }

    function applyUsage(data) {
      const offline = data.display_mode === 'face' || data.bridge_state === 'offline';
      els.agentSignal.textContent = formatAgentSignal(data.agent_signal);
      if (offline) {
        setStatus('已暂停，ESP 显示表情', 'var(--idle)');
        els.windowPct.textContent = '--';
        els.weekPct.textContent = '--';
        els.windowBar.style.width = '0%';
        els.weekBar.style.width = '0%';
        els.resetText.textContent = '--:--';
        els.resetMinutes.textContent = '';
        return;
      }

      if (data.usage_available === false) {
        els.windowPct.textContent = '--';
        els.weekPct.textContent = '--';
        els.windowBar.style.width = '0%';
        els.weekBar.style.width = '0%';
        els.resetText.textContent = '--:--';
        els.resetMinutes.textContent = '';
        setStatus('桥接已连接，用量暂不可用', 'var(--idle)');
        return;
      }

      const windowPct = Number(data.window_pct ?? 0);
      const weekPct = Number(data.week_pct ?? 0);
      const resetMin = Number(data.reset_min ?? 0);
      els.windowPct.textContent = windowPct;
      els.weekPct.textContent = weekPct;
      els.windowBar.style.width = `${Math.max(0, Math.min(100, windowPct))}%`;
      els.weekBar.style.width = `${Math.max(0, Math.min(100, weekPct))}%`;
      els.resetText.textContent = data.reset_text || '--:--';
      els.resetMinutes.textContent = Number.isFinite(resetMin) ? ` · 还剩 ${resetMin} 分钟` : '';
      setStatus(data.usage_stale ? '桥接已连接，用量为缓存' : '桥接已连接', 'var(--ok)');
    }

    async function loadUsage() {
      try {
        setError('');
        const res = await fetch('/usage?format=json', { cache: 'no-store' });
        const data = await res.json();
        if (!res.ok) {
          throw new Error(data.error || `Request failed: ${res.status}`);
        }
        applyUsage(data);
      } catch (error) {
        setStatus('桥接异常', 'var(--danger)');
        setError(error.message || String(error));
      }
    }

    els.refreshBtn.addEventListener('click', loadUsage);
    loadUsage();
    window.setInterval(loadUsage, 15000);
  </script>
</body>
</html>
"""


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


class UsageCache:
    def __init__(
        self,
        fetch,
        ttl_seconds: float = USAGE_CACHE_TTL_SECONDS,
        retry_seconds: float = USAGE_CACHE_RETRY_SECONDS,
        clock=time.monotonic,
    ):
        self._fetch = fetch
        self._ttl_seconds = ttl_seconds
        self._retry_seconds = retry_seconds
        self._clock = clock
        self._lock = threading.Lock()
        self._payload = None
        self._expires_at = 0.0
        self._retry_at = 0.0
        self._refreshing = False

    def get(self) -> dict:
        return self.get_snapshot()[0]

    def get_snapshot(self) -> tuple[dict | None, bool]:
        with self._lock:
            now = self._clock()
            if now >= self._expires_at and now >= self._retry_at and not self._refreshing:
                self._refreshing = True
                refresh = threading.Thread(target=self._refresh, daemon=True)
                try:
                    refresh.start()
                except RuntimeError:
                    self._refreshing = False
                    self._retry_at = now + self._retry_seconds

            stale = self._payload is not None and now >= self._expires_at
            return self._payload, stale

    def _refresh(self) -> None:
        try:
            payload = self._fetch()
        except Exception:
            with self._lock:
                self._retry_at = self._clock() + self._retry_seconds
                self._refreshing = False
            return

        with self._lock:
            self._payload = payload
            self._expires_at = self._clock() + self._ttl_seconds
            self._retry_at = 0.0
            self._refreshing = False


def read_bridge_token(secrets_path: Path) -> str:
    contents = secrets_path.read_text(encoding="utf-8-sig")
    match = BRIDGE_TOKEN_PATTERN.search(contents)
    if match is None:
        raise ValueError(f"kBridgeAuthToken is missing from {secrets_path}")

    token = match.group(1)
    if re.fullmatch(r"[0-9a-fA-F]{64}", token) is None:
        raise ValueError(f"kBridgeAuthToken in {secrets_path} must be 64 hexadecimal characters")
    return token


def request_signature(token: str, method: str, request_target: str, nonce: str) -> str:
    message = f"{method}\n{request_target}\n{nonce}".encode("utf-8")
    return hmac.new(token.encode("ascii"), message, hashlib.sha256).hexdigest()


class ChallengeStore:
    def __init__(self, ttl_seconds: float = 30, clock=time.monotonic):
        self._ttl_seconds = ttl_seconds
        self._clock = clock
        self._lock = threading.Lock()
        self._challenges: dict[tuple[str, str], float] = {}

    def issue(self, client_ip: str) -> str:
        nonce = secrets.token_hex(16)
        now = self._clock()
        with self._lock:
            self._remove_expired(now)
            while len(self._challenges) >= 1024:
                self._challenges.pop(next(iter(self._challenges)))
            self._challenges[(client_ip, nonce)] = now + self._ttl_seconds
        return nonce

    def verify_and_consume(
        self,
        client_ip: str,
        method: str,
        request_target: str,
        nonce: str,
        signature: str,
        token: str,
    ) -> bool:
        if re.fullmatch(r"[0-9a-f]{32}", nonce or "") is None:
            return False
        if re.fullmatch(r"[0-9a-f]{64}", signature or "") is None:
            return False
        expected = request_signature(token, method, request_target, nonce)
        if not hmac.compare_digest(signature, expected):
            return False

        now = self._clock()
        with self._lock:
            expires_at = self._challenges.pop((client_ip, nonce), None)
        return expires_at is not None and now < expires_at

    def _remove_expired(self, now: float) -> None:
        expired = [key for key, expires_at in self._challenges.items() if now >= expires_at]
        for key in expired:
            self._challenges.pop(key, None)


def is_authorized_request(
    client_ip: str,
    request_target: str,
    nonce: str,
    signature: str,
    expected_token: str,
    challenges: ChallengeStore,
) -> bool:
    try:
        if ipaddress.ip_address(client_ip).is_loopback:
            return True
    except ValueError:
        return False
    return challenges.verify_and_consume(
        client_ip, "GET", request_target, nonce, signature, expected_token
    )


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


def parse_event_timestamp(value: str) -> datetime | None:
    text = str(value or "").strip()
    if not text:
        return None
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        return datetime.fromisoformat(text)
    except ValueError:
        return None


def read_recent_session_lines(path: Path, max_lines: int = SESSION_TAIL_MAX_LINES) -> list[str]:
    with path.open("rb") as handle:
        handle.seek(0, os.SEEK_END)
        file_size = handle.tell()
        handle.seek(max(0, file_size - SESSION_TAIL_READ_BYTES))
        chunk = handle.read().decode("utf-8", errors="replace")

    lines = [line for line in chunk.splitlines() if line.strip()]
    if len(lines) > max_lines:
        return lines[-max_lines:]
    return lines


def _permission_state_lock(path: Path):
    lock_path = path.with_suffix(path.suffix + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    handle = lock_path.open("a+b")
    try:
        if os.name == "nt":
            import msvcrt

            handle.seek(0)
            if not handle.read(1):
                handle.write(b"\0")
                handle.flush()
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl

            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        return handle
    except Exception:
        handle.close()
        raise


def _unlock_permission_state(handle) -> None:
    try:
        if os.name == "nt":
            import msvcrt

            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
    finally:
        handle.close()


def _replace_with_retry(source: Path, destination: Path) -> None:
    attempts = 4 if IS_WINDOWS else 1
    for attempt in range(attempts):
        try:
            os.replace(source, destination)
            return
        except PermissionError:
            if attempt + 1 == attempts:
                raise
            time.sleep(0.01 * (attempt + 1))


def apply_permission_hook_event(
    event: dict,
    state_path: Path = DEFAULT_PERMISSION_STATE_PATH,
    *,
    clock=time.time,
) -> None:
    event_name = str(event.get("hook_event_name") or "")
    session_id = str(event.get("session_id") or "").strip()
    turn_id = str(event.get("turn_id") or "").strip()
    if not session_id:
        return

    lock_handle = _permission_state_lock(state_path)
    try:
        try:
            state = json.loads(state_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            state = {}
        pending = [item for item in state.get("pending", []) if isinstance(item, dict)]

        if event_name == "PermissionRequest":
            pending = [
                item
                for item in pending
                if not (item.get("session_id") == session_id and item.get("turn_id") == turn_id)
            ]
            pending.append(
                {
                    "session_id": session_id,
                    "turn_id": turn_id,
                    "created_at": clock(),
                }
            )
        elif event_name == "PostToolUse" and turn_id:
            pending = [
                item
                for item in pending
                if not (item.get("session_id") == session_id and item.get("turn_id") == turn_id)
            ]
        elif event_name in {
            "SessionStart",
            "SessionEnd",
            "UserPromptSubmit",
            "Stop",
            "Interrupt",
        }:
            pending = [item for item in pending if item.get("session_id") != session_id]
        else:
            return

        state_path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=state_path.parent,
            prefix=state_path.name + ".",
            suffix=".tmp",
            delete=False,
        ) as handle:
            json.dump({"pending": pending}, handle, separators=(",", ":"))
            temp_path = Path(handle.name)
        try:
            _replace_with_retry(temp_path, state_path)
        finally:
            temp_path.unlink(missing_ok=True)
    finally:
        _unlock_permission_state(lock_handle)


def has_pending_permission(
    state_path: Path = DEFAULT_PERMISSION_STATE_PATH,
    *,
    now: float | None = None,
) -> bool:
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False

    timestamp = time.time() if now is None else now
    for item in state.get("pending", []):
        if not isinstance(item, dict):
            continue
        try:
            age = timestamp - float(item.get("created_at"))
        except (TypeError, ValueError):
            continue
        if 0 <= age <= PERMISSION_PENDING_MAX_AGE_SECONDS:
            return True
    return False


def find_codex_session_paths(sessions_root: Path = DEFAULT_SESSIONS_PATH) -> list[Path]:
    if not sessions_root.is_dir():
        return []

    minimum_mtime = time.time() - ACTIVE_TOOL_CALL_LEASE_SECONDS
    candidates: list[tuple[float, Path]] = []
    for path in sessions_root.rglob("*.jsonl"):
        try:
            modified_at = path.stat().st_mtime
        except OSError:
            continue
        if modified_at >= minimum_mtime:
            candidates.append((modified_at, path))
    candidates.sort(key=lambda candidate: candidate[0], reverse=True)
    return [path for _, path in candidates]


def find_latest_codex_session_path(sessions_root: Path = DEFAULT_SESSIONS_PATH) -> Path | None:
    paths = find_codex_session_paths(sessions_root)
    return paths[0] if paths else None


def derive_agent_signal_from_codex_sessions(
    sessions_root: Path = DEFAULT_SESSIONS_PATH,
    permission_state_path: Path = DEFAULT_PERMISSION_STATE_PATH,
) -> str:
    if has_pending_permission(permission_state_path):
        return "permission"

    session_paths = find_codex_session_paths(sessions_root)
    if not session_paths:
        return "idle"

    state_priority = {"idle": 0, "blocked": 1, "thinking": 2, "working": 3, "permission": 4}
    states = (derive_agent_signal_from_session_path(path) for path in session_paths)
    return max(states, key=lambda state: state_priority.get(state, 0), default="idle")


def derive_agent_signal_from_session_path(session_path: Path) -> str:

    try:
        session_age_seconds = (datetime.now(timezone.utc).timestamp() - session_path.stat().st_mtime)
    except OSError:
        return "idle"

    latest_started: datetime | None = None
    latest_completed: datetime | None = None
    latest_reasoning: datetime | None = None
    latest_token_count: datetime | None = None
    latest_user_message: datetime | None = None
    latest_aborted: tuple[datetime, str] | None = None
    pending_tool_calls: dict[str, datetime] = {}

    for raw_line in read_recent_session_lines(session_path):
        try:
            record = json.loads(raw_line)
        except json.JSONDecodeError:
            continue

        event_time = parse_event_timestamp(record.get("timestamp"))
        if event_time is None:
            continue

        record_type = record.get("type")
        payload = record.get("payload") or {}
        payload_type = payload.get("type")

        if record_type == "event_msg" and payload_type == "task_started":
            latest_started = max(latest_started or event_time, event_time)
        elif record_type == "event_msg" and payload_type == "task_complete":
            latest_completed = max(latest_completed or event_time, event_time)
        elif record_type == "event_msg" and payload_type == "turn_aborted":
            reason = str(payload.get("reason") or "").strip().lower()
            if latest_aborted is None or event_time > latest_aborted[0]:
                latest_aborted = (event_time, reason)
        elif record_type == "event_msg" and payload_type == "token_count":
            latest_token_count = max(latest_token_count or event_time, event_time)
        elif record_type == "event_msg" and payload_type == "user_message":
            latest_user_message = max(latest_user_message or event_time, event_time)
        elif record_type == "response_item":
            if payload_type == "reasoning":
                latest_reasoning = max(latest_reasoning or event_time, event_time)
            elif payload_type in {"function_call", "custom_tool_call"}:
                call_id = str(payload.get("call_id") or "").strip()
                if call_id:
                    pending_tool_calls[call_id] = event_time
            elif payload_type in {"function_call_output", "custom_tool_call_output"}:
                call_id = str(payload.get("call_id") or "").strip()
                if call_id:
                    pending_tool_calls.pop(call_id, None)

    now_utc = datetime.now(timezone.utc)
    min_time = datetime.min.replace(tzinfo=timezone.utc)
    latest_started_or_min = latest_started or min_time
    latest_completed_or_min = latest_completed or min_time

    def is_recent(event_time: datetime | None, max_age: float = ACTIVE_SIGNAL_HOLD_SECONDS) -> bool:
        if event_time is None:
            return False
        age_seconds = (now_utc - event_time).total_seconds()
        return 0 <= age_seconds <= max_age

    if latest_aborted is not None:
        aborted_time, aborted_reason = latest_aborted
        if aborted_time >= max(latest_started_or_min, latest_completed_or_min):
            interrupted = aborted_reason in {
                "interrupted",
                "cancelled",
                "canceled",
                "aborted_by_user",
            }
            if not interrupted:
                return "blocked"
            if latest_user_message is None or latest_user_message <= aborted_time:
                return "idle"

    active_tool_call = any(
        call_time >= latest_started_or_min
        and call_time > latest_completed_or_min
        and (latest_aborted is None or call_time > latest_aborted[0])
        and is_recent(call_time, ACTIVE_TOOL_CALL_LEASE_SECONDS)
        for call_time in pending_tool_calls.values()
    )
    if active_tool_call:
        return "working"

    completed_current_turn = latest_completed is not None and (
        latest_started is None or latest_completed >= latest_started
    )
    if completed_current_turn and (
        latest_user_message is None or latest_user_message <= latest_completed
    ):
        return "idle"

    if session_age_seconds > ACTIVE_SESSION_STALE_SECONDS:
        return "idle"

    last_active_detail = max(
        [dt for dt in (latest_reasoning, latest_token_count) if dt is not None],
        default=None,
    )

    if completed_current_turn:
        current_turn_started = latest_user_message or min_time
    else:
        current_turn_started = latest_started or min_time
        if latest_user_message is not None and latest_user_message > latest_completed_or_min:
            current_turn_started = max(current_turn_started, latest_user_message)

    if (
        last_active_detail is not None
        and last_active_detail >= current_turn_started
        and last_active_detail > latest_completed_or_min
        and is_recent(last_active_detail)
    ):
        return "working"

    if (
        latest_user_message is not None
        and latest_user_message >= current_turn_started
        and latest_user_message > latest_completed_or_min
        and is_recent(latest_user_message)
    ):
        return "thinking"

    return "thinking" if latest_started is not None and latest_started > latest_completed_or_min else "idle"


def build_usage_fields(payload: dict) -> dict:
    rate_limit = payload.get("rate_limit") or {}
    primary = find_window(rate_limit, 18000, "primary_window")
    secondary = find_window(rate_limit, 604800, "secondary_window")
    return {
        "window_pct": remaining_percent(primary.get("used_percent")),
        "week_pct": remaining_percent(secondary.get("used_percent")),
        "reset_min": minutes_until(primary.get("reset_at")),
        "reset_text": format_reset_time(primary.get("reset_at")),
    }


def build_payload_from_usage(payload: dict) -> dict:
    return {
        **build_usage_fields(payload),
        "agent_signal": derive_agent_signal_from_codex_sessions(),
        "usage_available": True,
        "usage_stale": False,
    }


def build_current_payload(auth_path: Path, mode_path: Path, usage_cache: UsageCache | None = None) -> dict:
    if read_bridge_mode(mode_path) == "offline":
        return {
            "display_mode": "face",
            "bridge_state": "offline",
            "agent_signal": "idle",
        }

    result = {
        "agent_signal": derive_agent_signal_from_codex_sessions(),
        "usage_available": False,
        "usage_stale": False,
    }
    if usage_cache is None:
        return result

    usage_payload, usage_stale = usage_cache.get_snapshot()
    if usage_payload is not None:
        result.update(build_usage_fields(usage_payload))
        result["usage_available"] = True
        result["usage_stale"] = usage_stale
    return result


def read_bridge_mode(mode_path: Path) -> str:
    try:
        data = json.loads(mode_path.read_text(encoding="utf-8-sig"))
    except FileNotFoundError:
        return "live"
    except Exception as exc:
        print(f"Bridge mode read failed for {mode_path}: {exc}", file=sys.stderr)
        return "live"
    mode = str(data.get("mode") or "live").strip().lower()
    return mode if mode else "live"


def is_preferred_lan_ipv4(address: str) -> bool:
    try:
        ip = ipaddress.ip_address(address)
    except ValueError:
        return False

    if ip.version != 4 or ip.is_loopback or ip.is_link_local:
        return False

    if ip in ipaddress.ip_network("198.18.0.0/15"):
        return False

    return ip.is_private


def parse_ipconfig_ipv4_interfaces(output: str) -> list[tuple[str, str | None]]:
    lines = output.splitlines()
    interfaces: list[tuple[str, str | None]] = []
    for index, raw_line in enumerate(lines):
        if re.search(r"\bIPv4\b", raw_line, re.IGNORECASE) is None:
            continue

        addresses = re.findall(r"(\d+\.\d+\.\d+\.\d+)", raw_line)
        if not addresses:
            continue
        host = addresses[-1]
        subnet_mask = None
        for mask_line in lines[index + 1 :]:
            if re.search(r"\bIPv4\b", mask_line, re.IGNORECASE):
                break
            for candidate in re.findall(r"(\d+\.\d+\.\d+\.\d+)", mask_line):
                try:
                    network = ipaddress.IPv4Network(f"0.0.0.0/{candidate}", strict=False)
                except ValueError:
                    continue
                if network.prefixlen < 31:
                    subnet_mask = candidate
                    break
            if subnet_mask is not None:
                break
        interfaces.append((host, subnet_mask))
    return interfaces


def discover_local_ipv4_addresses() -> list[str]:
    candidates = []

    try:
        hostname = socket.gethostname()
        for family, _, _, _, sockaddr in socket.getaddrinfo(hostname, None, socket.AF_INET):
            host = sockaddr[0]
            if host not in candidates:
                candidates.append(host)
    except OSError:
        pass

    try:
        ipconfig = subprocess.run(
            ["ipconfig"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            check=False,
        )
        for host, _ in parse_ipconfig_ipv4_interfaces(ipconfig.stdout):
            if host not in candidates:
                candidates.append(host)
    except OSError:
        pass

    return candidates


def choose_discovery_host(preferred_host: str | None = None) -> str:
    candidates = discover_local_ipv4_addresses()

    if preferred_host:
        if not is_preferred_lan_ipv4(preferred_host):
            raise ValueError("--discovery-host must be a non-loopback private IPv4 address")
        if preferred_host not in candidates:
            raise ValueError(f"--discovery-host {preferred_host} is not assigned to a local interface")
        return preferred_host

    for address in candidates:
        if is_preferred_lan_ipv4(address):
            return address

    for address in candidates:
        try:
            ip = ipaddress.ip_address(address)
        except ValueError:
            continue

        if ip.version == 4 and not ip.is_loopback and not ip.is_link_local:
            return address

    return "127.0.0.1"


def compute_directed_broadcast(host: str) -> str | None:
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return None

    if ip.version != 4 or ip.is_loopback or ip.is_link_local:
        return None

    try:
        ipconfig = subprocess.run(
            ["ipconfig"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            check=False,
        )
    except OSError:
        return None

    for interface_host, subnet_mask in parse_ipconfig_ipv4_interfaces(ipconfig.stdout):
        if interface_host != host or subnet_mask is None:
            continue
        try:
            network = ipaddress.IPv4Network(f"{host}/{subnet_mask}", strict=False)
        except ValueError:
            return None
        return str(network.broadcast_address)
    return None


def build_discovery_beacon(port: int, host: str, bridge_token: str) -> bytes:
    signature_message = f"{DISCOVERY_SERVICE}\n{DISCOVERY_VERSION}\n{host}\n{port}".encode("ascii")
    payload = {
        "service": DISCOVERY_SERVICE,
        "version": DISCOVERY_VERSION,
        "port": port,
        "host": host,
        "signature": hmac.new(
            bridge_token.encode("ascii"), signature_message, hashlib.sha256
        ).hexdigest(),
    }
    return json.dumps(payload, ensure_ascii=True, separators=(",", ":")).encode("utf-8")


def start_discovery_broadcaster(
    stop_event: threading.Event,
    *,
    bind_host: str,
    http_port: int,
    bridge_token: str,
    discovery_port: int,
    interval: float,
) -> threading.Thread:
    beacon = build_discovery_beacon(http_port, bind_host, bridge_token)
    destinations = ["255.255.255.255"]
    directed_broadcast = compute_directed_broadcast(bind_host)
    if directed_broadcast and directed_broadcast not in destinations:
        destinations.append(directed_broadcast)

    def run() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                if bind_host and bind_host != "0.0.0.0":
                    sock.bind((bind_host, 0))
                while not stop_event.is_set():
                    for destination in destinations:
                        try:
                            sock.sendto(beacon, (destination, discovery_port))
                        except OSError as exc:
                            print(
                                "Discovery broadcast failed "
                                f"(service={DISCOVERY_SERVICE}, bind_host={bind_host}, "
                                f"destination={destination}, http_port={http_port}, "
                                f"discovery_port={discovery_port}): {exc}",
                                file=sys.stderr,
                            )
                    if stop_event.wait(interval):
                        break
        except OSError as exc:
            print(
                f"Discovery broadcaster could not start on UDP port {discovery_port}: {exc}",
                file=sys.stderr,
            )

    thread = threading.Thread(target=run, name="discovery-broadcaster", daemon=True)
    thread.start()
    return thread


class BridgeHTTPServer(ThreadingHTTPServer):
    daemon_threads = True


def make_handler(auth_path: Path, mode_path: Path, bridge_token: str):
    usage_cache = UsageCache(lambda: fetch_wham_usage(auth_path))
    challenges = ChallengeStore()

    class UsageHandler(BaseHTTPRequestHandler):
        def write_json(self, status: int, payload: dict) -> None:
            body = json.dumps(payload, ensure_ascii=True).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def write_html(self, html: str) -> None:
            body = html.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def wants_html(self, query: dict[str, list[str]]) -> bool:
            fmt = (query.get("format") or [""])[0].strip().lower()
            if fmt == "json":
                return False
            if fmt == "html":
                return True
            accept = self.headers.get("Accept", "")
            user_agent = self.headers.get("User-Agent", "")
            return "text/html" in accept or "Mozilla/" in user_agent

        def do_GET(self):
            parsed = urlparse(self.path)
            query = parse_qs(parsed.query)

            if parsed.path == "/challenge":
                nonce = challenges.issue(self.client_address[0])
                self.write_json(200, {"nonce": nonce})
                return

            if parsed.path in ("/", "/health", "/usage") and not is_authorized_request(
                self.client_address[0],
                self.path,
                self.headers.get("X-Codex-Miao-Nonce", ""),
                self.headers.get("X-Codex-Miao-Signature", ""),
                bridge_token,
                challenges,
            ):
                self.write_json(403, {"error": "unauthorized"})
                return

            if parsed.path == "/":
                self.write_html(USAGE_UI_HTML)
                return

            if parsed.path == "/health":
                self.write_json(
                    200,
                    {
                        "ok": True,
                        "bridge_state": read_bridge_mode(mode_path),
                    },
                )
                return

            if parsed.path != "/usage":
                self.send_response(404)
                self.end_headers()
                return
            try:
                if self.wants_html(query):
                    self.write_html(USAGE_UI_HTML)
                    return
                self.write_json(200, build_current_payload(auth_path, mode_path, usage_cache))
            except Exception as exc:
                if self.wants_html(query):
                    self.write_html(USAGE_UI_HTML)
                    return
                self.write_json(500, {"error": str(exc)})

        def log_message(self, format, *args):
            return

    return UsageHandler


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve Codex usage JSON over local HTTP.")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host")
    parser.add_argument("--port", type=int, default=8765, help="Bind port")
    parser.add_argument(
        "--discovery-port",
        type=int,
        default=DEFAULT_DISCOVERY_PORT,
        help="UDP broadcast port for discovery beacons",
    )
    parser.add_argument(
        "--discovery-interval",
        type=float,
        default=DEFAULT_DISCOVERY_INTERVAL,
        help="Seconds between discovery beacons",
    )
    parser.add_argument(
        "--discovery-host",
        default=os.environ.get("CODEX_MIAO_DISCOVERY_HOST"),
        help="Local private IPv4 address to advertise when multiple network interfaces are present",
    )
    parser.add_argument("--auth-path", type=Path, default=DEFAULT_AUTH_PATH, help="Path to auth.json")
    parser.add_argument("--mode-path", type=Path, default=DEFAULT_MODE_PATH, help="Path to bridge mode json")
    parser.add_argument(
        "--secrets-path",
        type=Path,
        default=DEFAULT_SECRETS_PATH,
        help="Path to wifi_secrets.h containing kBridgeAuthToken",
    )
    args = parser.parse_args()
    try:
        discovery_host = choose_discovery_host(args.discovery_host)
    except ValueError as exc:
        parser.error(str(exc))
    bridge_token = read_bridge_token(args.secrets_path)

    server = BridgeHTTPServer(
        (args.host, args.port), make_handler(args.auth_path, args.mode_path, bridge_token)
    )
    stop_event = threading.Event()
    broadcaster = start_discovery_broadcaster(
        stop_event,
        bind_host=discovery_host,
        http_port=args.port,
        bridge_token=bridge_token,
        discovery_port=args.discovery_port,
        interval=args.discovery_interval,
    )
    print(f"Serving usage bridge on http://{args.host}:{args.port}/usage")
    print(f"Discovery host: {discovery_host}")
    directed_broadcast = compute_directed_broadcast(discovery_host)
    if directed_broadcast:
        print(f"Directed broadcast: {directed_broadcast}")
    print(f"Broadcasting discovery beacons on UDP port {args.discovery_port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        server.shutdown()
        broadcaster.join(timeout=5)
        server.server_close()


if __name__ == "__main__":
    main()
