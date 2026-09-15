# DXMon Server

Backend for DXMon: ADXO ingestion, the web curation UI, a real-time HamAlert
Telnet listener, the Trigger Builder, beam heading calculations, and the
`/api/dxmon/*` endpoints the firmware consumes.

> ### ⚠️ ADXO permission is per-installation, not per-repository
>
> DXMon can retrieve announced-operation information from NG3K's ADXO text
> page. **This permission was granted to N4MI specifically, for the
> author's own personal installation -- it does not transfer to your
> installation just because you're running the same code.** If you're
> deploying your own instance, **contact Bill Feidt/NG3K directly and
> obtain your own permission before using ADXO retrieval.** Don't rely on
> this repository's own history of use as implicit permission for your own
> deployment.
>
> Once permission is granted, DXMon limits retrieval to once daily and
> caches the last successful result, to keep load on NG3K's server to a
> minimum. DXMon does not redistribute, republish, or otherwise expose
> retrieved ADXO data to anyone beyond the operator of that specific
> installation.
>
> **Technical note:** `dxmon-adxo` currently begins polling ADXO
> immediately on container start -- there is no separate code-level
> opt-in toggle to enable it after the fact. **Secure your own permission
> before you deploy this stack**, not after.

> This README assumes you're comfortable running a Portainer stack. It
> doesn't walk through Docker or Portainer basics from zero.

## Architecture

Two separate services, deployed as two separate Portainer stacks, on purpose
-- they have genuinely different failure modes (a persistent Telnet socket
vs. a periodic HTTP poller), so keeping them independent means one
restarting doesn't take down the other.

| Service | File | Port | Role |
|---|---|---|---|
| `dxmon-adxo` | `adxo_service.py` | 8083 | Flask app: ADXO ingestion, web curation UI, Trigger Builder, beam heading, `/api/dxmon/*` endpoints the firmware polls |
| `dxmon-hamalert` | `hamalert_listener.py` | 8084 | Persistent Telnet connection to HamAlert, buffers recent real-time spots, exposes them over HTTP for `dxmon-adxo` to consume |

`dxmon-adxo` calls `dxmon-hamalert`'s own HTTP endpoint internally to pull
recent spots -- they talk to each other over the network, not through a
shared file, aside from the persistent JSON data files below.

## Persistent data

All of this lives under `/app/data/` on a Docker volume, so it survives
container restarts and redeploys:

| File | Written by | What it holds |
|---|---|---|
| `watched.json` | `dxmon-adxo` | Your curated Watched list |
| `needed.json` | `dxmon-adxo` | Your curated Needed list (including any pinned entries and their priority order) |
| `last_seen.json` | `dxmon-adxo` | Persisted last-hit record per Watched/Needed entry, surviving HamAlert's own rolling spot buffer aging a hit out |
| `spot_history.json` | `dxmon-adxo` | Persisted last-10-spots history per callsign/entity, feeding the drill-down screens |
| `no_confirms.csv` | You, manually | Your own never-confirmed-entity list (e.g. exported from LoTW), seeding the Trigger Builder's entity picker. `dxmon-adxo` only ever reads this -- it's never written by the app, so your own periodic re-export is never at risk of being overwritten |
| `state.json` | `dxmon-hamalert` | Whether the HamAlert listener is currently enabled or disabled -- see "Pausing HamAlert" below. Persisted specifically so a deliberate pause survives a container/NAS restart |

**Confirmed from the real `docker-compose.yml`:** `dxmon-adxo` mounts a
single named volume (`dxmon_data`) at `/app/data`. `dxmon-hamalert`'s own
compose file wasn't available to verify directly, but its code defaults
`state.json` to that same `/app/data/state.json` path -- if your
`dxmon-hamalert` stack mounts a *different* volume at `/app/data`, your
enable/disable state won't be where this doc assumes. Worth confirming
against your own `dxmon-hamalert` compose file if you're relying on that
feature.

