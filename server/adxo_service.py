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

import csv
import hashlib
import json
import logging
import math
import os
import re
import threading
import time
import uuid
from datetime import date, datetime, timedelta
from zoneinfo import ZoneInfo

import requests
from bs4 import BeautifulSoup, NavigableString
from flask import Flask, jsonify, redirect, render_template, request, url_for

# --------------------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------------------

ADXO_URL = os.environ.get("ADXO_URL", "https://www.ng3k.com/Misc/adxoplain.html")

# The HamAlert listener is a separate service/container (see docker-compose-hamalert.yml)
# -- reached over the LAN like a browser would, not via a shared Docker network, since
# it's deployed as its own Portainer stack. Configurable rather than hardcoded, per the
# project's own "keep server addresses configurable" convention.
HAMALERT_LISTENER_URL = os.environ.get("HAMALERT_LISTENER_URL", "http://192.168.6.29:8084")
HAMALERT_REQUEST_TIMEOUT_SECONDS = 5
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

# Watched-list persistence. Must live under a Docker volume (see docker-compose.yml) --
# otherwise every container rebuild silently wipes the watchlist.
WATCHED_FILE = os.environ.get("WATCHED_FILE", "/app/data/watched.json")

# Needed -- Dan's small, manually-curated list of DX targets: either a whole
# never-confirmed entity (band/mode both blank) or a specific band/mode gap on an
# entity he's already confirmed (band and/or mode set). Unified 2026-09-04 -- this
# used to be split between a CSV-auto-populated "Needed" (all of no_confirms.csv)
# and a separately-curated "Wanted" (band/mode gaps only); in practice the auto-
# populated side wasn't useful (~68 entries, mostly permanently empty), so both
# collapsed into one curated list matching exactly what Dan sets up HamAlert
# triggers for. Deliberately NOT sourced from HamAlert's own "Band slots" condition,
# which draws from Club Log's automated missing-slot data -- that inherits the same
# worked-vs-LoTW-confirmed precision gap already documented for the old Needed-entity
# feature ("0-slots mystery", 2026-08-16). Stays fully Dan-curated for the same
# reason no_confirms.csv itself is sourced from his own LoTW export, not an API.
#
# File renamed from WANTED_FILE/wanted.json (2026-09-04). On startup, if NEEDED_FILE
# doesn't exist yet but the old wanted.json does, _load_needed() migrates it
# automatically -- see that function for details. No manual file operation needed.
NEEDED_FILE = os.environ.get("NEEDED_FILE", "/app/data/needed.json")
_LEGACY_WANTED_FILE = os.environ.get("WANTED_FILE", "/app/data/wanted.json")

# Trigger Builder -- never-confirmed entity seed list. A static file Dan replaces
# manually whenever he re-exports from his LoTW DXCC Credit Analyzer (no live sync --
# matches the already-decided "Dan's own export is the source of truth" design).
NO_CONFIRMS_FILE = os.environ.get("NO_CONFIRMS_FILE", "no_confirms.csv")

# Above this many selected entities, a single trigger risks HamAlert's 10,000-
# spots/day auto-disable ceiling (confirmed real, 2026-08-18 -- a 61-entity trigger
# hit it). Below the threshold, one combined recipe is fine. Callsign-based triggers
# never split -- Dan's own stated usage (2-3 watched callsigns, rarely more) plus the
# inherently low volume of exact-callsign matching means splitting isn't needed there.
ENTITY_SPLIT_THRESHOLD = 5

# Beam heading -- Dan's station grid square, configurable rather than hardcoded per the
# project's own convention. Accuracy ceiling already accepted (2026-08-15): country-file
# lookups typically resolve to an entity's approximate reference point, not the exact
# operator location -- judged sufficient given Dan's hexbeam beamwidth.
STATION_GRID = os.environ.get("STATION_GRID", "EM83")

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
# Watched list -- curation UI's core write path
# --------------------------------------------------------------------------------------

_watched_lock = threading.Lock()
_watched = []  # list of dicts, loaded from WATCHED_FILE at startup


def _load_watched():
    global _watched
    try:
        with open(WATCHED_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, list):
            _watched = data
            log.info("Loaded %d watched entries from %s", len(_watched), WATCHED_FILE)
        else:
            log.error("Watched file did not contain a list -- starting empty, not overwriting")
    except FileNotFoundError:
        log.info("No existing watched file at %s -- starting empty", WATCHED_FILE)
    except (json.JSONDecodeError, OSError) as exc:
        # Never let a corrupt/unreadable file silently wipe what might still be good data
        # on disk. Start the in-memory list empty for this run, but do NOT save over the
        # file until a real add/remove happens -- gives Dan a chance to notice and recover
        # the file manually if this ever fires unexpectedly.
        log.error("Failed to load watched file (%s) -- starting empty in memory, "
                   "NOT overwriting the file on disk: %s", WATCHED_FILE, exc)
        _watched = []


def _save_watched():
    """Atomic write -- write to a temp file, then replace, so a crash mid-write can't
    corrupt the real file."""
    os.makedirs(os.path.dirname(WATCHED_FILE), exist_ok=True)
    tmp_path = WATCHED_FILE + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(_watched, f, indent=2)
    os.replace(tmp_path, WATCHED_FILE)


def _watched_by_adxo_id():
    """dict of source_adxo_id -> list of watched entries, for the browse page to show
    'already watching X from this entry' inline."""
    result = {}
    with _watched_lock:
        for w in _watched:
            sid = w.get("source_adxo_id")
            if sid:
                result.setdefault(sid, []).append(w)
    return result


def _add_watched(callsign, dxcc, source_adxo_id=None, note=""):
    callsign = (callsign or "").strip()
    dxcc = (dxcc or "").strip()
    if not callsign or not dxcc:
        return None, "Callsign and DXCC entity are both required."

    entry = {
        "id": uuid.uuid4().hex[:12],
        "callsign": callsign,
        "dxcc": dxcc,
        "source_adxo_id": (source_adxo_id or "").strip() or None,
        "note": (note or "").strip(),
        "added": datetime.now(EASTERN).isoformat(),
    }
    with _watched_lock:
        _watched.append(entry)
        _save_watched()
    return entry, None


def _remove_watched(watched_id):
    with _watched_lock:
        before = len(_watched)
        _watched[:] = [w for w in _watched if w["id"] != watched_id]
        removed = len(_watched) != before
        if removed:
            _save_watched()
    return removed


