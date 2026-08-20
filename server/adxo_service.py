"""
DXMon -- ADXO Ingestion Service

Polls NG3K's Announced DX Operations (ADXO) Text Version once daily, parses it into
structured entries, and serves the result as flat JSON.

IMPORTANT -- binding agreement with the data owner (Bill Feidt/NG3K, confirmed 2026-08-12):
  - Poll NO MORE THAN ONCE PER DAY.
  - Use conditional GET (If-Modified-Since) on every request.
  - ADXO is used here strictly as a DISCOVERY source (browse / decide what to watch).
    Real-time alerting for anything watched runs through HamAlert, not this service --
    DXMon does not depend on ADXO's continued existence for its core alerting function.
Do not change the polling interval or remove the conditional-GET logic without re-reading
that agreement (see DXMon's Joplin note, "ADXO Ingestion Service" section).
"""

import hashlib
import logging
import os
import re
import threading
import time
from datetime import date, datetime, timedelta
from zoneinfo import ZoneInfo

import requests
from bs4 import BeautifulSoup, NavigableString
from flask import Flask, jsonify

# --------------------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------------------

ADXO_URL = os.environ.get("ADXO_URL", "https://www.ng3k.com/Misc/adxoplain.html")
LISTEN_PORT = int(os.environ.get("PORT", 8083))

# Daily poll target time (America/New_York), scheduled after NG3K's own confirmed
# ~00:10 ET nightly rebuild.
POLL_HOUR_ET = int(os.environ.get("POLL_HOUR_ET", 0))
POLL_MINUTE_ET = int(os.environ.get("POLL_MINUTE_ET", 30))
EASTERN = ZoneInfo("America/New_York")

# Hard floor on how often we will actually contact NG3K's server, regardless of how many
# times /api/adxo/refresh is called. Protects the once-daily commitment from being broken
# by repeated manual force-refreshes during testing or from the eventual Config screen
# button. This is a safety net, not the normal polling mechanism.
MIN_REFRESH_INTERVAL_SECONDS = int(os.environ.get("MIN_REFRESH_INTERVAL_SECONDS", 3600))

REQUEST_TIMEOUT_SECONDS = 20
USER_AGENT = "N4MI-DXMon/1.0 (+https://github.com/N4MI73/n4mi-dx-monitor; contact via n4mi73@gmail.com)"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("adxo_service")

# --------------------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------------------

MONTH_NUM = {
    "Jan": 1, "Feb": 2, "Mar": 3, "Apr": 4, "May": 5, "Jun": 6,
    "Jul": 7, "Aug": 8, "Sep": 9, "Oct": 10, "Nov": 11, "Dec": 12,
}
MONTH_RE = r"(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)"

# Most specific first. Every real ADXO date-range line carries an explicit year on both
# ends when a year boundary is crossed, so no cross-entry year inheritance is needed.
PATTERN_CROSS_YEAR = re.compile(
    rf"^{MONTH_RE} (\d{{1,2}}), (\d{{4}})-{MONTH_RE} (\d{{1,2}}), (\d{{4}})$"
)
PATTERN_CROSS_MONTH = re.compile(
    rf"^{MONTH_RE} (\d{{1,2}})-{MONTH_RE} (\d{{1,2}}), (\d{{4}})$"
)
PATTERN_SINGLE_MONTH = re.compile(
    rf"^{MONTH_RE} (\d{{1,2}})-(\d{{1,2}}), (\d{{4}})$"
)

FIELD_PREFIXES = ("DXCC:", "Callsign:", "QSL:", "Source:", "Info:")

# Block-level tags: flush the current line before and after, so text never bleeds across
# them. Everything else (a, b, span, etc.) is treated as inline -- its text stays
# concatenated onto the current line rather than starting a new one. Only a literal <br>
# starts a new line. This matters because real ADXO fields routinely wrap their value in
# a link or bold tag (e.g. "Callsign: <b>J68TT</b>", "Source: <a>DXW.Net</a> (date)") --
# a naive get_text(separator="\n") splits those onto their own line and silently loses
# the field's value. Confirmed with a real bug during testing 2026-08-20 before this was
# fixed -- do not "simplify" this back to get_text(separator="\n").
_BLOCK_TAGS = {"p", "div", "tr", "table", "td", "th", "ul", "ol", "li",
               "h1", "h2", "h3", "h4", "h5", "h6"}
_SKIP_TAGS = {"script", "style", "img"}


def _extract_lines(html_text):
    """Flatten the page into logical lines, breaking only at <br> and block-tag boundaries."""
    soup = BeautifulSoup(html_text, "html.parser")
    lines = []
    current = []

    def flush():
        text = "".join(current).strip()
        text = re.sub(r"\s+", " ", text)
        current.clear()
        if text:
            lines.append(text)

    def walk(node):
        for child in node.children:
            if isinstance(child, NavigableString):
                current.append(str(child))
            elif child.name == "br":
                flush()
            elif child.name in _SKIP_TAGS:
                continue
            elif child.name in _BLOCK_TAGS:
                flush()
                walk(child)
                flush()
            else:
                # Inline tag (a, b, span, ...) -- recurse without flushing so its text
                # stays on the same logical line as its surroundings.
                walk(child)

    body = soup.body or soup
    walk(body)
    flush()
    return lines


