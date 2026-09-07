# DXMon Server

Backend for DXMon: ADXO ingestion, the web curation UI, a real-time HamAlert
Telnet listener, the Trigger Builder, beam heading calculations, and the
`/api/dxmon/*` endpoints the firmware consumes.

> **ADXO permission:** DXMon can retrieve announced-operation information
> from NG3K's ADXO text page. Permission granted to N4MI applies to the
> project author's personal installation and should not be assumed to
> cover other installations. Before enabling ADXO retrieval, contact the
> ADXO owner and request permission for your instance. DXMon limits
> retrieval to once daily and caches the last successful result to
> minimize load.

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

| File | What it holds |
|---|---|
| `watched.json` | Your curated Watched list |
| `needed.json` | Your curated Needed list |
| `last_seen.json` | Persisted last-hit record per Watched/Needed entry, surviving HamAlert's own rolling spot buffer aging a hit out |
| `spot_history.json` | Persisted last-10-spots history per callsign/entity, feeding the drill-down screens |
| `no_confirms.csv` | Your own never-confirmed-entity list (e.g. exported from LoTW), seeding the Trigger Builder's entity picker |

**Mount the same volume path in both stacks** if both services need to read
any of these -- verify which files each service actually touches against
your own copy of the code before assuming a shared mount is required for
both.

## Environment variables

| Variable | Used by | Purpose |
|---|---|---|
| `STATION_GRID` | `dxmon-adxo` | Your own grid square (e.g. `EM83`), the origin point for beam heading/distance calculations |
| HamAlert Telnet credentials | `dxmon-hamalert` | Login for HamAlert's Telnet interface |

> **Verify the exact variable names against your own `docker-compose.yml`
> before deploying** -- confirm these against what's actually committed in
> this repo rather than assuming the names above are exact.

Never commit real credentials to the repo. Set them as environment variables
directly in each Portainer stack's own configuration, the same way
`TEMPEST_TOKEN` is handled for the sibling PropMon project.

## Deploying via Portainer

For each stack (`dxmon-adxo`, `dxmon-hamalert`):

1. **Stacks** -> **Add stack** -> **Repository** as the build method.
2. Point it at this repo, with the compose file under `server/`.
3. Set the environment variables for that specific stack (see above).
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

Both should return a simple healthy response regardless of upstream data
state (ADXO/HamAlert connectivity issues degrade gracefully rather than
crashing the service).

To confirm real data is flowing:

```
curl http://<your-server-ip>:8083/api/dxmon/watched
curl http://<your-server-ip>:8083/api/dxmon/needed
```

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

## Monitoring

If you run Uptime Kuma or similar, point an HTTP(s) monitor at
`http://<your-server-ip>:8084/healthz` for the HamAlert listener specifically
-- it's the one with a persistent connection that can silently drop without
an obvious symptom otherwise.