# --------------------------------------------------------------------------------------
# Needed list -- Dan's small, manually-curated DX target list. Unified 2026-09-04 from
# the former separate Needed (auto-populated from no_confirms.csv) and Wanted (band/mode
# gaps only) -- see NEEDED_FILE's module-level comment. Mirrors the Watched list's
# structure exactly (own file, own lock, same load/save/add/remove pattern) --
# deliberately not merged into the Watched data model, since the matching key is
# fundamentally different (entity name +/- band/mode, not a callsign).
#
# Entry shape: {id, entity, band, mode, note, added}. band and mode are each
# independently optional -- BOTH blank means "whole entity, any band/mode" (the old
# Needed semantic); either set means "this specific slot" (the old Wanted semantic).
# There is no stored type/category field -- see _needed_entry_kind() below, which
# derives ENTITY vs. SLOT from whether band/mode are present, so there's only ever
# one source of truth for an entry's kind.
# --------------------------------------------------------------------------------------

_needed_lock = threading.Lock()
_needed = []  # list of dicts, loaded from NEEDED_FILE at startup


def _load_needed():
    """Loads NEEDED_FILE. If it doesn't exist yet but the old WANTED_FILE does
    (pre-2026-09-04 deployment), migrates it automatically: loads the old file,
    writes it out as the new one, and continues -- no manual rename step needed on
    deploy. This is a one-time migration; once needed.json exists, wanted.json is
    never consulted again."""
    global _needed
    try:
        with open(NEEDED_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, list):
            _needed = data
            log.info("Loaded %d needed entries from %s", len(_needed), NEEDED_FILE)
        else:
            log.error("Needed file did not contain a list -- starting empty, not overwriting")
        return
    except FileNotFoundError:
        pass  # fall through to migration check below
    except (json.JSONDecodeError, OSError) as exc:
        log.error("Failed to load needed file (%s) -- starting empty in memory, "
                   "NOT overwriting the file on disk: %s", NEEDED_FILE, exc)
        _needed = []
        return

    # NEEDED_FILE doesn't exist -- check for the pre-rename wanted.json and migrate
    # it forward if found, so Dan's two real existing entries (Sierra Leone,
    # Singapore) and their live HamAlert triggers keep working across the rename
    # with zero manual steps.
    try:
        with open(_LEGACY_WANTED_FILE, "r", encoding="utf-8") as f:
            legacy_data = json.load(f)
        if isinstance(legacy_data, list):
            _needed = legacy_data
            _save_needed()
            log.info("Migrated %d entries from legacy %s to %s",
                      len(_needed), _LEGACY_WANTED_FILE, NEEDED_FILE)
        else:
            log.error("Legacy wanted file did not contain a list -- starting empty")
    except FileNotFoundError:
        log.info("No existing needed file at %s (and no legacy wanted file to migrate) "
                  "-- starting empty", NEEDED_FILE)
    except (json.JSONDecodeError, OSError) as exc:
        log.error("Failed to read legacy wanted file (%s) -- starting empty in memory: %s",
                   _LEGACY_WANTED_FILE, exc)
        _needed = []


def _save_needed():
    """Atomic write -- same pattern as _save_watched()."""
    os.makedirs(os.path.dirname(NEEDED_FILE), exist_ok=True)
    tmp_path = NEEDED_FILE + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(_needed, f, indent=2)
    os.replace(tmp_path, NEEDED_FILE)


def _needed_entry_kind(entry):
    """Derived, not stored -- see the module comment above. Returns 'entity' when
    band and mode are both blank (whole-entity target), 'slot' otherwise."""
    if (entry.get("band") or "").strip() or (entry.get("mode") or "").strip():
        return "slot"
    return "entity"


def _needed_days_old(entry):
    """Days since a Needed entry was curated, for the web page's stale-entry flag
    (2026-09-05) -- purely a web curation UI concern, not exposed via
    /api/dxmon/needed, since the device has no use for it. Returns None if
    'added' is missing or unparseable, so the template can skip the flag rather
    than show a wrong number."""
    added = (entry.get("added") or "").strip()
    if not added:
        return None
    try:
        added_dt = datetime.fromisoformat(added)
    except ValueError:
        return None
    return (datetime.now(EASTERN) - added_dt).days


def _add_needed(entity, band, mode, note=""):
    """Entity is always required. Band and mode are each independently optional and
    may now BOTH be blank (2026-09-04 -- represents a whole never-confirmed entity,
    the old Needed semantic). Previously at least one of band/mode was required
    (Wanted-only); that constraint is relaxed now that this list covers both cases."""
    entity = (entity or "").strip()
    band = (band or "").strip()
    mode = (mode or "").strip()
    if not entity:
        return None, "Entity is required."

    entry = {
        "id": uuid.uuid4().hex[:12],
        "entity": entity,
        "band": band,
        "mode": mode,
        "note": (note or "").strip(),
        "added": datetime.now(EASTERN).isoformat(),
    }
    with _needed_lock:
        _needed.append(entry)
        _save_needed()
    return entry, None


def _remove_needed(needed_id):
    with _needed_lock:
        before = len(_needed)
        _needed[:] = [w for w in _needed if w["id"] != needed_id]
        removed = len(_needed) != before
        if removed:
            _save_needed()
    return removed


def _update_needed(needed_id, entity, band, mode, note=""):
    """In-place edit (2026-09-05) -- id and added are preserved unchanged; only
    entity/band/mode/note are replaced. Same validation as _add_needed (entity
    required, band/mode each independently optional)."""
    entity = (entity or "").strip()
    band = (band or "").strip()
    mode = (mode or "").strip()
    if not entity:
        return None, "Entity is required."

    with _needed_lock:
        for w in _needed:
            if w["id"] == needed_id:
                w["entity"] = entity
                w["band"] = band
                w["mode"] = mode
                w["note"] = (note or "").strip()
                _save_needed()
                return w, None
    return None, "Not found."


# --------------------------------------------------------------------------------------
# Trigger Builder -- generates ready-to-paste HamAlert trigger recipes
# --------------------------------------------------------------------------------------

_no_confirms_entities = []  # list of {"entity": str, "prefix": str}, loaded at startup


