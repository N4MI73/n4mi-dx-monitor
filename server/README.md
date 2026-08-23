# DXMon -- ADXO Ingestion Service

Polls NG3K's [Announced DX Operations (ADXO)](https://www.ng3k.com/Misc/adxoplain.html)
Text Version once daily and serves the parsed result as flat JSON, for DXMon's Watched
and Needed-entity screens to consume.

## Data source agreement -- read before changing polling behavior

Permission to use this feed was requested from and granted by Bill Feidt/NG3K
(email exchange, 2026-08-10 to 2026-08-12). Two commitments were made and are binding
on this service's design:

1. **Poll no more than once per day.** Bill pays for bandwidth on his hosting; once/day
   was confirmed negligible, more than that was not discussed/approved.
2. **Use conditional GET (`If-Modified-Since`) on every request.** A `304 Not Modified`
   response must be treated as "no change, keep the existing cache" -- not an error.

`MIN_REFRESH_INTERVAL_SECONDS` (default 3600s / 1 hour) is a server-side safety floor
that prevents a manual `/api/adxo/refresh` call -- used for testing or a future
Config-screen "Force Refresh" button -- from ever exceeding the once-daily agreement,
regardless of how many times it's triggered.

**ADXO is used strictly as a discovery source** (browse announced operations, decide
what to add to the watchlist). Real-time alerting for watched callsigns runs entirely
through HamAlert, not this service -- DXMon's core alerting does not depend on ADXO's
continued availability. Bill was explicit that he may not maintain ADXO indefinitely;
this design means that's a degradation (losing a convenient discovery source), not a
breakage, if it ever happens.

Full email chain preserved in DXMon's Joplin note ("ADXO Ingestion Service" section).

Give credit to Bill Feidt/NG3K as the data source anywhere this project is described
publicly (blog post, repo README, etc.) -- per Dan's own commitment in the request email.

## Running locally

```bash
pip install -r requirements.txt
python adxo_service.py
```

Listens on port 8083 (override with the `PORT` env var).

## Web curation UI

Server-rendered pages (plain HTML forms, no JS required) at the container's root:

| Page | Purpose |
|---|---|
| `/` | Browse current/upcoming ADXO entries. Active-now entries sort first. Each entry has an inline "Watch" form pre-filled with ADXO's listed callsign -- edit it before submitting if the actual on-air callsign differs (e.g. "3B9" listed, operating "as 3B9/SQ9UM"). Already-watched callsigns tied to an entry show as tags so you don't accidentally double-watch. |
| `/watched` | Current watchlist, with a Remove action per entry. |

Watched entries persist to `WATCHED_FILE` (default `/app/data/watched.json`), which **must**
be on a Docker volume (see `docker-compose.yml`) -- otherwise a container rebuild wipes the
watchlist.

## Endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/healthz` | GET | Liveness check, always returns `{"status": "ok"}` |
| `/api/adxo` | GET | Current parsed entries (expired entries already filtered out) |
| `/api/adxo/refresh` | POST | Manual refresh trigger, rate-limited by `MIN_REFRESH_INTERVAL_SECONDS` |
| `/api/watched` | GET | Current watchlist as JSON |
| `/api/watched` | POST | Add a watched entry -- JSON body `{"callsign", "dxcc", "source_adxo_id"?, "note"?}` |
| `/api/watched/<id>` | DELETE | Remove a watched entry |
| `/debug` | GET | Internal state for self-diagnosis (last fetch time/status, poll schedule, etc.) |

## JSON shape

```json
{
  "updated": "2026-08-20T00:32:00-04:00",
  "entry_count": 47,
  "entries": [
    {
      "id": "5dad60c415fd",
      "dxcc": "St Lucia",
      "callsign": "J68TT",
      "begin": "2026-08-15",
      "end": "2026-08-21",
      "active": true,
      "qsl": "LoTW",
      "source": "TDDX (Aug 6, 2026)",
      "info": "By N4XTT; 40-10m, perhaps 80m; QSL via Club Log OQRS or N4XTT direct"
    }
  ]
}
```

