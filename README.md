# DXMon -- DX Monitor

**"Is a DX station I care about active right now, and where should I listen?"**

DXMon is the third instrument in the **N4MI Desktop Instrument Series** -- a family of
small, glanceable desk instruments for the ham shack. Each one answers a single
operational question at a glance:

- **[PropMon](https://github.com/N4MI73/n4mi-propagation-monitor)** -- "Can I make the contact?" (HF band conditions, solar indices)
- **[APRSMon](https://github.com/N4MI73/n4mi-aprs-monitor)** -- "What's happening around me?" (local weather + APRS activity)
- **DXMon** (this repo) -- "Is a DX station I care about active right now, and where should I listen?"

> **Status: actively developed, not yet operational.** DXMon's backend (ADXO ingestion,
> a web curation UI, and a real-time HamAlert listener) is fully built and running
> against live data. The device firmware doesn't exist yet -- see
> [Current Status](#current-status) below.

## What DXMon does (planned)

- **Watched:** track a curated list of specific DXpedition callsigns, with real-time
  on-air alerts (band, mode, precise frequency, spotter, and -- for real DX-Cluster
  spots -- any operating notes like split/offset info, e.g. "up 2.4").
- **Needed:** surface DXCC entities you've never confirmed, ranked by rarity and
  cross-referenced against currently-active/upcoming DXpeditions.
- **Overview:** an at-a-glance summary of both, on the device screen.
- **Trigger Builder:** generates ready-to-paste HamAlert trigger recipes for any
  callsign or DXCC entity -- HamAlert has no API for creating triggers automatically,
  so this does the recipe-splitting math (avoiding HamAlert's spot-volume limits) and
  you paste the result into hamalert.org yourself.
- All curation (deciding what to watch, entity sourcing/ranking, building triggers)
  happens through a LAN-only web UI -- the device itself is a thin display/poll
  client, not where you manage configuration. **This web UI is turning out to be
  useful in its own right** -- worth keeping in mind as a possible standalone
  consumption method alongside the eventual ESP32 device, not purely a
  before-firmware development aid.

## Hardware

Unlike PropMon and APRSMon (LilyGO T-Encoder Pro, round AMOLED + rotary encoder),
DXMon uses a **Waveshare ESP32-S3-Touch-LCD-4.3B** -- a 4.3", 800x480, touch-capable
landscape display. DXMon's content is inherently list/roster-heavy, which the round
displays' proven text-width limitations don't suit well; touch replaces the rotary
encoder entirely as the input model. This is a deliberate deviation from the rest of
the series, not scope creep.

## Data sources

- **[NG3K's Announced DX Operations (ADXO)](https://www.ng3k.com/Misc/adxoplain.html)**,
  used with permission from Bill Feidt/NG3K -- the discovery source for browsing and
  selecting what to watch. Polled once daily; see `server/README.md` for the specific
  polling agreement this project is bound to.
- **[HamAlert](https://hamalert.org/)** -- real-time spot matching for watched
  callsigns and (post-v1) needed-entity alerting. This is the actual real-time
  alerting path; DXMon's core function does not depend on ADXO's continued
  availability.

## Repository structure

```
n4mi-dx-monitor/
├── server/        -- backend (Flask): ADXO ingestion, web curation UI, HamAlert
│                     listener, Trigger Builder. Built, deployed, running.
├── firmware/       -- ESP32-S3 / LVGL firmware. Not yet started.
└── README.md       -- this file
```

## Current status

| Piece | Status |
|---|---|
| ADXO ingestion service | Built, deployed, confirmed working against live data |
| Web curation UI (browse ADXO, manage watchlist) | Built, deployed, confirmed working -- including persistence across a container restart |
| HamAlert Telnet listener (real-time spot matching) | Built, deployed, confirmed working against live spots (band/mode/frequency/source/comment/spotter), including an enable/disable toggle for extended absences from the shack |
| Trigger Builder (generates ready-to-paste HamAlert trigger recipes) | Built, deployed, confirmed working |
| Merged watched-status JSON (`/api/dxmon/watched`) | Built, deployed, confirmed working -- joins watchlist + ADXO status + latest HamAlert spot per entry |
| Needed-entity feature | Not yet built |
| Virtual device preview (in-browser, for testing before firmware) | Not yet built |
| Firmware | Not started -- hardware (Waveshare ESP32-S3-Touch-LCD-4.3B) has reached the US, expected soon |

This project follows the series' established practice: design before code, real-data
testing before deployment, hardware confirmation before any firmware commit. Nothing
here should be assumed device-ready until the firmware section above says so.

## Planned next

1. **Beam heading to target station** -- using `pyhamtools` and Club Log's `cty.xml`
   country-file data against Dan's known grid square (EM83); self-contained, no
   external dependencies still pending.
2. **Virtual DXMon preview** -- an in-browser rendering of the device screens, fed by
   the same JSON the firmware will eventually consume. The Watched-side data shape is
   effectively already proven via `/api/dxmon/watched`; the Needed side awaits that
   feature's own build.
3. **Alert when a needed (never-confirmed) DXCC entity appears on ADXO** -- currently
   you'd only notice this by checking the Needed tab; a proactive alert closes that
   gap. Needs its own short design pass (distinguishing "newly announced" from "went
   active," plus day-over-day diffing the ADXO service doesn't currently do).
4. **Firmware** -- once hardware arrives and the schema/preview above are further
   along.

## Credit

DXpedition schedule data courtesy of Bill Feidt/NG3K's
[Announced DX Operations](https://www.ng3k.com/Misc/adxoplain.html), used with his
permission. A great long-running resource for the ham radio community -- go check it
out directly if you're not already using it.

## License

Not yet decided. This project will be freely available and open for anyone to build
their own instrument, matching the rest of the N4MI Desktop Instrument Series.