def _load_no_confirms():
    """Load the never-confirmed-entity seed list from NO_CONFIRMS_FILE. Missing or
    unreadable file degrades to an empty list (the picker just shows no entities,
    ad-hoc entity/callsign entry still works) rather than crashing the service."""
    global _no_confirms_entities
    try:
        with open(NO_CONFIRMS_FILE, "r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            _no_confirms_entities = [
                {"entity": row["Entity"].strip(), "prefix": row.get("Prefix", "").strip()}
                for row in reader
                if row.get("Entity", "").strip()
            ]
        log.info("Loaded %d never-confirmed entities from %s", len(_no_confirms_entities), NO_CONFIRMS_FILE)
    except FileNotFoundError:
        log.warning("No no_confirms file at %s -- entity picker will be empty until one is added", NO_CONFIRMS_FILE)
        _no_confirms_entities = []
    except (csv.Error, OSError, KeyError) as exc:
        log.error("Failed to load %s -- entity picker will be empty: %s", NO_CONFIRMS_FILE, exc)
        _no_confirms_entities = []


DIGITAL_MODES = "FT8, FT4"
VOICE_CW_MODES = "CW, SSB, RTTY"


def _build_trigger_recipes(callsigns, entities):
    """Pure function: given raw callsign/entity target lists, return a list of
    ready-to-paste recipe dicts. No HamAlert API exists for automation (confirmed),
    so this only ever produces text for Dan to manually enter on hamalert.org --
    never claims to create anything automatically.

    Callsign targets always produce exactly one recipe, unsplit -- see
    ENTITY_SPLIT_THRESHOLD's comment for why. Entity targets split into a
    Digital / Voice+CW pair once the list exceeds ENTITY_SPLIT_THRESHOLD, the same
    lever design already established for the (still separately-designed) bulk
    Needed-entity case."""
    recipes = []

    clean_callsigns = sorted({c.strip().upper() for c in callsigns if c.strip()})
    clean_entities = sorted({e.strip() for e in entities if e.strip()})

    if clean_callsigns:
        recipes.append({
            "title": "Watched Callsigns",
            "condition_label": "Callsign is",
            "condition_value": ", ".join(clean_callsigns),
            "mode_filter": None,
            "note": "Exact-callsign match, naturally low volume -- no splitting needed.",
        })

    if clean_entities:
        if len(clean_entities) <= ENTITY_SPLIT_THRESHOLD:
            recipes.append({
                "title": "Needed Entities",
                "condition_label": "DXCC is",
                "condition_value": ", ".join(clean_entities),
                "mode_filter": None,
                "note": (
                    "Select each entity BY NAME in HamAlert's own DXCC picker, not by "
                    "prefix -- some prefixes are shared across entities (e.g. 3Y covers "
                    "both Bouvet Island and Peter I Island)."
                ),
            })
        else:
            shared_note = (
                "Select each entity BY NAME in HamAlert's own DXCC picker, not by "
                "prefix -- some prefixes are shared across entities. Split into two "
                "triggers because more than %d entities risks HamAlert's 10,000-"
                "spots/day auto-disable ceiling (confirmed real, 2026-08-18). If a "
                "bucket still gets auto-disabled, check its trigger status page on "
                "hamalert.org and come back here to narrow the entity list further."
            ) % ENTITY_SPLIT_THRESHOLD
            recipes.append({
                "title": "Needed Entities -- Digital",
                "condition_label": "DXCC is",
                "condition_value": ", ".join(clean_entities),
                "mode_filter": DIGITAL_MODES,
                "note": shared_note + " Digital spots are continuously robot-generated (RBN/skimmer decoders) -- much higher volume than the Voice/CW bucket.",
            })
            recipes.append({
                "title": "Needed Entities -- Voice/CW",
                "condition_label": "DXCC is",
                "condition_value": ", ".join(clean_entities),
                "mode_filter": VOICE_CW_MODES,
                "note": shared_note + " Human-posted spots only, naturally much lower volume than the Digital bucket.",
            })

    return recipes


def _build_needed_trigger_recipe(entity, band, mode):
    """Single recipe for one Needed entry (unified 2026-09-04 -- covers both the old
    Needed and old Wanted cases). Confirmed 2026-08-25 (screenshot) that HamAlert's
    own trigger editor supports combining DXCC, Band, and Mode conditions together in
    one trigger. Uses a `conditions` list rather than the single condition_label/
    condition_value shape _build_trigger_recipes uses -- kept as a separate, additive
    format so the already-tested Watched general-picker recipe path isn't touched.

    Band and mode are each independently optional and may both be blank -- a
    whole-entity target (e.g. a never-confirmed DXCC, any band/mode) generates a
    DXCC-only condition; a slot target (band and/or mode set) adds those as
    additional conditions, same as before. No splitting logic needed either way:
    even the broadest case here (one entity, any band/mode) is one Dan already
    scopes narrowly himself when he pastes it into HamAlert, same discipline as his
    existing Watched triggers.

    Deliberately does NOT use HamAlert's own \"Band slots\" condition (which draws from
    Club Log's automated missing-slot data) -- see NEEDED_FILE's module-level comment
    for why this list stays fully Dan-curated instead."""
    entity = (entity or "").strip()
    band = (band or "").strip()
    mode = (mode or "").strip()
    if not entity:
        return None

    conditions = [{"label": "DXCC is", "value": entity}]
    if band:
        conditions.append({"label": "Band is", "value": band})
    if mode:
        conditions.append({"label": "Mode is", "value": mode})

    title_parts = [entity] + [p for p in (band, mode) if p]

    return {
        "title": f"Needed: {' '.join(title_parts)}",
        "conditions": conditions,
        "note": (
            "Select the entity BY NAME in HamAlert's own DXCC picker, not by prefix. "
            "Volume expected to be very low."
        ),
    }


# --------------------------------------------------------------------------------------
# HamAlert listener status/control -- calls the separate dxmon-hamalert service
# --------------------------------------------------------------------------------------

def _hamalert_get(path):
    """GET a path on the HamAlert listener. Returns (json_or_None, error_str_or_None) --
    never raises, so a down/unreachable listener degrades to a clear message on the page
    rather than crashing the curation UI."""
    try:
        r = requests.get(f"{HAMALERT_LISTENER_URL}{path}", timeout=HAMALERT_REQUEST_TIMEOUT_SECONDS)
        r.raise_for_status()
        return r.json(), None
    except requests.RequestException as exc:
        return None, str(exc)


def _hamalert_post(path):
    try:
        r = requests.post(f"{HAMALERT_LISTENER_URL}{path}", timeout=HAMALERT_REQUEST_TIMEOUT_SECONDS)
        r.raise_for_status()
        return r.json(), None
    except requests.RequestException as exc:
        return None, str(exc)