def _parse_date_range(line):
    """Return (begin: date, end: date) or None if the line isn't a date-range line."""
    m = PATTERN_CROSS_YEAR.match(line)
    if m:
        mon1, d1, y1, mon2, d2, y2 = m.groups()
        begin = date(int(y1), MONTH_NUM[mon1], int(d1))
        end = date(int(y2), MONTH_NUM[mon2], int(d2))
        return begin, end

    m = PATTERN_CROSS_MONTH.match(line)
    if m:
        mon1, d1, mon2, d2, y = m.groups()
        y = int(y)
        begin = date(y, MONTH_NUM[mon1], int(d1))
        end = date(y, MONTH_NUM[mon2], int(d2))
        return begin, end

    m = PATTERN_SINGLE_MONTH.match(line)
    if m:
        mon, d1, d2, y = m.groups()
        y = int(y)
        begin = date(y, MONTH_NUM[mon], int(d1))
        end = date(y, MONTH_NUM[mon], int(d2))
        return begin, end

    return None


def _finalize_entry(raw, today):
    """Turn accumulated raw field strings into a served entry dict, or None if invalid."""
    if not raw.get("begin") or not raw.get("dxcc") or not raw.get("callsign"):
        log.warning("Skipping incomplete ADXO entry: %r", raw)
        return None

    begin = raw["begin"]
    end = raw["end"]

    if end < today:
        # Filtered per Dan's decision 2026-08-20: don't serve expired entries.
        return None

    entry_id = hashlib.sha1(
        f"{raw['callsign']}|{begin.isoformat()}|{end.isoformat()}|{raw['dxcc']}".encode("utf-8")
    ).hexdigest()[:12]

    return {
        "id": entry_id,
        "dxcc": raw["dxcc"],
        "callsign": raw["callsign"],
        "begin": begin.isoformat(),
        "end": end.isoformat(),
        "active": begin <= today <= end,
        "qsl": raw.get("qsl", ""),
        "source": raw.get("source", ""),
        "info": raw.get("info", ""),
    }


def parse_adxo_html(html_text, today=None):
    """
    Parse the ADXO Text Version page into a list of served entry dicts.
    Expired entries (end < today) are dropped. `today` is injectable for testing.
    """
    if today is None:
        today = date.today()

    lines = _extract_lines(html_text)

    entries = []
    current = None

    for line in lines:
        date_range = _parse_date_range(line)
        if date_range:
            # Starting a new entry -- finalize whatever we were accumulating.
            if current is not None:
                finalized = _finalize_entry(current, today)
                if finalized:
                    entries.append(finalized)
            begin, end = date_range
            current = {"begin": begin, "end": end}
            continue

        if current is None:
            # Not inside an entry yet (menu text, headers, footer, etc.) -- skip.
            continue

        matched_prefix = next((p for p in FIELD_PREFIXES if line.startswith(p)), None)
        if matched_prefix:
            key = matched_prefix[:-1].lower()  # "DXCC:" -> "dxcc"
            value = line[len(matched_prefix):].strip()
            current[key] = value
        # Any other line (stray menu/footer text between fields) is ignored rather than
        # treated as an error -- keeps the parser tolerant of minor page-structure noise.

    if current is not None:
        finalized = _finalize_entry(current, today)
        if finalized:
            entries.append(finalized)

    entries.sort(key=lambda e: e["begin"])
    return entries


# --------------------------------------------------------------------------------------
# Fetch + cache
# --------------------------------------------------------------------------------------

_lock = threading.Lock()
_state = {
    "entries": [],           # last known-good parsed entries
    "updated": None,         # ISO timestamp of our last successful fetch-and-parse
    "source_last_modified": None,  # raw Last-Modified header value from NG3K, if any
    "last_fetch_attempt": None,    # ISO timestamp of last attempted fetch (success or not)
    "last_fetch_status": None,     # "ok" | "not_modified" | "error"
    "last_error": None,
}


