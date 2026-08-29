# DXMon -- DX Monitor

**"Is a DX station I care about active right now, and where should I listen?"**

DXMon is the third instrument in the **N4MI Desktop Instrument Series** -- a family of
small, glanceable desk instruments for the ham shack. Each one answers a single
operational question at a glance:

- **[PropMon](https://github.com/N4MI73/n4mi-propagation-monitor)** -- "Can I make the contact?" (HF band conditions, solar indices)
- **[APRSMon](https://github.com/N4MI73/n4mi-aprs-monitor)** -- "What's happening around me?" (local weather + APRS activity)
- **DXMon** (this repo) -- "Is a DX station I care about active right now, and where should I listen?"

> **Status: backend feature-complete, firmware toolchain proven, real UI + live data
> confirmed on real hardware.** DXMon's backend -- ADXO ingestion, a web curation UI,
> a real-time HamAlert listener, a Trigger Builder, beam heading, a Wanted list, and
> an in-browser virtual device preview -- is fully built and confirmed against live
> data, including real HamAlert triggers created directly from the tool. Firmware now
> has a real four-tab UI on the Waveshare display, with the Overview screen showing
> genuinely live watched-DX data over Wi-Fi, confirmed on real hardware. See
> [Current Status](#current-status) below.

## What DXMon does

- **Watched:** track a curated list of specific DXpedition callsigns, with real-time
  on-air alerts (band, mode, precise frequency, spotter, and -- for real DX-Cluster
  spots -- any operating notes like split/offset info, e.g. "up 2.4"). **Overview
  screen live on real hardware**, showing the most relevant watched entry and
  refreshing automatically.
- **Needed:** surface DXCC entities you've never confirmed, ranked by rarity and
  cross-referenced against currently-active/upcoming DXpeditions. Backend not yet
  built; firmware shows an honest "Not yet built" placeholder.
- **Wanted:** a small, manually-curated list of specific band/mode gaps on entities
  you've already confirmed -- distinct from Needed, which tracks whole never-confirmed
  entities. Always expected to be a short list. Web-only for now; device placement
  still undecided.
- **Overview:** an at-a-glance summary, on the real device screen -- watched DX on the
  left (live), needed DX on the right (placeholder, pending the backend feature).
- **Trigger Builder:** generates ready-to-paste HamAlert trigger recipes for any
  callsign or DXCC entity -- HamAlert has no API for creating triggers automatically,
  so this does the recipe-splitting math (avoiding HamAlert's spot-volume limits) and
  you paste the result into hamalert.org yourself.
- **Beam heading:** great-circle bearing and distance from the operator's station to
  any watched callsign, computed via `pyhamtools` against Club Log/country-files.com
  reference data. Shown on the Watched page and as a quick side-panel lookup for any
  callsign currently appearing in the live spot feed.
- **Virtual device preview:** an in-browser, 800x480 rendering of the actual device
  screens (Overview/Watched/Needed/Config, with the real tab navigation), fed by the
  same JSON the firmware consumes -- lets screen layout and data flow get tested and
  iterated on before touching hardware.
- All curation (deciding what to watch, entity sourcing/ranking, building triggers)
  happens through a LAN-only web UI -- the device itself is a thin display/poll
  client, not where you manage configuration. This web UI is proving useful enough in
  its own right that it's worth treating as a standalone consumption method alongside
  the ESP32 device, not purely a before-firmware development aid.

## Hardware

Unlike PropMon and APRSMon (LilyGO T-Encoder Pro, round AMOLED + rotary encoder),
DXMon uses a **Waveshare ESP32-S3-Touch-LCD-4.3B** -- a 4.3", 800x480, touch-capable
landscape display. DXMon's content is inherently list/roster-heavy, which the round
displays' proven text-width limitations don't suit well; touch replaces the rotary
encoder entirely as the input model. This is a deliberate deviation from the rest of
the series, not scope creep -- and it's now proven working end-to-end, from panel
timing through touch input through live Wi-Fi data.

## Data sources

- **[NG3K's Announced DX Operations (ADXO)](https://www.ng3k.com/Misc/adxoplain.html)**,
  used with permission from Bill Feidt/NG3K -- the discovery source for browsing and
  selecting what to watch. Polled once daily.
- **[HamAlert](https://hamalert.org/)** -- real-time spot matching for watched
  callsigns and (post-v1) needed-entity alerting. This is the actual real-time
  alerting path; DXMon's core function does not depend on ADXO's continued
  availability.

## Repository structure

```
n4mi-dx-monitor/
├── server/        -- backend (Flask): ADXO ingestion, web curation UI, HamAlert
│                     listener, Trigger Builder, beam heading, virtual preview.
│                     Built, deployed, running.
├── firmware/       -- ESP32-S3 / LVGL / PlatformIO firmware.
│                     Toolchain proven, real four-tab UI built, live data confirmed.
└── README.md       -- this file
```

## Current status

| Piece | Status |
|---|---|
| ADXO ingestion service | Built, deployed, confirmed working against live data |
| Web curation UI (browse ADXO, manage watchlist) | Built, deployed, confirmed working -- including persistence across a container restart |
| HamAlert Telnet listener (real-time spot matching) | Built, deployed, confirmed working against live spots, including an enable/disable toggle for extended absences from the shack |
| Trigger Builder (generates ready-to-paste HamAlert trigger recipes) | Built, deployed, **confirmed working with real HamAlert triggers created via the tool** |
| Merged watched-status JSON (`/api/dxmon/watched`) | Built, deployed, confirmed working -- joins watchlist + ADXO status + latest HamAlert spot + beam heading per entry; real wrapper shape and null-handling confirmed against live data |
| Beam heading (great-circle bearing/distance to any watched callsign) | Built, tested, confirmed live via screenshot |
| Virtual device preview (`/preview`, in-browser, for testing before firmware) | Built, tested, confirmed live via screenshot |
| Wanted (small, manually-curated band/mode gaps on confirmed entities) | Built, tested, **confirmed live with real HamAlert triggers** |
| Needed-entity feature | Not yet built |
| Firmware toolchain (PlatformIO, pinned libraries, named-board display config) | **Confirmed working on real hardware.** Pinned to pioarduino 51.03.07 (Arduino core 3.0.7), ESP32_Display_Panel 1.0.4, ESP32_IO_Expander 1.1.1, esp-lib-utils 0.3.0, LVGL 8.4.0, ArduinoJson 7.4.3. |
| Firmware UI -- four-tab navigation (Overview/Watched/Needed/Config) | **Built and confirmed on real hardware**, touch-switching all four tabs. Watched/Needed/Config content still honest placeholders. |
| Firmware UI -- Overview screen | **Built and confirmed on real hardware with live data.** Real Wi-Fi + HTTP fetch from `/api/dxmon/watched`, auto-refreshing every 60s, confirmed against multiple real spots across a session. NEEDED panel is an honest placeholder pending the backend feature. |

This project follows the series' established practice: design before code, real-data
testing before deployment, hardware confirmation before any firmware commit. Nothing
here should be assumed device-ready until the status above says so.

## Planned next

1. **Watched, Needed, and Config tab content** -- currently honest placeholders.
   Each needs a mockup-approval pass (Overview's mockup was approval-quality;
   Watched/Needed/Config mockups are still rough/illustrative) before real
   firmware content gets built.
2. **Wi-Fi status indicator** -- a small always-visible glyph on Overview (quick
   glance-check), and full Wi-Fi status/IP detail on Config once its real content
   is built -- matching how PropMon and APRSMon both surface this.
3. **Spotter-Continent filter for the Trigger Builder** -- real evidence (a single
   popular DXpedition callsign hit HamAlert's 10,000-spots/day ceiling on its own)
   showed this is needed even for single-callsign triggers, not just broad
   multi-entity ones.
4. **The Needed-entity feature itself** -- still entirely unbuilt; only the ranking
   design exists so far.
5. **Wanted's device-side/Overview placement** -- currently web-only; the plan is to
   share the Needed panel space, visually distinguished, once Needed itself exists.
6. **Alert when a needed (never-confirmed) DXCC entity appears on ADXO** -- currently
   you'd only notice by checking the Needed tab; needs its own short design pass
   (distinguishing "newly announced" from "went active," plus day-over-day diffing
   the ADXO service doesn't currently do).
7. **Real elapsed-time display** on device timestamps (currently plain reformatted
   dates/times, not computed "N minutes ago" -- would need NTP sync and real date
   arithmetic).

## Credit

DXpedition schedule data courtesy of Bill Feidt/NG3K's
[Announced DX Operations](https://www.ng3k.com/Misc/adxoplain.html), used with his
permission. A great long-running resource for the ham radio community -- go check it
out directly if you're not already using it.

## License

Not yet decided. This project will be freely available and open for anyone to build
their own instrument, matching the rest of the N4MI Desktop Instrument Series.