# --------------------------------------------------------------------------------------
# Beam heading -- great-circle bearing/distance from STATION_GRID to a watched
# callsign's DXCC entity, via pyhamtools' country-file lookup.
# --------------------------------------------------------------------------------------

_lookuplib = None
_callinfo = None
_beam_heading_init_failed = False
_station_latlon = None


def _bearing_deg(lat1, lon1, lat2, lon2):
    """Great-circle initial bearing, in degrees (0-360), from point 1 to point 2.
    Matches pyhamtools.locator.calculate_heading's own internal formula exactly
    (verified against its documented example, JN48QM -> QF67bf = 74.3136 deg) --
    reimplemented directly against lat/lon rather than calling that function, since
    it only accepts Maidenhead locator strings and Callinfo.get_lat_long() already
    gives us lat/lon directly; round-tripping through a locator string would only
    add unnecessary precision loss."""
    r_lat1, r_lat2 = math.radians(lat1), math.radians(lat2)
    d_lon = math.radians(lon2 - lon1)
    b = math.atan2(
        math.sin(d_lon) * math.cos(r_lat2),
        math.cos(r_lat1) * math.sin(r_lat2) - math.sin(r_lat1) * math.cos(r_lat2) * math.cos(d_lon),
    )
    bd = math.degrees(b)
    _, bn = divmod(bd + 360, 360)
    return bn


def _distance_km(lat1, lon1, lat2, lon2):
    """Great-circle distance in km via the haversine formula."""
    R = 6371.0
    r_lat1, r_lat2 = math.radians(lat1), math.radians(lat2)
    d_lat = r_lat2 - r_lat1
    d_lon = math.radians(lon2 - lon1)
    a = math.sin(d_lat / 2) ** 2 + math.cos(r_lat1) * math.cos(r_lat2) * math.sin(d_lon / 2) ** 2
    c = 2 * math.asin(math.sqrt(a))
    return R * c


def _init_beam_heading():
    """Lazily initializes pyhamtools' country-file lookup (downloads/caches the
    database once at first use, not per-request -- matches the project's established
    'no live per-lookup external calls' pattern). A failure here (e.g. no network
    path to country-files.com) disables heading calculation gracefully rather than
    crashing the service -- every /api/dxmon/watched response still works, just
    without a heading field, same degrade-gracefully approach used everywhere else
    in this service."""
    global _lookuplib, _callinfo, _beam_heading_init_failed, _station_latlon
    if _callinfo is not None or _beam_heading_init_failed:
        return
    try:
        from pyhamtools import LookupLib, Callinfo
        from pyhamtools.locator import locator_to_latlong

        _lookuplib = LookupLib(lookuptype="countryfile")
        _callinfo = Callinfo(_lookuplib)
        _station_latlon = locator_to_latlong(STATION_GRID)
        log.info(
            "Beam heading initialized -- station grid %s -> lat/lon %s",
            STATION_GRID, _station_latlon,
        )
    except Exception as exc:  # noqa: BLE001 -- any failure here just disables the feature
        log.error("Beam heading unavailable (station grid %s): %s", STATION_GRID, exc)
        _beam_heading_init_failed = True


def _get_heading_to_callsign(callsign):
    """Returns {'heading_deg': float, 'distance_km': int} or None if unavailable
    (lookup not initialized, unknown callsign, or any other failure). Never raises --
    a missing heading should never break the rest of /api/dxmon/watched's response."""
    _init_beam_heading()
    if _callinfo is None or not callsign:
        return None
    try:
        target = _callinfo.get_lat_long(callsign)
        my_lat, my_lon = _station_latlon
        heading = _bearing_deg(my_lat, my_lon, target["latitude"], target["longitude"])
        distance = _distance_km(my_lat, my_lon, target["latitude"], target["longitude"])
        return {"heading_deg": round(heading, 1), "distance_km": round(distance)}
    except Exception as exc:  # noqa: BLE001 -- unknown callsign, bad data, etc.
        log.debug("No heading available for %r: %s", callsign, exc)
        return None


# --------------------------------------------------------------------------------------
# Flask app
# --------------------------------------------------------------------------------------

app = Flask(__name__)
app.secret_key = os.environ.get("FLASK_SECRET_KEY", "dxmon-dev-secret")


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


@app.route("/api/watched", methods=["GET"])
def api_watched_list():
    with _watched_lock:
        return jsonify({"entries": list(_watched), "count": len(_watched)})


@app.route("/api/watched", methods=["POST"])
def api_watched_add():
    payload = request.get_json(silent=True) or {}
    entry, error = _add_watched(
        callsign=payload.get("callsign"),
        dxcc=payload.get("dxcc"),
        source_adxo_id=payload.get("source_adxo_id"),
        note=payload.get("note"),
    )
    if error:
        return jsonify({"error": error}), 400
    return jsonify(entry), 201


@app.route("/api/watched/<watched_id>", methods=["DELETE"])
def api_watched_remove(watched_id):
    removed = _remove_watched(watched_id)
    if not removed:
        return jsonify({"error": "not found"}), 404
    return jsonify({"removed": watched_id})


# --------------------------------------------------------------------------------------
# DXMon-wide display schema -- the merged, device-facing view over watched.json,
# the ADXO cache, and the HamAlert listener's recent spots. This is intentionally a
# DIFFERENT endpoint from /api/watched: that one is the raw CRUD data model the
# curation UI itself edits, this one is a display-ready join built for firmware/the
# future virtual preview to consume directly, with no client-side merging needed.
# --------------------------------------------------------------------------------------

def _find_last_spot_for_callsign(callsign, recent_spots):
    """Return a trimmed, display-ready spot dict for the most recent HamAlert spot
    matching `callsign`, or None if there's no match. Matching is exact (case-
    insensitive) against either the spot's `callsign` or `fullCallsign` field --
    known limitation: doesn't attempt to reconcile portable-prefix variants (e.g.
    a watched entry of "3B9/SQ9UM" won't match a bare "3B9" spot). Flagged as an
    open item rather than solved here, since it needs real-world examples to get
    right rather than guessing at every possible callsign format.
    `recent_spots` is expected newest-first (matches the listener's own ordering).

    `comment` (e.g. "up \"big pile\"", "FT8 F/H") is only present on real
    `source: "cluster"` spots -- a human typed it. Automated sources (`"rbn"`,
    `"pskreporter"`) never have it. Confirmed live 2026-08-24 -- HamAlert already
    extracts this as its own field, no free-text parsing of `rawText` needed."""
    target = (callsign or "").strip().upper()
    if not target:
        return None
    for entry in recent_spots:
        spot = entry.get("spot", {})
        spot_callsign = (spot.get("callsign") or "").upper()
        spot_full = (spot.get("fullCallsign") or "").upper()
        if target in (spot_callsign, spot_full):
            return {
                "band": spot.get("band"),
                "mode": spot.get("mode"),
                "frequency": spot.get("frequency"),
                "received_at": entry.get("received_at"),
                "source": spot.get("source"),
                "comment": spot.get("comment"),
                "raw_text": spot.get("rawText"),
                "spotter": spot.get("spotter"),
                "spotter_continent": spot.get("spotterContinent"),
                "spotter_entity": spot.get("spotterEntity"),
            }
    return None


