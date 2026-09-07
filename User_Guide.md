# DXMon User Guide

This guide covers what DXMon does and how to use it, once both the
[server](server/README.md) and [firmware](firmware/README.md) are set up and
running. It doesn't cover installation -- see those two READMEs for that.

## The big picture

DXMon tracks two kinds of things:

- **Watched** -- specific DXpedition callsigns you're following (e.g. a
  callsign currently operating from a rare entity).
- **Needed** -- DXCC entities or specific band/mode gaps you still need,
  curated by you as a short list matching exactly what you've set up real
  HamAlert alerts for.

Everything you curate on the web pages shows up on the physical device,
updated automatically as real spots come in from HamAlert.

## The device screens

The device has four tabs across the bottom: **Overview**, **Watched**,
**Needed**, and **Config**.

### Overview

Two panels side by side. The left panel shows your Watched list; the right
shows your Needed list. Each panel shows:

- The featured entry -- whichever one has the most recent real activity.
- Frequency, mode, and when it was last spotted.
- The spotted callsign and a beam heading + distance to it (e.g. "5A1AL --
  62 deg / 9186 km"), so you know which direction to point your antenna.
- A count of how many other entries you're tracking.

If nothing's currently active, the panel falls back to showing the last real
hit you had, or -- if nothing's ever hit yet -- a simple tracked-count line.

**Tap either panel** to open a full-screen **Recent Activity** feed for that
category -- a scrolling list of every real spot across everything in that
category, most recent first.

### Watched (tab)

The full list of everything you're watching, each showing its own status
(active/upcoming/waiting for a spot), last spot detail, and beam heading.
Scroll to see everything.

### Needed (tab)

The full list of everything in your curated Needed list. Each entry shows an
**ENTITY** or **SLOT** badge:

- **ENTITY** -- a whole never-confirmed DXCC entity, any band or mode.
- **SLOT** -- a specific band/mode gap on an entity you've already confirmed.

Since a single Needed entry can be worked by different callsigns over time
(e.g. different DXpeditions activating the same rare entity), the roster
shows whichever callsign was actually heard for each real hit.

### Config

Shows Wi-Fi connection status and IP address, whether the ADXO and HamAlert
backend connections are healthy, how many entries you're watching, the URL
for the web curation pages, and the firmware version. Includes a **Force
Refresh** button if you want to pull fresh data immediately rather than
waiting for the next automatic update.

## Curating your lists (web pages)

All curation happens on the web pages served by the backend --
`http://<your-server-ip>:8083/`. The device itself never edits your lists;
it only displays what you've curated here.

### Browse ADXO (`/`)

Browse currently-announced DXpeditions and add any you want to follow to
your Watched list with one click.

### Watched (`/watched`)

Your curated Watched list. Remove an entry, or use the **Create HamAlert
trigger** link next to it to get a ready-to-paste trigger recipe for that
callsign.

### Needed (`/needed`)

Your curated Needed list. To add an entry:

1. Type the entity name -- **copy it exactly from HamAlert's own DXCC
   condition picker** when you build the matching trigger. Matching only
   handles case and spacing differences, not spelling differences, so this
   is the one step worth getting right.
2. Check any bands and/or modes for a specific slot, or leave everything
   unchecked for a whole-entity target.
3. Optionally add a note, then submit.

Existing entries can be **edited in place** (no need to delete and re-add),
and any entry sitting in the list 30+ days gets a small flag as a reminder to
review whether it's still worth tracking.

### Trigger Builder (`/triggers`)

Generates a ready-to-paste HamAlert trigger recipe for any callsign or DXCC
entity. HamAlert has no API for creating triggers automatically -- you still
paste the result into hamalert.org yourself -- but this tool does the
recipe-splitting math for you (avoiding HamAlert's own spot-volume limits on
large multi-entity triggers).

If you're building a trigger for an entity, check **"also add to Needed"**
to curate it as a whole-entity target at the same time, without a separate
trip to the Needed page.

### HamAlert (`/hamalert`)

Shows recent real spots HamAlert's own triggers have matched, plus a beam
headings side panel for every distinct callsign currently in that list.
Useful for eyeballing what's actually coming through before it shows up on
the device.

### Preview (`/preview`)

An in-browser rendering of the device's own screens, useful as a quick
remote check when the physical device isn't nearby. It's meant as a
good-enough glance, not an exact mirror of every device screen.

## Beam heading

Wherever you see a callsign next to a heading and distance (device screens,
the HamAlert page, the Watched/Needed web pages), that's a great-circle
bearing and distance from your own station to that callsign, computed
automatically. If a heading is ever missing, the underlying lookup couldn't
resolve that particular callsign -- nothing to configure, it just means that
one entry didn't have a usable heading calculated for it.

## Tips

- **Entity names must match HamAlert's own naming** for a Needed target to
  actually catch spots. If a real spot you know matches never shows up, the
  most likely cause is a spelling mismatch between what's curated and what
  HamAlert itself resolved the entity to.
- **A curated Needed entry with no matching HamAlert trigger will never show
  activity**, even if real spots for that entity/callsign exist elsewhere.
  Curating something here doesn't create a HamAlert trigger by itself -- you
  still need to build and paste in the actual trigger.
- **The Watched and Needed roster order** reflects the underlying data's own
  real-time status (live hit, upcoming, etc.), not simply the order you
  added things.