## Environment variables

Confirmed directly against the real source (`adxo_service.py` and
`hamalert_listener.py`) rather than described generically -- exact names,
which service, and real defaults.

### `dxmon-adxo`

| Variable | Default | Required? | Purpose |
|---|---|---|---|
| `PORT` | `8083` | No | Listen port |
| `STATION_GRID` | `EM83` | **Yes, override this** | Your own grid square -- the origin point for every beam heading/distance calculation. The default is N4MI's own grid; leaving it unset means headings are computed from N4MI's station, not yours |
| `PROPMON_URL` | `http://192.168.6.29:8076/api/instrument/propagation` | **Yes, if you run PropMon** | Your own PropMon instance's API address, for the band-condition indicator. The default is N4MI's own LAN address and won't resolve on your network |
| `PROPMON_CACHE_SECONDS` | `300` | No | How long a PropMon band-condition response is cached before re-fetching |
| `HAMALERT_LISTENER_URL` | `http://192.168.6.29:8084` | **Yes** | Where `dxmon-adxo` reaches `dxmon-hamalert` to pull recent spots. The default is N4MI's own LAN address; point this at wherever you actually deployed your own `dxmon-hamalert` stack |
| `ADXO_URL` | NG3K's real ADXO page | No | Override only if NG3K's own URL structure ever changes |
| `POLL_HOUR_ET` / `POLL_MINUTE_ET` | `0` / `30` | No | Time of day (Eastern) for the once-daily ADXO poll |
| `MIN_REFRESH_INTERVAL_SECONDS` | `3600` | No | Minimum spacing between ADXO fetches, as a safety floor independent of the scheduled time above |
| `WATCHED_FILE` | `/app/data/watched.json` | No | Override only if you're customizing the data volume layout |
| `NEEDED_FILE` | `/app/data/needed.json` | No | Same |
| `NO_CONFIRMS_FILE` | `no_confirms.csv` | No | Same -- note this one is relative, not under `/app/data/` by default |
| `LAST_SEEN_FILE` | `/app/data/last_seen.json` | No | Same |
| `SPOT_HISTORY_FILE` | `/app/data/spot_history.json` | No | Same |
| `FLASK_SECRET_KEY` | a fixed dev default | No | Fine to leave default -- this is a LAN-only app with no authentication, so there's nothing this key is actually protecting |

### `dxmon-hamalert`

| Variable | Default | Required? | Purpose |
|---|---|---|---|
| `PORT` | `8084` | No | Listen port |
| `HAMALERT_USER` | none | **Yes** | Your HamAlert callsign/login. The service refuses to start without this set |
| `HAMALERT_PASS` | none | **Yes** | Your HamAlert Telnet password. The service refuses to start without this set |
| `HEARTBEAT_INTERVAL_SECONDS` | `60` | No | How often an idle connection sends a keepalive |
| `HEARTBEAT_TIMEOUT_SECONDS` | `30` | No | How long to wait for a keepalive reply before treating the connection as dead and reconnecting |
| `HAMALERT_STATE_FILE` | `/app/data/state.json` | No | Where the enable/disable flag persists -- see "Pausing HamAlert" below |

Never commit real credentials to the repo. Set `HAMALERT_USER`/
`HAMALERT_PASS` as environment variables directly in the `dxmon-hamalert`
stack's own configuration, the same way `TEMPEST_TOKEN` is handled for the
sibling PropMon project.

## Pausing HamAlert (e.g. before a trip)

`dxmon-hamalert` can be told to disconnect and stop listening without
stopping the container itself -- useful if you're going to be away and
don't want spots accumulating (or a dead Telnet session silently sitting
there) while you're gone:

```
curl -X POST http://<your-server-ip>:8084/api/hamalert/disable
curl -X POST http://<your-server-ip>:8084/api/hamalert/enable
```