- `active` is computed locally from `begin`/`end` against today's date -- not scraped
  from NG3K's own bold-text markup -- so it stays correct even if their page styling
  changes, and matches the same active-definition used elsewhere in DXMon.
- `id` is a stable hash of `callsign + begin + end + dxcc`. It will change if NG3K
  edits an existing entry's dates, which is treated as acceptable (a materially changed
  entry is arguably a new one anyway).
- Entries with `end` in the past are filtered out before serving; only current and
  upcoming operations are included.
- `info` is served as ADXO's raw free-text field, unparsed -- band/mode formatting in
  the source is inconsistent enough (`"40-10m"` vs `"160-6m, incl 60m"` vs `"20 15 10m"`)
  that structured extraction isn't attempted for v1. Revisit if a real downstream need
  for structured bands/modes from ADXO specifically (as opposed to HamAlert) comes up.

## Deployment (Portainer)

Standard series pattern -- Stacks -> Repository build method, compose path
`server/docker-compose.yml` if this ends up under a `server/` subfolder alongside
`firmware/` (APRSMon's pattern). Set `MIN_REFRESH_INTERVAL_SECONDS` as an env var
override only if you have a real reason to change the testing floor -- do not use it
to increase real-world polling frequency past once/day.

## HamAlert listener (separate service)

Real-time spot matching runs as its own service, `hamalert_listener.py`, deployed as
a **separate Portainer stack** (compose path `server/docker-compose-hamalert.yml`,
its own container `dxmon-hamalert`, port 8084) -- not part of this Flask process.
Unlike the ADXO service, this is a persistent Telnet connection with a genuinely
different failure mode (matches the reasoning behind APRSMon's own Weather/Mobile
split), so it's kept separate rather than folded in.

**Protocol** (confirmed live 2026-08-22/23, since HamAlert's Telnet interface has no
formal published spec -- only two documented commands, `sh/dx N` and `set/json`):
connect to `hamalert.org:7300` -> login prompt -> username -> password prompt ->
password -> `set/json` to enable JSON spot output. An undocumented `echo <token>`
command (confirmed by the HamAlert developer on their support forum) is used as a
keepalive, since no other heartbeat mechanism exists.

**Credentials** (`HAMALERT_USER`, `HAMALERT_PASS`) are Portainer stack environment
variables, entered directly in Portainer's UI -- never committed. The compose file
uses bare-name pass-through syntax (`- HAMALERT_USER` with no `=value`) rather than
`${VAR}` interpolation, because Portainer's Repository build method requires
interpolated values to come from an actual `stack.env` file in the repo, which would
mean committing credentials -- not acceptable. Bare-name pass-through sidesteps that
requirement entirely.

**Enable/disable:** the listener can be toggled via `POST /api/hamalert/enable` and
`POST /api/hamalert/disable` (or the toggle button on the curation UI's HamAlert
page, `/hamalert`). The enabled/disabled state persists to a Docker volume
(`dxmon_hamalert_data`) and survives a container restart -- so disabling before an
extended absence from the shack stays disabled even through a NAS reboot, rather
than silently re-enabling.

**Endpoints:**

| Endpoint | Method | Purpose |
|---|---|---|
| `/healthz` | GET | Liveness check |
| `/api/hamalert/recent` | GET | Last 100 matched spots received |
| `/api/hamalert/status` | GET | `{enabled, connected, logged_in}` |
| `/api/hamalert/enable` | POST | Enable the listener |
| `/api/hamalert/disable` | POST | Disable the listener (closes the connection if active) |
| `/debug` | GET | Full internal state -- connection status, last spot/heartbeat times, reconnect count, last error |

No matching/filtering happens in this service -- HamAlert's own trigger(s), configured
directly on hamalert.org (no API exists for managing them from here), decide what
gets sent. This service just receives, stores, and exposes whatever arrives.