def _sort_key_group(item):
    """0 = ADXO-active, 1 = ADXO-upcoming, 2 = spotted-only (no ADXO link), 3 = neither."""
    if item["adxo"] and item["adxo"]["active"]:
        return 0
    if item["adxo"]:
        return 1
    if item["last_spot"]:
        return 2
    return 3


def _build_dxmon_watched():
    with _watched_lock:
        watched_list = list(_watched)
    with _lock:
        adxo_by_id = {e["id"]: e for e in _state["entries"]}

    recent, _recent_err = _hamalert_get("/api/hamalert/recent")
    recent_spots = recent.get("spots", []) if recent else []

    result = []
    for w in watched_list:
        adxo_obj = None
        sid = w.get("source_adxo_id")
        if sid and sid in adxo_by_id:
            e = adxo_by_id[sid]
            adxo_obj = {
                "active": e["active"],
                "begin": e["begin"],
                "end": e["end"],
                "info": e["info"],
            }
        result.append({
            "callsign": w["callsign"],
            "dxcc": w["dxcc"],
            "note": w.get("note", ""),
            "adxo": adxo_obj,
            "last_spot": _find_last_spot_for_callsign(w["callsign"], recent_spots),
            "beam": _get_heading_to_callsign(w["callsign"]),
        })

    # Layered stable sort, least-significant key first -- see _sort_key_group's
    # docstring for the four-group priority. Each pass only reorders items that are
    # tied on every more-significant key applied afterward, since Python's sort is
    # stable; this avoids needing one comparator that mixes ascending and descending
    # directions across different fields.
    result.sort(key=lambda item: item["callsign"])
    result.sort(
        key=lambda item: item["last_spot"]["received_at"] if item["last_spot"] else "",
        reverse=True,
    )
    result.sort(key=lambda item: item["adxo"]["begin"] if item["adxo"] else "9999-99-99")
    result.sort(key=_sort_key_group)

    return result


@app.route("/api/dxmon/watched")
def api_dxmon_watched():
    return jsonify({
        "updated": datetime.now(EASTERN).isoformat(),
        "watched": _build_dxmon_watched(),
    })

# =========================================================================================
# Needed / Wanted -- real-time spot cross-referencing, added 2026-09-01
#
# Real design basis (session discussion 2026-09-01):
#   - Watched vs. (Needed+Wanted) is the real conceptual split -- both Needed and Wanted
#     answer "DX I still need, ranked by how close I am," just from different sources
#     (a whole missing entity vs. a specific missing band/mode slot on a confirmed one).
#   - Matching is by ENTITY NAME, not DXCC number -- confirmed as the existing codebase's
#     own convention throughout (Watched's "dxcc" field holds a name string; the Trigger
#     Builder explicitly tells Dan to "Select each entity BY NAME... not by prefix"; ADXO
#     entries' own "dxcc" field is a name string too). HamAlert's real spot payload already
#     carries a resolved "entity" name string on every spot regardless of source (cluster/
#     pskreporter/rbn) or trigger type (callsign/DXCC/band/mode) -- confirmed live 2026-09-01
#     against real firing triggers (9V1SH/9V1ZV -> "Singapore", 9S1P -> "Democratic Republic
#     of the Congo"). No pyhamtools lookup needed for this -- that stays beam-heading-only.
#   - "triggerComment" on each spot echoes the firing trigger's real Comment text, but is
#     deliberately NOT used for matching -- free-text comments could drift out of sync with
#     the real watched/needed/wanted lists over time. Structured fields (entity/band/mode)
#     are the source of truth; triggerComment is display-only if ever surfaced.
#   - Known real risk, not fully mitigated: entity-name matching requires no_confirms.csv's
#     "Entity" column to string-match HamAlert's own resolved entity names for the same real
#     place. Normalized (lowercased, stripped) comparison is used to reduce but not eliminate
#     this risk. If a Needed entity never seems to match despite a real spot existing, check
#     for a naming mismatch between the CSV and HamAlert's own DXCC picker first.
#   - Club Log Most Wanted List ranking (for Needed entities with no ADXO link and no live
#     spot) is NOT implemented here -- the original ranking design called for it, but no
#     Club Log integration exists anywhere in this codebase yet. Real, separate task,
#     flagged rather than faked. Entities without a live spot or ADXO link fall back to
#     no_confirms.csv's own file order for now.
# =========================================================================================

def _normalize_entity_name(name):
    return (name or "").strip().lower()


def _needed_entity_kind_exists(entity_name):
    """True if a whole-entity (band+mode both blank) Needed entry already exists
    for this entity name (2026-09-05, supports the Trigger Builder's 'also add to
    Needed' checkbox). Deliberately does NOT count an existing SLOT entry on the
    same entity as a duplicate -- an entity can legitimately have both a
    whole-entity target and one or more separate band/mode-slot targets at once
    (same reasoning already established for last-seen records, see the comment
    above that section)."""
    target = _normalize_entity_name(entity_name)
    with _needed_lock:
        for w in _needed:
            if _normalize_entity_name(w.get("entity")) == target and _needed_entry_kind(w) == "entity":
                return True
    return False