This state is **persisted to `state.json`** and survives a container
restart or a full NAS reboot -- if you disable it before leaving, it stays
disabled even if something restarts the stack while you're away, rather
than silently reconnecting on its own.

## Deploying via Portainer

For each stack (`dxmon-adxo`, `dxmon-hamalert`):

1. **Stacks** -> **Add stack** -> **Repository** as the build method.
2. Point it at this repo, with the compose file under `server/`.
3. Set the environment variables for that specific stack (see above --
   at minimum, `STATION_GRID`, `PROPMON_URL`, and `HAMALERT_LISTENER_URL`
   for `dxmon-adxo`, and `HAMALERT_USER`/`HAMALERT_PASS` for
   `dxmon-hamalert`).
4. Deploy.
5. Confirm the container starts cleanly by checking its **Logs** tab (not
   Portainer's own "Activity Logs," which is a Business Edition feature
   covering Portainer's own admin actions, not your application's console
   output -- container logs live under **Containers** -> the container
   itself -> **Logs**).

To update after a code change: commit and push, then use Portainer's
pull-and-redeploy action on the stack.

## Confirming it's running

```
curl http://<your-server-ip>:8083/healthz    # dxmon-adxo
curl http://<your-server-ip>:8084/healthz    # dxmon-hamalert
```

**Both of these are pure liveness checks** -- confirmed directly against
the real code, each one always returns `{"status": "ok"}` regardless of
ADXO/HamAlert connectivity state. They'll tell you the container process
is running, but not whether HamAlert is actually connected -- see
"Monitoring" below for the endpoint that does.

To confirm real data is flowing:

```
curl http://<your-server-ip>:8083/api/dxmon/watched
curl http://<your-server-ip>:8083/api/dxmon/needed
curl http://<your-server-ip>:8084/api/hamalert/status
```

The last one returns the real connection state:
`{"enabled": true, "connected": true, "logged_in": true}`.

## Known external dependency quirk: beam heading

Beam heading uses `pyhamtools`, which downloads country reference data from
country-files.com once per process startup. If you ever see every heading
come back blank at once (not just one callsign), check the container logs
for a line like:

```
Beam heading unavailable (station grid <your grid>): <exception>
```

This has previously been caused by a stale bundled mapping file inside
`pyhamtools` itself not yet knowing about a real-world country renaming
(confirmed and worked around once already -- see the code's own comments
near `_init_beam_heading()` for the specific fix and reasoning if it recurs
for a different country in the future).

## Known external dependency: band condition (PropMon)

The band-condition indicator shown on the device consumes PropMon's own
JSON API as a read-only external contract -- no shared code, no reaching
into PropMon's own repo, same pattern this series already uses elsewhere
(HamOps Console and the Ham Shack Automation propagation dashboard both
consume PropMon the same way). Cached for 5 minutes; degrades to no
indicator (rather than a wrong one) if PropMon is unreachable, and picks up
real condition changes within one cache cycle.

Two bands (160m and 30m) are interpolated rather than natively rated on
PropMon's own side -- lower confidence on those two specifically if a spot
ever lands there. Not something to fix from DXMon's side; it's PropMon's
own code, per this project's own documented boundary with sibling
instruments in this series.

## Monitoring

**`/healthz` on either service will not catch a dropped HamAlert
connection** -- confirmed above, it's a pure liveness check regardless of
connectivity state. For real connection-aware monitoring, point at:

```
http://<your-server-ip>:8084/api/hamalert/status
```

If your monitoring tool supports checking response content (e.g. Uptime
Kuma's "Keyword" monitor type, not its plain HTTP check), watch for
`"connected":true` -- a plain up/down HTTP check on this endpoint will
still report "up" even while `connected` is `false`, since the endpoint
itself always responds. This is the one with a persistent connection that
can silently drop without an obvious symptom otherwise, which is exactly
why `/healthz` alone isn't enough for it.