def _do_fetch(force=False):
    """
    Fetch + parse ADXO, updating _state. Respects MIN_REFRESH_INTERVAL_SECONDS unless
    the cache is still empty (first run). Safe to call from the scheduler thread or a
    manual /refresh request -- both funnel through here.
    """
    with _lock:
        last_attempt = _state["last_fetch_attempt"]
        has_data = bool(_state["entries"])

    if has_data and last_attempt and not force:
        elapsed = time.time() - last_attempt
        if elapsed < MIN_REFRESH_INTERVAL_SECONDS:
            log.info(
                "Skipping fetch -- last attempt %.0fs ago, floor is %ds",
                elapsed, MIN_REFRESH_INTERVAL_SECONDS,
            )
            return

    headers = {"User-Agent": USER_AGENT}
    with _lock:
        if _state["source_last_modified"]:
            headers["If-Modified-Since"] = _state["source_last_modified"]

    now_iso = datetime.now(EASTERN).isoformat()

    try:
        resp = requests.get(ADXO_URL, headers=headers, timeout=REQUEST_TIMEOUT_SECONDS)
    except requests.RequestException as exc:
        log.error("ADXO fetch failed: %s", exc)
        with _lock:
            _state["last_fetch_attempt"] = time.time()
            _state["last_fetch_status"] = "error"
            _state["last_error"] = str(exc)
        return

    with _lock:
        _state["last_fetch_attempt"] = time.time()

    if resp.status_code == 304:
        log.info("ADXO not modified since last fetch (304).")
        with _lock:
            _state["last_fetch_status"] = "not_modified"
            _state["last_error"] = None
        return

    if resp.status_code != 200:
        log.error("ADXO fetch returned unexpected status %s", resp.status_code)
        with _lock:
            _state["last_fetch_status"] = "error"
            _state["last_error"] = f"HTTP {resp.status_code}"
        return

    try:
        entries = parse_adxo_html(resp.text)
    except Exception as exc:  # noqa: BLE001 -- never let a parse bug wipe the cache
        log.exception("ADXO parse failed, keeping last known-good data")
        with _lock:
            _state["last_fetch_status"] = "error"
            _state["last_error"] = f"parse error: {exc}"
        return

    if not entries:
        # A genuinely empty result is far more likely a parse/format problem than reality
        # (ADXO is essentially never empty). Never let this silently wipe a good cache.
        log.warning("Parsed 0 entries from ADXO -- keeping last known-good data instead")
        with _lock:
            _state["last_fetch_status"] = "error"
            _state["last_error"] = "parsed 0 entries, discarded as likely bad parse"
        return

    with _lock:
        _state["entries"] = entries
        _state["updated"] = now_iso
        _state["source_last_modified"] = resp.headers.get("Last-Modified") or _state["source_last_modified"]
        _state["last_fetch_status"] = "ok"
        _state["last_error"] = None

    log.info("ADXO fetch OK -- %d entries (after filtering expired)", len(entries))


def _seconds_until_next_run():
    now = datetime.now(EASTERN)
    target = now.replace(hour=POLL_HOUR_ET, minute=POLL_MINUTE_ET, second=0, microsecond=0)
    if target <= now:
        target += timedelta(days=1)
    return (target - now).total_seconds()


def _scheduler_loop():
    # Fetch once at startup so the service isn't empty while waiting for the first
    # scheduled run (e.g. after a container restart).
    _do_fetch(force=True)
    while True:
        sleep_seconds = _seconds_until_next_run()
        log.info("Next scheduled ADXO poll in %.0f minutes", sleep_seconds / 60)
        time.sleep(sleep_seconds)
        _do_fetch(force=True)


# --------------------------------------------------------------------------------------
# Flask app
# --------------------------------------------------------------------------------------

app = Flask(__name__)


@app.route("/healthz")
def healthz():
    return jsonify({"status": "ok"})


@app.route("/api/adxo")
def api_adxo():
    with _lock:
        return jsonify({
            "updated": _state["updated"],
            "entry_count": len(_state["entries"]),
            "entries": _state["entries"],
        })


@app.route("/api/adxo/refresh", methods=["POST"])
def api_adxo_refresh():
    """
    Manual refresh trigger. Still bound by MIN_REFRESH_INTERVAL_SECONDS -- see _do_fetch.
    Useful for testing and for a future Config-screen "Force Refresh" button, without
    risking the once-daily commitment to NG3K if it's ever clicked repeatedly.
    """
    _do_fetch(force=False)
    with _lock:
        return jsonify({
            "last_fetch_status": _state["last_fetch_status"],
            "last_error": _state["last_error"],
            "entry_count": len(_state["entries"]),
        })


@app.route("/debug")
def debug():
    """Self-diagnosis endpoint, same pattern as aprsmon_mobile.py's /debug."""
    with _lock:
        return jsonify({
            "adxo_url": ADXO_URL,
            "updated": _state["updated"],
            "source_last_modified": _state["source_last_modified"],
            "last_fetch_attempt": _state["last_fetch_attempt"],
            "last_fetch_status": _state["last_fetch_status"],
            "last_error": _state["last_error"],
            "entry_count": len(_state["entries"]),
            "min_refresh_interval_seconds": MIN_REFRESH_INTERVAL_SECONDS,
            "poll_target_et": f"{POLL_HOUR_ET:02d}:{POLL_MINUTE_ET:02d}",
        })


if __name__ == "__main__":
    t = threading.Thread(target=_scheduler_loop, daemon=True)
    t.start()
    app.run(host="0.0.0.0", port=LISTEN_PORT, threaded=True)
