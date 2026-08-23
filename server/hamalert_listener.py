"""
DXMon -- HamAlert Telnet listener

Maintains a persistent Telnet connection to hamalert.org:7300, logs in, enables
JSON spot mode, and stores incoming matched spots. HamAlert itself does all the
filtering (per whatever trigger(s) Dan has configured directly on hamalert.org --
no API exists for this project to manage triggers) -- this service does no
matching of its own, it just receives and stores whatever HamAlert decides to
send.

Real protocol confirmed live 2026-08-22 via a standalone probe script
(hamalert_probe.py, not part of this service, not deployed):
  - Connect -> "HamAlert Telnet/Cluster emulation server\nlogin:\n"
  - Send "<callsign>\r\n" -> "password:\n"
  - Send "<password>\r\n" -> "Hello <callsign>, this is HamAlert\n<callsign> de HamAlert >\n"
  - Send "set/json\r\n" -> "Operation successful\n"
  - From here, matched spots arrive as JSON lines whenever HamAlert's own
    trigger(s) fire. No further commands needed for normal operation.
  - Undocumented heartbeat, confirmed by the HamAlert developer (HB9DQM) on the
    support forum: "echo <token>\r\n" -> "<token>\n". No other keepalive
    mechanism is documented or exists, per the same forum thread. Used here for
    liveness checking on an otherwise-silent connection.

Credentials (HAMALERT_USER / HAMALERT_PASS) must be set as environment
variables (Portainer stack env vars in deployment) -- never hardcoded, never
committed. This mirrors the project's existing pattern for PropMon's
TEMPEST_TOKEN and APRSMon's APRSFI_API_KEY.
"""

import json
import logging
import os
import socket
import threading
import time
from collections import deque
from datetime import datetime
from zoneinfo import ZoneInfo

from flask import Flask, jsonify

HOST = "hamalert.org"
PORT = 7300
EASTERN = ZoneInfo("America/New_York")

HAMALERT_USER = os.environ.get("HAMALERT_USER")
HAMALERT_PASS = os.environ.get("HAMALERT_PASS")
LISTEN_PORT = int(os.environ.get("PORT", 8084))

HEARTBEAT_INTERVAL_SECONDS = int(os.environ.get("HEARTBEAT_INTERVAL_SECONDS", 60))
HEARTBEAT_TIMEOUT_SECONDS = int(os.environ.get("HEARTBEAT_TIMEOUT_SECONDS", 30))
RECV_TIMEOUT_SECONDS = 10  # socket recv() timeout granularity -- drives the main loop tick

RECONNECT_BACKOFF_START = 5
RECONNECT_BACKOFF_MAX = 60

MAX_RECENT_SPOTS = 100

DISABLED_POLL_INTERVAL_SECONDS = 5  # how often the loop rechecks the enabled flag while disabled

# Persisted enable/disable state -- must survive a container restart, since the whole
# point (per Dan, 2026-08-23) is being able to disable this before a trip and have it
# stay disabled even if the NAS reboots while he's away.
STATE_FILE = os.environ.get("HAMALERT_STATE_FILE", "/app/data/state.json")

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("hamalert_listener")

app = Flask(__name__)


class VoluntaryDisconnect(Exception):
    """Raised internally when the user disables the listener while a connection is
    active -- distinct from a real network/protocol failure so it doesn't trigger
    reconnect backoff or get logged as an error."""


_lock = threading.Lock()
_state = {
    "enabled": True,  # overwritten by _load_enabled_state() at startup
    "connected": False,
    "logged_in": False,
    "json_mode": False,
    "last_connected_at": None,
    "last_spot_at": None,
    "last_heartbeat_at": None,
    "last_error": None,
    "reconnect_count": 0,
    "spots_received_total": 0,
}
_recent_spots = deque(maxlen=MAX_RECENT_SPOTS)


def _load_enabled_state():
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return bool(data.get("enabled", True))
    except FileNotFoundError:
        log.info("No existing state file at %s -- defaulting to enabled", STATE_FILE)
        return True
    except (json.JSONDecodeError, OSError) as exc:
        log.error("Failed to load state file (%s) -- defaulting to enabled: %s", STATE_FILE, exc)
        return True