def _find_last_spot_for_entity(entity_name, recent_spots, band=None, mode=None):
    """Same shape/purpose as _find_last_spot_for_callsign, but matches by entity name
    instead of callsign -- the right join key for Needed (any band/mode) and Wanted
    (a specific band/mode) since neither is tied to one advance-known callsign.

    band/mode, if given, are additional required-match filters (Wanted's own scoping --
    Needed passes both as None, matching "any band/mode" per its own definition).
    Band comparison is normalized lowercase (spot bands are already lowercase, e.g. "15m",
    but defensive here); mode comparison likewise (spot modes are lowercase, e.g. "ft8")."""
    target = _normalize_entity_name(entity_name)
    if not target:
        return None
    band_filter = (band or "").strip().lower() or None
    mode_filter = (mode or "").strip().lower() or None
    # Real bug found 2026-09-05: a Needed entry's band/mode fields can hold multiple
    # comma-separated values (e.g. "17m, 15m", entered via the curation form's own
    # "17M, 15M" example wording) -- but a single real spot only ever has ONE band
    # and ONE mode. Comparing the whole stored string against the spot's single
    # value with exact equality meant a multi-band/mode entry could never match at
    # all (confirmed live: Singapore/9V1SH real 15m FT8 spots sat in HamAlert's
    # buffer but never showed as a hit, because "15m" != "17m, 15m"). Fixed by
    # splitting each filter into a set of acceptable individual values and matching
    # on membership instead of whole-string equality -- single-value entries (like
    # Sierra Leone's) behave identically to before, since a one-item set is
    # equivalent to the old exact-match check.
    band_filter_set = {b.strip() for b in band_filter.split(",") if b.strip()} if band_filter else None
    mode_filter_set = {m.strip() for m in mode_filter.split(",") if m.strip()} if mode_filter else None

    for entry in recent_spots:
        spot = entry.get("spot", {})
        if _normalize_entity_name(spot.get("entity")) != target:
            continue
        if band_filter_set and (spot.get("band") or "").strip().lower() not in band_filter_set:
            continue
        if mode_filter_set and (spot.get("mode") or "").strip().lower() not in mode_filter_set:
            continue
        return {
            "callsign": spot.get("callsign"),
            "band": spot.get("band"),
            "mode": spot.get("mode"),
            "frequency": spot.get("frequency"),
            "received_at": entry.get("received_at"),
            "source": spot.get("source"),
            "comment": spot.get("comment"),
            "raw_text": spot.get("rawText"),
            "spotter": spot.get("spotter"),
            "spotter_continent": spot.get("spotterContinent"),
            "spotter_entity": spot.get("spotterEntity"),
        }
    return None


# --------------------------------------------------------------------------------------
# Persistent last-seen tracking for Needed/Wanted -- added 2026-09-02.
#
# Real gap found and fixed: hamalert_listener.py only keeps a rolling buffer of the
# last ~100 spots across ALL entities combined (not per-entity). That's enough to know
# "is this entity live right now" (last_spot, above) but NOT enough to answer "when did
# we last see this entity" once a spot ages out of that shared buffer -- a real risk
# given Watched's own busy DXpeditions (RI1FJL alone produced dozens of spots in a
# single day earlier this project) can push a rare Needed/Wanted hit out of that window
# within hours. This persists a per-entity/per-wanted-target "last seen" record
# separately, surviving both buffer churn and container restarts -- the real data
# source for the firmware's Tier 2 ("Last hit: 3 days ago") empty-state design.
# --------------------------------------------------------------------------------------

LAST_SEEN_FILE = os.environ.get("LAST_SEEN_FILE", "/app/data/last_seen.json")

_last_seen_lock = threading.Lock()
_last_seen = {}  # key -> spot-info dict (same shape _find_last_spot_for_entity returns)


