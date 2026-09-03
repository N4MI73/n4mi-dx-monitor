# DXMon -- DX Monitor

**"Is a DX station I care about active right now, and where should I listen?"**

DXMon is the third instrument in the **N4MI Desktop Instrument Series** -- a family of
small, glanceable desk instruments for the ham shack. Each one answers a single
operational question at a glance:

- **[PropMon](https://github.com/N4MI73/n4mi-propagation-monitor)** -- "Can I make the contact?" (HF band conditions, solar indices)
- **[APRSMon](https://github.com/N4MI73/n4mi-aprs-monitor)** -- "What's happening around me?" (local weather + APRS activity)
- **DXMon** (this repo) -- "Is a DX station I care about active right now, and where should I listen?"

> **Status: backend feature-complete, firmware UI substantially complete, all four
> tabs live with real data on real hardware.** DXMon's backend -- ADXO ingestion, a
> web curation UI, a real-time HamAlert listener, a Trigger Builder, beam heading, a
> merged Needed+Wanted feed with persistent history, and an in-browser virtual device
> preview -- is fully built and confirmed against live data, including real HamAlert
> triggers created directly from the tool. Firmware has a real four-tab UI: Overview
> (live Watched + Needed/Wanted summary), full Watched and Needed rosters, and a
> Config screen -- all confirmed working with genuinely live data on real hardware.
> See [Current Status](#current-status) below.

## What DXMon does

- **Watched:** track a curated list of specific DXpedition callsigns, with real-time
  on-air alerts (band, mode, precise frequency, spotter, and -- for real DX-Cluster
  spots -- any operating notes like split/offset info, e.g. "up 2.4"). **Overview
  panel and a full scrollable roster both live on real hardware**, auto-refreshing.
- **Needed:** DXCC entities you've never confirmed, cross-referenced in real time
  against HamAlert spots and ADXO's active/upcoming schedule. **Backend and firmware
  both built and confirmed live** -- entity-name matching (no DXCC-number lookup
  needed), persistent last-seen history that survives HamAlert's own rolling spot
  buffer aging out a rare hit, and a real day-count "starts in N days" display for
  upcoming DXpeditions on never-confirmed entities.
- **Wanted:** a small, manually-curated list of specific band/mode gaps on entities
  you've already confirmed -- distinct from Needed, which tracks whole never-confirmed
  entities. **Merged with Needed on the device** -- one tab, one Overview panel,
  entries tagged by type (NEEDED/WANTED badges) -- since both answer the same real
  question ("DX I still need, ranked by how close I am"), just from different sources.
- **Overview:** an at-a-glance summary on the real device screen -- Watched on the
  left, the merged Needed+Wanted feed on the right, both live. The right panel uses a
  three-tier design so a quiet day never looks broken: a live hit (if anything's
  active right now), a "last hit N days ago" summary (if there's real history but
  nothing live), or a tracked-count line with a slowly rotating list of tracked
  entities (if nothing's ever hit yet).
- **Trigger Builder:** generates ready-to-paste HamAlert trigger recipes for any
  callsign or DXCC entity -- HamAlert has no API for creating triggers automatically,
  so this does the recipe-splitting math (avoiding HamAlert's spot-volume limits) and
  you paste the result into hamalert.org yourself.
- **Beam heading:** great-circle bearing and distance from the operator's station to
  any watched callsign, computed via `pyhamtools` against Club Log/country-files.com
  reference data. Shown on the Watched page and as a quick side-panel lookup for any
  callsign currently appearing in the live spot feed.
- **Virtual device preview:** an in-browser, 800x480 rendering of the actual device
  screens, fed by the same JSON the firmware consumes -- lets screen layout and data
  flow get tested and iterated on before touching hardware. (Its own Needed tab
  still shows a placeholder pending an update to match the real firmware's design --
  see Planned Next.)
- All curation (deciding what to watch, entity sourcing/ranking, building triggers)
  happens through a LAN-only web UI -- the device itself is a thin display/poll
  client, not where you manage configuration. This web UI is useful enough in its own
  right that it's worth treating as a standalone consumption method alongside the
  ESP32 device, not purely a before-firmware development aid.

## Hardware

Unlike PropMon and APRSMon (LilyGO T-Encoder Pro, round AMOLED + rotary encoder),
DXMon uses a **Waveshare ESP32-S3-Touch-LCD-4.3B** -- a 4.3", 800x480, touch-capable
landscape display. DXMon's content is inherently list/roster-heavy, which the round
displays' proven text-width limitations don't suit well; touch replaces the rotary
encoder entirely as the input model. This is a deliberate deviation from the rest of
the series, not scope creep -- and it's proven working end-to-end, from panel timing
through touch input through live Wi-Fi data across real rosters of 60+ entries.

## Data sources

- **[NG3K's Announced DX Operations (ADXO)](https://www.ng3k.com/Misc/adxoplain.html)**,
  used with permission from Bill Feidt/NG3K -- the discovery source for browsing and
  selecting what to watch, and for the "starts in N days" data on upcoming
  DXpeditions. Polled once daily.
- **[HamAlert](https://hamalert.org/)** -- real-time spot matching for Watched
  callsigns and the merged Needed+Wanted feed. This is the actual real-time alerting
  path; DXMon's core function does not depend on ADXO's continued availability.

## Repository structure

```
n4mi-dx-monitor/
├── server/        -- backend (Flask): ADXO ingestion, web curation UI, HamAlert
│                     listener, Trigger Builder, beam heading, virtual preview,
│                     merged Needed+Wanted feed with persistent last-seen tracking.
│                     Built, deployed, running.
├── firmware/       -- ESP32-S3 / LVGL / PlatformIO firmware. Four-tab UI
│                     (Overview/Watched/Needed/Config) built and confirmed live.
└── README.md       -- this file
```

## Current status

| Piece | Status |
|---|---|
| ADXO ingestion service | Built, deployed, confirmed working against live data |
| Web curation UI (browse ADXO, manage watchlist) | Built, deployed, confirmed working -- including persistence across a container restart |
| HamAlert Telnet listener (real-time spot matching) | Built, deployed, confirmed working against live spots, including an enable/disable toggle for extended absences from the shack |
| Trigger Builder (generates ready-to-paste HamAlert trigger recipes) | Built, deployed, confirmed working with real HamAlert triggers created via the tool |
| Merged watched-status JSON (`/api/dxmon/watched`) | Built, deployed, confirmed working -- joins watchlist + ADXO status + latest HamAlert spot + beam heading per entry |
| Merged Needed+Wanted feed (`/api/dxmon/targets`) | Built, deployed, **confirmed live** -- entity-name matching (no DXCC-number lookup needed), real ADXO cross-reference, plus persistent per-entity last-seen tracking that survives HamAlert's own rolling spot buffer aging out a rare hit |
| Beam heading (great-circle bearing/distance to any watched callsign) | Built, tested, confirmed live |
| Virtual device preview (`/preview`, in-browser) | Built, tested, confirmed live -- Needed tab still shows a placeholder pending an update to match real firmware, see Planned Next |
| Firmware toolchain (PlatformIO, pinned libraries, named-board display config) | Confirmed working on real hardware. Pinned to pioarduino 51.03.07 (Arduino core 3.0.7), ESP32_Display_Panel 1.0.4, ESP32_IO_Expander 1.1.1, esp-lib-utils 0.3.0, LVGL 8.4.0, ArduinoJson 7.4.3 |
| Firmware UI -- four-tab navigation (Overview/Watched/Needed/Config) | **Built and confirmed on real hardware**, touch-switching all four tabs, all with real content |
| Firmware UI -- Overview screen | **Built and confirmed live** -- Watched panel (left) and the merged Needed+Wanted panel (right, three-tier design) both auto-refreshing with real data |
| Firmware UI -- Watched roster (tab) | **Built and confirmed live** -- full touch-scrollable roster, real day-count math for upcoming ADXO entries, computed from the server's own clock (no device-side NTP needed) |
| Firmware UI -- Needed roster (tab) | **Built and confirmed live** -- merged Needed+Wanted roster, type badges, five real states (live/recently-seen/awaiting-first-spot/upcoming/never-spotted) |
| Firmware UI -- Config screen | **Built and confirmed live** -- Wi-Fi/ADXO/HamAlert status, watched count, curate-at URL, firmware version, a working Force Refresh button. Wi-Fi Setup is a visible, deliberately disabled placeholder (a real captive-portal flow is a bigger future task) |

This project follows the series' established practice: design before code, real-data
testing before deployment, hardware confirmation before any firmware commit. Nothing
here should be assumed device-ready until the status above says so.

## Planned next

1. **Spot-history drill-down screens** -- tapping an Overview panel (Watched or
   Needed) should open a full-screen recent-spot-history view for that category;
   tapping a specific callsign in the Watched roster should open that entry's last 10
   spots. Both designed in concept, neither built yet.
2. **Spotter-Continent filter for the Trigger Builder** -- real evidence (a single
   popular DXpedition callsign hit HamAlert's 10,000-spots/day ceiling on its own)
   showed this is needed even for single-callsign triggers, not just broad
   multi-entity ones.
3. **Club Log Most Wanted List ranking** -- Needed entities with no ADXO link and no
   live/recent spot currently fall back to plain file order; the original design
   called for ranking them by real Club Log rarity data, not yet integrated.
4. **Alert when a needed (never-confirmed) DXCC entity appears on ADXO** -- currently
   you'd only notice by checking the Needed tab; needs its own short design pass
   (distinguishing "newly announced" from "went active," plus day-over-day diffing
   the ADXO service doesn't currently do).
5. **Real elapsed-time display** for live "Xm ago"-style timestamps (currently plain
   reformatted dates/times computed from the server's own clock -- real day-count
   math now exists for the "starts in N days" case, but a continuously-updating
   elapsed-time display would need device-side NTP sync).
6. **A real Wi-Fi Setup captive-portal flow** on the Config screen -- currently a
   visible, deliberately disabled placeholder; both sibling instruments (PropMon,
   APRSMon) needed multiple dedicated sessions to build this feature safely.
7. **Defensive null-checking after LVGL object-creation calls** -- a real memory
   lesson from building the Needed roster at scale (~600 UI objects for a 68-entry
   list): the fix in place should prevent recurrence at current data size, but a
   much larger future entity list could theoretically hit the same wall again with
   no graceful fallback.
8. **Tab bar icon glyphs** matching the original mockup's custom vector shapes --
   currently text-only tabs, a visual-polish item.
9. **Update the virtual web preview's Needed tab** to match the real firmware design
   (currently a placeholder predating the Needed+Wanted merge).

## Credit

DXpedition schedule data courtesy of Bill Feidt/NG3K's
[Announced DX Operations](https://www.ng3k.com/Misc/adxoplain.html), used with his
permission. A great long-running resource for the ham radio community -- go check it
out directly if you're not already using it.

## License

Not yet decided. This project will be freely available and open for anyone to build
their own instrument, matching the rest of the N4MI Desktop Instrument Series.
