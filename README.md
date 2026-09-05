# DXMon -- DX Monitor

**"Is a DX station I care about active right now, and where should I listen?"**

DXMon is the third instrument in the **N4MI Desktop Instrument Series** -- a family of
small, glanceable desk instruments for the ham shack. Each one answers a single
operational question at a glance:

- **[PropMon](https://github.com/N4MI73/n4mi-propagation-monitor)** -- "Can I make the contact?" (HF band conditions, solar indices)
- **[APRSMon](https://github.com/N4MI73/n4mi-aprs-monitor)** -- "What's happening around me?" (local weather + APRS activity)
- **DXMon** (this repo) -- "Is a DX station I care about active right now, and where should I listen?"

> **Status: backend and firmware both feature-complete and confirmed on real hardware,
> all four tabs live with real data.** DXMon's backend -- ADXO ingestion, a web curation
> UI, a real-time HamAlert listener, a Trigger Builder, beam heading, a unified Needed
> feed with persistent history, and an in-browser virtual device preview -- is fully
> built and confirmed against live data, including real HamAlert triggers created
> directly from the tool. Firmware has a real four-tab UI: Overview (live Watched +
> Needed summary), full Watched and Needed rosters, and a Config screen -- all confirmed
> working with genuinely live data on real hardware, including a scheduled-reboot
> mitigation for a display-rendering quirk that resisted root-causing despite a real,
> thorough investigation. See [Current Status](#current-status) below.

## What DXMon does

- **Watched:** track a curated list of specific DXpedition callsigns, with real-time
  on-air alerts (band, mode, precise frequency, spotter, and -- for real DX-Cluster
  spots -- any operating notes like split/offset info, e.g. "up 2.4"). **Overview
  panel and a full scrollable roster both live on real hardware**, auto-refreshing.
- **Needed:** a single, deliberately curated list -- exactly what you've set up real
  HamAlert triggers for -- covering both whole never-confirmed DXCC entities (any
  band/mode) and specific band/mode gaps on entities you've already confirmed,
  distinguished only by a derived ENTITY/SLOT badge, not a stored type field.
  Cross-referenced in real time against HamAlert spots. Multi-band/mode entries (e.g.
  "17m, 15m" on one entity) are matched correctly against any one of the listed
  values -- a real bug in this exact scenario was found and fixed via live testing.
  **Backend and firmware both built and confirmed live**, including persistent
  last-seen history that survives HamAlert's own rolling spot buffer aging out a rare
  hit.
- **Overview:** an at-a-glance summary on the real device screen -- Watched on the
  left, the unified Needed feed on the right, both live. The right panel uses a
  three-tier design so a quiet day never looks broken: a live hit (if anything's
  active right now), a "last hit N days ago" summary (if there's real history but
  nothing live), or a tracked-count line with a slowly rotating list of tracked
  entities (if nothing's ever hit yet).
- **Web curation UI:** band and mode are entered via checkboxes -- matching exactly
  what HamAlert's own trigger-condition editor supports for these entries -- rather
  than free text, closing off a real typo/separator class of bug found during
  development. Existing Needed entries can be edited in place (not just added/
  removed), entries sitting in the list 30+ days get a visible flag, and a
  convenience checkbox in the Trigger Builder can add an entity straight to Needed
  while generating its trigger recipe.
- **Trigger Builder:** generates ready-to-paste HamAlert trigger recipes for any
  callsign or DXCC entity -- HamAlert has no API for creating triggers automatically,
  so this does the recipe-splitting math (avoiding HamAlert's spot-volume limits) and
  you paste the result into hamalert.org yourself.
- **Beam heading:** great-circle bearing and distance from the operator's station to
  any watched callsign, computed via `pyhamtools` against Club Log/country-files.com
  reference data. Shown on the Watched page and as a quick side-panel lookup for any
  callsign currently appearing in the live spot feed.
- **Virtual device preview:** an in-browser, 800x480 rendering of the actual device
  screens, fed by the same JSON the firmware consumes. (Its own Needed tab still shows
  a placeholder predating the unified-list design -- see Planned Next.)
- All curation happens through a LAN-only web UI -- the device itself is a thin
  display/poll client, not where you manage configuration. This web UI is useful
  enough in its own right that it's worth treating as a standalone consumption method
  alongside the ESP32 device, not purely a before-firmware development aid.

## Hardware

Unlike PropMon and APRSMon (LilyGO T-Encoder Pro, round AMOLED + rotary encoder),
DXMon uses a **Waveshare ESP32-S3-Touch-LCD-4.3B** -- a 4.3", 800x480, touch-capable
landscape display. DXMon's content is inherently list/roster-heavy, which the round
displays' proven text-width limitations don't suit well; touch replaces the rotary
encoder entirely as the input model. This is a deliberate deviation from the rest of
the series, not scope creep -- and it's proven working end-to-end, from panel timing
through touch input through live Wi-Fi data across real rosters.

**A known, unresolved display-rendering quirk** (the tab bar and other content can
periodically shift vertically, self-correcting on a device reset) was investigated at
length -- available memory, the RGB bounce buffer size, and the panel's pixel clock
were all tested and ruled out or reversed, and the display driver library was
confirmed to expose no diagnostic hook for the underlying DMA/panel timing. Rather
than continue chasing an inaccessible root cause, the device automates the one action
already proven 100% reliable at clearing it: a scheduled reboot, currently every 15
minutes (tuned down from an initial 30 as real-world occurrences showed longer
intervals weren't consistently ahead of the glitch's onset). Confirmed working
unattended in real daily use.

## Data sources

- **[NG3K's Announced DX Operations (ADXO)](https://www.ng3k.com/Misc/adxoplain.html)**,
  used with permission from Bill Feidt/NG3K -- the discovery source for browsing and
  selecting what to watch, and for the "starts in N days" data on upcoming
  DXpeditions. Polled once daily.
- **[HamAlert](https://hamalert.org/)** -- real-time spot matching for Watched
  callsigns and the unified Needed feed. This is the actual real-time alerting path;
  DXMon's core function does not depend on ADXO's continued availability.

## Repository structure

```
n4mi-dx-monitor/
├── server/        -- backend (Flask): ADXO ingestion, web curation UI, HamAlert
│                     listener, Trigger Builder, beam heading, virtual preview,
│                     the unified Needed list. Built, deployed, running.
├── firmware/       -- ESP32-S3 / LVGL / PlatformIO firmware. Four-tab UI
│                     (Overview/Watched/Needed/Config) built and confirmed live.
└── README.md       -- this file
```

## Current status

| Piece | Status |
|---|---|
| ADXO ingestion service | Built, deployed, confirmed working against live data |
| Web curation UI (browse ADXO, manage watchlist, unified Needed list) | Built, deployed, confirmed working -- checkbox-based band/mode input, in-place editing, and a 30-day stale-entry flag all added following real usage |
| HamAlert Telnet listener (real-time spot matching) | Built, deployed, confirmed working against live spots, including an enable/disable toggle for extended absences from the shack |
| Trigger Builder (generates ready-to-paste HamAlert trigger recipes) | Built, deployed, confirmed working with real HamAlert triggers created via the tool; a convenience checkbox can add an entity to Needed while building its trigger |
| Merged watched-status JSON (`/api/dxmon/watched`) | Built, deployed, confirmed working -- joins watchlist + ADXO status + latest HamAlert spot + beam heading per entry |
| Unified Needed feed (`/api/dxmon/needed`) | Built, deployed, **confirmed live** -- entity-name matching (no DXCC-number lookup needed), correct multi-value band/mode matching (a real bug in this exact area was found and fixed via live testing), plus persistent per-entity last-seen tracking that survives HamAlert's own rolling spot buffer aging out a rare hit |
| Beam heading (great-circle bearing/distance to any watched callsign) | Built, tested, confirmed live |
| Virtual device preview (`/preview`, in-browser) | Built, tested, confirmed live -- Needed tab still shows a placeholder predating the unified-list design, see Planned Next |
| Firmware toolchain (PlatformIO, pinned libraries, named-board display config) | Confirmed working on real hardware. Pinned to pioarduino 51.03.07 (Arduino core 3.0.7), ESP32_Display_Panel 1.0.4, ESP32_IO_Expander 1.1.1, esp-lib-utils 0.3.0, LVGL 8.4.0, ArduinoJson 7.4.3 |
| Firmware UI -- four-tab navigation (Overview/Watched/Needed/Config) | **Built and confirmed on real hardware**, touch-switching all four tabs, all with real content |
| Firmware UI -- Overview screen | **Built and confirmed live** -- Watched panel (left) and the unified Needed panel (right, three-tier design) both auto-refreshing with real data |
| Firmware UI -- Watched roster (tab) | **Built and confirmed live** -- full touch-scrollable roster, real day-count math for upcoming ADXO entries, computed from the server's own clock (no device-side NTP needed) |
| Firmware UI -- Needed roster (tab) | **Built and confirmed live** -- unified curated-list roster, derived ENTITY/SLOT badges, three real states (live/recently-seen/never-spotted) |
| Firmware UI -- Config screen | **Built and confirmed live** -- Wi-Fi/ADXO/HamAlert status, watched count, curate-at URL, firmware version, a working Force Refresh button. Wi-Fi Setup is a visible, deliberately disabled placeholder (a real captive-portal flow is a bigger future task) |
| Display-rendering quirk (periodic vertical shift, self-correcting on reset) | Root cause not found despite a thorough real investigation (heap, bounce buffer, pixel clock, driver diagnostic access all ruled out or closed off). **Mitigated via a scheduled reboot** (currently 15 minutes), confirmed working unattended in real daily use |

This project follows the series' established practice: design before code, real-data
testing before deployment, hardware confirmation before any firmware commit. Nothing
here should be assumed device-ready until the status above says so.

## Planned next

1. **Spot-history drill-down screens** -- tapping an Overview panel (Watched or
   Needed) should open a full-screen recent-spot-history view for that category;
   tapping a specific callsign in the Watched roster should open that entry's last 10
   spots. Both designed in concept, neither built yet.
2. **Manual re-ordering of curated Needed entries** on the web curation page.
3. **A Watched-page flag for ended DXpeditions**, so the Watched entry and its
   HamAlert trigger can be cleaned up together -- blocked on a real design question
   about whether ADXO's own data makes "ended" distinguishable from "never linked."
4. **Spotter-Continent filter for the Trigger Builder** -- real evidence (a single
   popular DXpedition callsign hit HamAlert's 10,000-spots/day ceiling on its own)
   showed this is needed even for single-callsign triggers, not just broad
   multi-entity ones.
5. **Real elapsed-time display** for live "Xm ago"-style timestamps (currently plain
   reformatted dates/times computed from the server's own clock -- real day-count
   math now exists for the "starts in N days" case, but a continuously-updating
   elapsed-time display would need device-side NTP sync).
6. **A real Wi-Fi Setup captive-portal flow** on the Config screen -- currently a
   visible, deliberately disabled placeholder; both sibling instruments (PropMon,
   APRSMon) needed multiple dedicated sessions to build this feature safely.
7. **Defensive null-checking after LVGL object-creation calls** -- a real memory
   lesson from building the Needed roster at scale during an earlier design: the fix
   in place should prevent recurrence at current data size, but a much larger future
   entity list could theoretically hit the same wall again with no graceful fallback.
8. **Tab bar icon glyphs** matching the original mockup's custom vector shapes --
   currently text-only tabs, a visual-polish item.
9. **Update the virtual web preview's Needed tab** to match the current unified-list
   design (currently a placeholder predating that redesign).
10. **A fresh SVG mockup for the Needed screen**, reflecting both the unified-list
    architecture and the checkbox-based curation UI -- firmware has run ahead of the
    mockup at this point.

## Credit

DXpedition schedule data courtesy of Bill Feidt/NG3K's
[Announced DX Operations](https://www.ng3k.com/Misc/adxoplain.html), used with his
permission. A great long-running resource for the ham radio community -- go check it
out directly if you're not already using it.

## License

Not yet decided. This project will be freely available and open for anyone to build
their own instrument, matching the rest of the N4MI Desktop Instrument Series.
