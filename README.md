# DXMon -- DX Monitor

**"Is a DX station I care about active right now, and where should I listen?"**

DXMon is the third instrument in the **N4MI Desktop Instrument Series** -- a family of
small, glanceable desk instruments for the ham shack. Each one answers a single
operational question at a glance:

- **[PropMon](https://github.com/N4MI73/n4mi-propagation-monitor)** -- "Can I make the contact?" (HF band conditions, solar indices)
- **[APRSMon](https://github.com/N4MI73/n4mi-aprs-monitor)** -- "What's happening around me?" (local weather + APRS activity)
- **DXMon** (this repo) -- "Is a DX station I care about active right now, and where should I listen?"

> **Status: work in progress.** DXMon is under active development and is **not yet
> operational**. The backend (ADXO ingestion + a web curation UI) is built and running;
> several major phases remain before there's a working device -- see
> [Current Status](#current-status) below.

## What DXMon does (planned)

- **Watched:** track a curated list of specific DXpedition callsigns, with real-time
  on-air alerts.
- **Needed:** surface DXCC entities you've never confirmed, ranked by rarity and
  cross-referenced against currently-active/upcoming DXpeditions.
- **Overview:** an at-a-glance summary of both, on the device screen.
- All curation (deciding what to watch, entity sourcing/ranking) happens through a
  LAN-only web UI -- the device itself is a thin display/poll client, not where you
  manage configuration.

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
├── server/        -- backend (Flask): ADXO ingestion + web curation UI. Built, deployed, running.
├── firmware/       -- ESP32-S3 / LVGL firmware. Not yet started.
└── README.md       -- this file
```

## Current status

| Piece | Status |
|---|---|
| ADXO ingestion service | Built, deployed, confirmed working against live data |
| Web curation UI (browse ADXO, manage watchlist) | Built, deployed, confirmed working -- including persistence across a container restart |
| HamAlert Telnet listener (real-time spot matching) | Not yet built |
| Backend-to-firmware JSON schema | Not yet settled |
| Virtual device preview (in-browser, for testing before firmware) | Not yet built |
| Firmware | Not started |

This project follows the series' established practice: design before code, real-data
testing before deployment, hardware confirmation before any firmware commit. Nothing
here should be assumed device-ready until the firmware section above says so.

## Credit

DXpedition schedule data courtesy of Bill Feidt/NG3K's
[Announced DX Operations](https://www.ng3k.com/Misc/adxoplain.html), used with his
permission. A great long-running resource for the ham radio community -- go check it
out directly if you're not already using it.

## License

Not yet decided. This project will be freely available and open for anyone to build
their own instrument, matching the rest of the N4MI Desktop Instrument Series.