def _load_last_seen():
    global _last_seen
    try:
        with open(LAST_SEEN_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            _last_seen = data
            log.info("Loaded %d last-seen records from %s", len(_last_seen), LAST_SEEN_FILE)
        else:
            log.error("Last-seen file did not contain a dict -- starting empty, not overwriting")
    except FileNotFoundError:
        log.info("No existing last-seen file at %s -- starting empty", LAST_SEEN_FILE)
    except (json.JSONDecodeError, OSError) as exc:
        # Same recovery posture as _load_watched -- never let a corrupt file silently
        # wipe real data on disk. Start empty in memory this run only.
        log.error("Failed to load last-seen file (%s) -- starting empty in memory, "
                   "NOT overwriting the file on disk: %s", LAST_SEEN_FILE, exc)
        _last_seen = {}


def _save_last_seen():
    """Atomic write, same pattern as _save_watched/_save_wanted."""
    os.makedirs(os.path.dirname(LAST_SEEN_FILE), exist_ok=True)
    tmp_path = LAST_SEEN_FILE + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(_last_seen, f, indent=2)
    os.replace(tmp_path, LAST_SEEN_FILE)


def _record_last_seen(key, spot_info):
    """Only writes to disk when this is genuinely new information -- a fresh key, or a
    newer received_at than what's already stored (ISO 8601 timestamps sort correctly as
    plain strings, the same comparison technique already used elsewhere in this
    codebase). Avoids a disk write on every ~60s device poll for unchanged data."""
    new_received_at = spot_info.get("received_at") or ""
    with _last_seen_lock:
        existing = _last_seen.get(key)
        if existing and existing.get("received_at", "") >= new_received_at:
            return  # not newer -- nothing to record
        _last_seen[key] = spot_info
        _save_last_seen()


def _get_last_seen(key):
    with _last_seen_lock:
        return _last_seen.get(key)


def _build_dxmon_needed():
    """Unified 2026-09-04 -- one curated list (_needed), no ADXO cross-reference
    (ADXO is Watched-only going forward -- a curated Needed entry has no DXpedition
    date window to schedule against) and no no_confirms.csv cross-reference (that
    file now only seeds the Trigger Builder's picker, see NEEDED_FILE's comment).

    Each entry is keyed by its own stable id (not entity name alone), since two
    different Needed entries can share an entity (e.g. a whole-entity target and a
    separate specific-slot target on the same DXCC) and each needs its own
    independent last-seen record -- same reasoning the old Wanted builder already
    used for its own id-keyed records.

    Sort priority (replaces the old live-spot > ADXO-active > ADXO-upcoming > CSV-
    order chain, which no longer applies without ADXO/CSV involvement):
      0 = has a real live spot right now, tied-broken by recency (most recent first)
      1 = no live spot, but a persisted last-seen record exists, tie-broken by
          recency (most recent first)
      2 = neither -- falls back to order-added, oldest-curated-first."""
    with _needed_lock:
        needed_list = list(_needed)
    recent, _recent_err = _hamalert_get("/api/hamalert/recent")
    recent_spots = recent.get("spots", []) if recent else []

    result = []
    for n in needed_list:
        key = "needed:" + n["id"]
        last_spot = _find_last_spot_for_entity(n["entity"], recent_spots, n.get("band"), n.get("mode"))
        if last_spot:
            _record_last_seen(key, last_spot)
        result.append({
            "id": n["id"],
            "kind": _needed_entry_kind(n),  # "entity" or "slot" -- derived, see comment above _needed_entry_kind
            "entity": n["entity"],
            "band": n.get("band", ""),
            "mode": n.get("mode", ""),
            "note": n.get("note", ""),
            "added": n.get("added", ""),
            "last_spot": last_spot,
            "last_seen": _get_last_seen(key),
        })

    # Stable multi-key sort, least significant first (matches this file's existing
    # convention elsewhere, e.g. the old _build_dxmon_needed above did the same).
    result.sort(key=lambda item: item["added"])  # tier 2 fallback: order-added, oldest first
    result.sort(
        key=lambda item: item["last_seen"]["received_at"] if item["last_seen"] else "",
        reverse=True,
    )  # tier 1: most-recently-seen first
    result.sort(
        key=lambda item: item["last_spot"]["received_at"] if item["last_spot"] else "",
        reverse=True,
    )  # tier 0 tie-break: most-recent live spot first
    result.sort(
        key=lambda item: 0 if item["last_spot"] else (1 if item["last_seen"] else 2)
    )  # group: live > last-seen > neither
    return result


@app.route("/api/dxmon/needed")
def api_dxmon_needed():
    return jsonify({
        "updated": datetime.now(EASTERN).isoformat(),
        "needed": _build_dxmon_needed(),
    })


@app.route("/api/beam/<callsign>")
def api_beam_test(callsign):
    """Standalone test endpoint for the beam heading lookup, independent of the
    watchlist -- useful for confirming the live country-file lookup actually works
    against real callsigns once deployed (this can't be tested from Claude's own
    sandbox, which has no network path to country-files.com)."""
    result = _get_heading_to_callsign(callsign)
    if result is None:
        return jsonify({
            "callsign": callsign,
            "beam": None,
            "note": "No result -- either the lookup isn't initialized (check server logs "
                    "for a country-files.com fetch error) or the callsign wasn't found.",
        }), 404
    return jsonify({"callsign": callsign, "station_grid": STATION_GRID, **result})


@app.route("/api/preview/status")
def api_preview_status():
    """Combined status for the virtual device preview's Config tab. Deliberately
    server-side proxied rather than having the preview's client-side JS fetch the
    HamAlert listener directly -- that's a different port/origin (8084 vs this
    service's 8083), which a browser blocks as cross-origin unless the listener
    explicitly sets CORS headers (it doesn't, and shouldn't need to just for this).
    Same server-side-proxy pattern the /hamalert page already uses, just also
    exposed as JSON here for the preview page's own fetch() calls."""
    hamalert_status, hamalert_err = _hamalert_get("/api/hamalert/status")
    with _lock:
        adxo_updated = _state["updated"]
        adxo_entry_count = len(_state["entries"])
    with _watched_lock:
        watched_count = len(_watched)
    return jsonify({
        "adxo": {"updated": adxo_updated, "entry_count": adxo_entry_count},
        "hamalert": hamalert_status if hamalert_status else {"unreachable": True, "error": hamalert_err},
        "watched_count": watched_count,
    })


# --------------------------------------------------------------------------------------
# Curation UI pages (server-rendered, plain HTML forms -- no JS needed for this pass)
# --------------------------------------------------------------------------------------

@app.route("/")
def page_index():
    with _lock:
        entries = list(_state["entries"])
    # Active first, then soonest-upcoming -- matches the Watched screen's own sort rule
    # already decided for firmware, applied here too for consistency.
    entries.sort(key=lambda e: (not e["active"], e["begin"]))
    watched_map = _watched_by_adxo_id()
    return render_template("index.html", entries=entries, watched_map=watched_map)


@app.route("/watched")
def page_watched():
    with _watched_lock:
        entries = sorted(_watched, key=lambda w: w["added"], reverse=True)
    # Copy each entry (never mutate _watched itself) and attach beam heading for
    # display -- kept separate from the persisted watched.json record.
    enriched = []
    for w in entries:
        e = dict(w)
        e["beam"] = _get_heading_to_callsign(w["callsign"])
        enriched.append(e)
    return render_template("watched.html", entries=enriched)


@app.route("/watch", methods=["POST"])
def page_watch_add():
    _add_watched(
        callsign=request.form.get("callsign"),
        dxcc=request.form.get("dxcc"),
        source_adxo_id=request.form.get("source_adxo_id"),
        note=request.form.get("note"),
    )
    # Errors are simply ignored here (silently no-op if callsign/dxcc empty) --
    # the form itself makes both fields required client-side; this endpoint stays
    # forgiving rather than surfacing a raw 400 in the browser for v1.
    return redirect(url_for("page_index"))


@app.route("/watch/remove/<watched_id>", methods=["POST"])
def page_watch_remove(watched_id):
    _remove_watched(watched_id)
    return redirect(url_for("page_watched"))


@app.route("/needed")
def page_needed():
    with _needed_lock:
        entries = sorted(_needed, key=lambda w: w["added"], reverse=True)
    # 2026-09-05: attach days_old for the stale-entry flag -- computed here rather
    # than in the template, since Jinja has no built-in date arithmetic. Web-only,
    # not part of the entry shape returned by /api/dxmon/needed.
    entries = [dict(w, days_old=_needed_days_old(w)) for w in entries]
    return render_template("needed.html", entries=entries)


@app.route("/needed/add", methods=["POST"])
def page_needed_add():
    # 2026-09-05: band/mode are now checkboxes (multiple can be checked), not free
    # text -- getlist() collects every checked value under that name. Joined into
    # the same ", "-separated string format the storage/matching logic already
    # expects (see _find_last_spot_for_entity's band_filter_set/mode_filter_set),
    # so _add_needed() itself needs no change. A single checked value still stores
    # as a plain single-value string, identical to the old free-text behavior.
    _add_needed(
        entity=request.form.get("entity"),
        band=", ".join(request.form.getlist("band")),
        mode=", ".join(request.form.getlist("mode")),
        note=request.form.get("note"),
    )
    return redirect(url_for("page_needed"))


@app.route("/needed/remove/<needed_id>", methods=["POST"])
def page_needed_remove(needed_id):
    _remove_needed(needed_id)
    return redirect(url_for("page_needed"))


@app.route("/needed/edit/<needed_id>", methods=["GET"])
def page_needed_edit_form(needed_id):
    # 2026-09-05: reuses needed.html's own "Add a needed target" card in edit mode
    # (pre-filled, posts to page_needed_edit_save instead of page_needed_add)
    # rather than a separate template -- same form, same checkbox groups, just
    # pre-checked to the entry's current band/mode.
    with _needed_lock:
        entries = sorted(_needed, key=lambda w: w["added"], reverse=True)
        edit_entry = next((w for w in _needed if w["id"] == needed_id), None)
    if not edit_entry:
        return redirect(url_for("page_needed"))
    entries = [dict(w, days_old=_needed_days_old(w)) for w in entries]
    return render_template("needed.html", entries=entries, edit_entry=edit_entry)


@app.route("/needed/edit/<needed_id>", methods=["POST"])
def page_needed_edit_save(needed_id):
    _update_needed(
        needed_id,
        entity=request.form.get("entity"),
        band=", ".join(request.form.getlist("band")),
        mode=", ".join(request.form.getlist("mode")),
        note=request.form.get("note"),
    )
    return redirect(url_for("page_needed"))


@app.route("/api/needed", methods=["GET"])
def api_needed_list():
    with _needed_lock:
        return jsonify({"entries": list(_needed), "count": len(_needed)})


@app.route("/api/needed", methods=["POST"])
def api_needed_add():
    payload = request.get_json(silent=True) or {}
    entry, error = _add_needed(
        entity=payload.get("entity"),
        band=payload.get("band"),
        mode=payload.get("mode"),
        note=payload.get("note"),
    )
    if error:
        return jsonify({"error": error}), 400
    return jsonify(entry), 201


@app.route("/api/needed/<needed_id>", methods=["DELETE"])
def api_needed_remove(needed_id):
    removed = _remove_needed(needed_id)
    if not removed:
        return jsonify({"error": "not found"}), 404
    return jsonify({"removed": needed_id})


@app.route("/triggers/needed/<needed_id>")
def page_triggers_for_needed(needed_id):
    """Create-Trigger link from a Needed entry -- goes straight to the generated
    recipe, since all three fields (entity/band/mode) are already known. No form
    step needed, unlike the general /triggers builder."""
    with _needed_lock:
        entry = next((w for w in _needed if w["id"] == needed_id), None)
    if entry is None:
        return "Needed entry not found", 404
    recipe = _build_needed_trigger_recipe(entry["entity"], entry["band"], entry["mode"])
    recipes = [recipe] if recipe else []
    return render_template(
        "triggers.html",
        entities=_no_confirms_entities,
        prefill_callsign="",
        recipes=recipes,
    )


@app.route("/hamalert")
def page_hamalert():
    status, status_err = _hamalert_get("/debug")
    recent_resp, recent_err = _hamalert_get("/api/hamalert/recent")
    recent = recent_resp.get("spots", []) if recent_resp else []

    # Distinct callsigns currently appearing in the spot feed, most-recently-seen
    # first, each with a beam heading -- a quick "where do I point the beam" view
    # independent of the Watched list, per Dan's request 2026-08-25.
    seen = set()
    beam_headings = []
    for entry in recent:
        callsign = entry.get("spot", {}).get("callsign")
        if callsign and callsign not in seen:
            seen.add(callsign)
            beam_headings.append({"callsign": callsign, "beam": _get_heading_to_callsign(callsign)})

    return render_template(
        "hamalert.html",
        status=status,
        status_err=status_err,
        recent=recent,
        recent_err=recent_err,
        beam_headings=beam_headings,
    )


@app.route("/hamalert/enable", methods=["POST"])
def page_hamalert_enable():
    _hamalert_post("/api/hamalert/enable")
    return redirect(url_for("page_hamalert"))


@app.route("/hamalert/disable", methods=["POST"])
def page_hamalert_disable():
    _hamalert_post("/api/hamalert/disable")
    return redirect(url_for("page_hamalert"))


@app.route("/preview")
def page_preview():
    """Virtual DXMon device preview -- an 800x480 in-browser rendering of the
    Overview/Watched/Needed/Config screens, fed by this server's own live JSON
    (client-side fetch(), no server-side templating of the data itself). Build
    order step 6 -- exists to test server-side changes and screen layout before
    any firmware exists. Needed tab is an honest placeholder (that feature isn't
    built yet), matching the series' established practice of never faking data
    for a screen that doesn't have a real backend behind it yet."""
    return render_template("preview.html")


@app.route("/triggers")
def page_triggers():
    prefill_callsign = request.args.get("callsign", "")
    return render_template(
        "triggers.html",
        entities=_no_confirms_entities,
        prefill_callsign=prefill_callsign,
        recipes=None,
        added_to_needed=None,
    )


@app.route("/triggers/generate", methods=["POST"])
def page_triggers_generate():
    selected_entities = request.form.getlist("entity")
    extra_entities = [e.strip() for e in request.form.get("extra_entities", "").split(",")]
    extra_callsigns = [c.strip() for c in request.form.get("callsigns", "").split(",")]

    recipes = _build_trigger_recipes(
        callsigns=extra_callsigns,
        entities=selected_entities + extra_entities,
    )

    # 2026-09-05: "also add to Needed" convenience checkbox. Scoped deliberately
    # to entities only (checked never-confirmed entities + typed extra entities)
    # -- callsigns aren't entity names and would need a real country-file lookup
    # to map one to the other, which is out of scope for this convenience feature.
    # Every entity added this way is a whole-entity target (band/mode both blank),
    # since this general-purpose builder has no band/mode fields at all -- that's
    # the correct mapping onto Needed's own ENTITY kind. Dedupes against an
    # existing entity-kind entry for the same entity (see
    # _needed_entity_kind_exists); does NOT touch or duplicate any existing
    # SLOT entry on that same entity, since those are a separate, valid kind.
    added_to_needed = []
    if request.form.get("also_add_to_needed"):
        seen = set()
        for entity in selected_entities + extra_entities:
            entity = (entity or "").strip()
            if not entity:
                continue
            key = _normalize_entity_name(entity)
            if key in seen or _needed_entity_kind_exists(entity):
                continue
            seen.add(key)
            entry, error = _add_needed(entity, "", "")
            if entry:
                added_to_needed.append(entity)

    return render_template(
        "triggers.html",
        entities=_no_confirms_entities,
        prefill_callsign="",
        recipes=recipes,
        added_to_needed=added_to_needed,
    )


if __name__ == "__main__":
    _load_watched()
    _load_no_confirms()
    _load_needed()
    _load_last_seen()
    t = threading.Thread(target=_scheduler_loop, daemon=True)
    t.start()
    app.run(host="0.0.0.0", port=LISTEN_PORT, threaded=True)