def _save_enabled_state(enabled):
    try:
        os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
        tmp_path = STATE_FILE + ".tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump({"enabled": enabled}, f)
        os.replace(tmp_path, STATE_FILE)
    except OSError as exc:
        log.error("Failed to save state file (%s): %s", STATE_FILE, exc)


def _set_enabled(value):
    with _lock:
        _state["enabled"] = value
    _save_enabled_state(value)
    log.info("HamAlert listener %s", "ENABLED" if value else "DISABLED")


def _now_iso():
    return datetime.now(EASTERN).isoformat()


def _recv_until(sock, marker, timeout=15):
    """Read from sock until `marker` (bytes) appears in the accumulated buffer,
    or timeout. Returns the full buffer received (decoded), including marker."""
    sock.settimeout(timeout)
    buf = b""
    deadline = time.time() + timeout
    while marker not in buf and time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            raise ConnectionError("Connection closed by server during login")
        buf += chunk
    return buf.decode("utf-8", errors="replace")


def _login_and_enable_json(sock):
    """Runs the confirmed real login sequence (see module docstring). Raises
    ConnectionError on any unexpected step, which the caller treats as a
    reconnect-worthy failure rather than a crash."""
    banner = _recv_until(sock, b"login:")
    if "login:" not in banner.lower():
        raise ConnectionError(f"No login prompt received: {banner!r}")
    log.info("Server banner: %r", banner.strip())

    sock.sendall((HAMALERT_USER + "\r\n").encode())
    after_user = _recv_until(sock, b"password:")
    if "password:" not in after_user.lower():
        raise ConnectionError(f"Unexpected response after username: {after_user!r}")

    sock.sendall((HAMALERT_PASS + "\r\n").encode())
    after_pass = _recv_until(sock, b">")
    if "hello" not in after_pass.lower():
        raise ConnectionError(f"Login likely failed -- unexpected response: {after_pass!r}")
    log.info("Login confirmed: %r", after_pass.strip())

    sock.sendall(b"set/json\r\n")
    json_confirm = _recv_until(sock, b"\n")
    if "successful" not in json_confirm.lower():
        raise ConnectionError(f"set/json not confirmed: {json_confirm!r}")
    log.info("JSON mode enabled")


def _handle_line(line):
    """A single line of text from the persistent connection, post-login. Real
    spots arrive as JSON; anything else (prompt text, blank lines) is protocol
    noise and safely ignored -- never treated as an error."""
    line = line.strip()
    if not line:
        return
    try:
        spot = json.loads(line)
    except json.JSONDecodeError:
        log.debug("Non-JSON line (ignored): %r", line)
        return

    with _lock:
        _recent_spots.appendleft({"received_at": _now_iso(), "spot": spot})
        _state["last_spot_at"] = _now_iso()
        _state["spots_received_total"] += 1
    log.info("Spot received: %s", spot)


def _check_still_enabled_or_raise():
    """Called every tick of the inner receive loop. Raises VoluntaryDisconnect if
    the user has disabled the listener since the connection was established."""
    with _lock:
        if not _state["enabled"]:
            raise VoluntaryDisconnect("Disabled by user request")


def _connection_loop():
    backoff = RECONNECT_BACKOFF_START
    heartbeat_token_counter = 0

    while True:
        with _lock:
            enabled = _state["enabled"]

        if not enabled:
            with _lock:
                _state["connected"] = False
                _state["logged_in"] = False
                _state["json_mode"] = False
            time.sleep(DISABLED_POLL_INTERVAL_SECONDS)
            continue

        sock = None
        try:
            log.info("Connecting to %s:%s ...", HOST, PORT)
            sock = socket.create_connection((HOST, PORT), timeout=15)
            with _lock:
                _state["connected"] = True
                _state["last_connected_at"] = _now_iso()
                _state["last_error"] = None

            _login_and_enable_json(sock)
            with _lock:
                _state["logged_in"] = True
                _state["json_mode"] = True

            backoff = RECONNECT_BACKOFF_START  # reset after any successful (re)connect

            sock.settimeout(RECV_TIMEOUT_SECONDS)
            last_activity = time.time()
            last_heartbeat_sent = time.time()
            awaiting_heartbeat_token = None
            line_buf = b""

            while True:
                _check_still_enabled_or_raise()
                try:
                    chunk = sock.recv(4096)
                    if not chunk:
                        raise ConnectionError("Connection closed by server")
                    last_activity = time.time()
                    line_buf += chunk
                    while b"\n" in line_buf:
                        raw_line, line_buf = line_buf.split(b"\n", 1)
                        decoded = raw_line.decode("utf-8", errors="replace")
                        if awaiting_heartbeat_token and awaiting_heartbeat_token in decoded:
                            with _lock:
                                _state["last_heartbeat_at"] = _now_iso()
                            awaiting_heartbeat_token = None
                            continue
                        _handle_line(decoded)
                except socket.timeout:
                    pass  # normal -- just means the periodic tick fired with nothing to read

                idle_for = time.time() - last_activity
                since_last_heartbeat_sent = time.time() - last_heartbeat_sent

                if awaiting_heartbeat_token and idle_for > HEARTBEAT_TIMEOUT_SECONDS:
                    raise ConnectionError(
                        f"No heartbeat reply within {HEARTBEAT_TIMEOUT_SECONDS}s -- "
                        "treating connection as dead"
                    )

                if not awaiting_heartbeat_token and since_last_heartbeat_sent > HEARTBEAT_INTERVAL_SECONDS:
                    heartbeat_token_counter += 1
                    token = f"hb{heartbeat_token_counter}"
                    sock.sendall(f"echo {token}\r\n".encode())
                    awaiting_heartbeat_token = token
                    last_heartbeat_sent = time.time()

        except VoluntaryDisconnect:
            log.info("Closing connection -- listener was disabled by user request")
            with _lock:
                _state["connected"] = False
                _state["logged_in"] = False
                _state["json_mode"] = False
                _state["last_error"] = None
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass
            # Skip the reconnect-backoff path entirely -- loop straight back to the
            # top, which will now sleep in the "disabled" branch instead.
            continue
        except Exception as exc:  # noqa: BLE001 -- any other failure means reconnect, not a crash
            log.error("Connection error: %s", exc)
            with _lock:
                _state["connected"] = False
                _state["logged_in"] = False
                _state["json_mode"] = False
                _state["last_error"] = str(exc)
                _state["reconnect_count"] += 1
        finally:
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass

        log.info("Reconnecting in %ds...", backoff)
        time.sleep(backoff)
        backoff = min(backoff * 2, RECONNECT_BACKOFF_MAX)


@app.route("/healthz")
def healthz():
    return jsonify({"status": "ok"})


@app.route("/api/hamalert/recent")
def api_recent():
    with _lock:
        return jsonify({"count": len(_recent_spots), "spots": list(_recent_spots)})


@app.route("/api/hamalert/status")
def api_status():
    with _lock:
        return jsonify({
            "enabled": _state["enabled"],
            "connected": _state["connected"],
            "logged_in": _state["logged_in"],
        })


@app.route("/api/hamalert/enable", methods=["POST"])
def api_enable():
    _set_enabled(True)
    return jsonify({"enabled": True})


@app.route("/api/hamalert/disable", methods=["POST"])
def api_disable():
    _set_enabled(False)
    return jsonify({"enabled": False})


@app.route("/debug")
def debug():
    with _lock:
        return jsonify(dict(_state))


if __name__ == "__main__":
    if not HAMALERT_USER or not HAMALERT_PASS:
        raise SystemExit("HAMALERT_USER and HAMALERT_PASS environment variables are required.")

    _state["enabled"] = _load_enabled_state()
    log.info("Starting with enabled=%s (from %s)", _state["enabled"], STATE_FILE)

    t = threading.Thread(target=_connection_loop, daemon=True)
    t.start()
    app.run(host="0.0.0.0", port=LISTEN_PORT, threaded=True)
