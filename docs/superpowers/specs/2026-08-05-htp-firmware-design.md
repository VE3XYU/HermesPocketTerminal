# HTP Firmware — Design

Version 0.2 — 2026-08-09. Originally written 2026-08-05; reconciled with the as-built
device at wrap-up (Task 19) — corrections are marked "as built" with the hardware
checkpoint that motivated them.

## 1. Purpose and scope

This document designs the firmware for the Hermes Pocket Terminal: the ESP32-S3 handheld
that speaks the Hermes Terminal Protocol (HTP) to the HTP Bridge. It implements the
device behavior specified in the protocol design (§7 of
`2026-08-04-hermes-terminal-protocol-design.md`) — all of it: capture, upload, status
polling, reply playback, conversation follow-up, scheduled sync, the dashboard,
notifications, the Recordings menu, Settings, and multi-profile Wi-Fi. Delivery is
staged so the device is useful early, but this one design covers the full MVP.

Protocol section references (§3, §5, §7, §8…) point into the protocol design document,
which remains authoritative for everything on the wire.

### Governing principle

Unchanged from the protocol design: the firmware never knows what a task, note, or
reminder is. It renders generic primitives — list, text page, status line, audio — and
matches only on opaque IDs. Every semantic decision belongs to Hermes.

---

## 2. Hardware

The target is a commercial ESP32-S3 voice-notes handheld (board profile
`S3_ePaper_1_54`), with PSRAM. Its stock firmware is kept, unmodified and uncommitted,
in `reference/pala_note/` purely as hardware reference; pin assignments and
initialization order were confirmed from it.

Hardware ground truth established during bring-up (C1/C3): the module is an
ESP32-S3-PICO-1 (LGA56), **N8R8** — 8 MB embedded flash plus 8 MB octal PSRAM. The SD
card must be formatted FAT32 **with long-filename support enabled in the firmware**
(`CONFIG_FATFS_LFN_HEAP=y`): the layout below uses names like `config.json.tmp` that
are illegal under bare 8.3.

| Subsystem | Parts and pins |
|---|---|
| Display | 1.54″ 200×200 monochrome e-paper, SSD1681-class controller, full + partial refresh. SPI2: DC 10, CS 11, SCK 12, MOSI 13, RST 9, BUSY 8. Power gate GPIO 6 (active low) |
| Audio | ES8311 codec (mic **and** speaker), driven via Espressif `esp_codec_dev`. I2S: BCLK 15, WS 38, DOUT 45, DIN 16, MCLK 14. PA enable GPIO 46. Power gate GPIO 42 (active low) |
| Storage | SD card, SD-MMC 1-bit: CLK 39, CMD 41, D0 40 |
| Buttons | Record GPIO 0, Power/menu GPIO 18. Both are deep-sleep EXT1 wake sources (any-low) |
| Power | VBAT hold latch GPIO 17 (active high, held across deep sleep), battery ADC GPIO 4 |
| I2C (SDA 47, SCL 48) | ES8311 control, **PCF85063** RTC at 0x51, SHTC3 temperature/humidity at 0x70 (unused in MVP). As-built correction (C6): the design originally assumed a PCF8563; the part is a PCF85063 with a different register layout — a PCF8563 driver would read alarm/control registers as time. Verified against the reference firmware's register map |

Facts the design leans on, proven by the stock firmware on this exact board: I2S capture
streams to SD comfortably at 16 kHz; the codec's capture path delivers interleaved
stereo frames from which one channel is kept; playback duplicates mono to stereo; the
power rails are switchable per subsystem.

---

## 3. Platform

**ESP-IDF, pure — no Arduino layer.** CMake project built with `idf.py`, pinned to a
specific IDF v5.x release (exact version fixed at implementation time and recorded in
the repo). Reasons:

- Deep sleep, EXT1/RTC wake, power-rail gating, and radio-time control are the heart of
  this device, and IDF exposes them first-class.
- `esp_codec_dev` — the trickiest driver — is an Espressif IDF component, taken from
  upstream under Apache-2.0.
- TLS to Let's Encrypt works out of the box via `esp_crt_bundle`.
- Pure-C application components compile as host binaries, which is what makes the TDD
  workflow (§10) possible.

**Deliberately excluded:**

- **LVGL.** The UI vocabulary is a list, a text page, and a status line on a 200×200
  1-bit panel — a 5 KB framebuffer, a font blitter, and ~300 lines of widget code. LVGL
  is animation-oriented and repaint-driven, the opposite of e-paper's
  minimize-refreshes discipline, and would be the firmware's largest component serving
  its smallest need. The `ui` component hides rendering behind display primitives, so
  LVGL could be adopted later without touching application logic.
- **OTA updates.** An OTA path is remote code execution by design and would change the
  protocol's threat model (§9: a leaked token today yields no command surface). With a
  fleet of one device flashed over USB, it buys nothing. The partition table leaves
  room to adopt an OTA layout later; a USB reflash re-partitions anyway.
- **RTOS task architecture** beyond what IDF's drivers impose. The application is a
  sequential loop; the device does one thing at a time by design.

### Licensing constraints

The repository is public. `reference/pala_note/` is third-party, gitignored, and never
copied from. `esp_codec_dev` and the ES8311 driver are vendored from Espressif upstream
(Apache-2.0) with license headers intact. The SSD1681 driver, and everything else, is
written fresh from datasheets and this design.

Typography, as built (C7 rounds 3–6, four legibility escalations): the UI renders a
**proportional Liberation Sans ramp** (small/body/emphasis/hero) from GFX-format glyph
tables generated at metric parity with the reference product's type ramp — Liberation
Sans 2.1.5 under the SIL OFL 1.1, generation script and source hash committed under
`components/ui/fonts/`. Nothing is copied from the reference firmware or from any GFX
library's shipped tables. The 8×8 monospace font the early tasks used is fully
retired.

---

## 4. Architecture

```
firmware/
  main/                      # boot dispatch, config load, interface wiring — thin
  components/
    htp_client/              # HTP protocol: request building, JSON parsing,
                             #   capture states, retry policy      [pure C, host-testable]
    app_core/                # capture / sync / conversation state machines,
                             #   recordings index, sidecars,
                             #   wifi profile selection            [pure C, host-testable]
    board/                   # BSP: SSD1681 e-paper, ES8311 via esp_codec_dev,
                             #   buttons, power rails, SD-MMC, battery ADC, RTC
    ui/                      # 1-bpp framebuffer, font blitter, three widgets:
                             #   list-with-cursor, text page, status line
                             #                                     [pure C, host-testable]
  tests/host/                # unit + integration tests, run on the Linux dev host
  tools/                     # flash instructions and esptool one-liners
```

**The dependency rule.** `htp_client`, `app_core`, and `ui` contain no ESP-IDF
includes. Hardware and OS reach them only through small injected interfaces:

- `transport` — HTTP verbs with timeouts; returns status, headers, body
- `storage` — file open/read/write/rename/delete, directory listing
- `clock` — epoch seconds and monotonic milliseconds
- `rng` — random bytes
- `display` — accepts a framebuffer, performs full or partial refresh

On device, `main/` wires these to IDF implementations; on the development host they are
wired to fakes (or libcurl, for integration tests). This is the same injected-`clock`
discipline the bridge codebase uses — one habit across the project.

---

## 5. Runtime model

### 5.1 Wake dispatch

Deep sleep is the resting state, and firmware boots fresh on every wake — no RAM state
survives; anything persistent lives on SD or in NVS. `main` reads the wake cause and
branches:

| Wake cause | Path |
|---|---|
| Record button (EXT1) | Capture flow, immediately. Only what capture needs is initialized — audio rail, SD, I2S — inside the ~250 ms budget. Display, Wi-Fi join, and everything else start in parallel or after recording begins |
| Power button (EXT1) | UI session: Dashboard screen, button navigation |
| RTC timer | Silent sync cycle (§5.4); no display activity unless something changed |
| Cold boot / reset | Full init, splash, Dashboard, then a sync cycle |

### 5.2 Capture flow

One gesture: press and hold Record, speak, release.

On press, the WAV starts streaming from I2S to `/rec/<capture-id>.wav` on the SD card
while the recording glyph is drawn — and the Wi-Fi join begins in parallel, so the
network is typically up by the time the user stops talking. Recording ends on release,
at the 120-second protocol maximum, or on silence timeout (configurable; default off in
MVP — release is the natural end).

1. **Finalize.** Patch the WAV header, write the sidecar (`not_uploaded`), append to
   `/rec/index`. From this moment the capture is durable; everything after is a retry.
2. **Upload.** `POST /htp/v1/captures`, streamed from SD, with `X-Capture-Id`,
   `X-Capture-Mode: auto`, `X-Recorded-At`, `X-Battery`, and `X-Conversation-Id` when
   continuing a conversation. On `{"state":"received"}`, mark the sidecar uploaded.
3. **Poll.** `GET /htp/v1/captures?ids=<id>` at 1 Hz, branching per protocol §7.2:
   - `done` → confirmation screen (opening transcript words when present), then a sync
     cycle, then sleep.
   - `reply_ready` → download and play the reply (§5.3), open a 30-second follow-up
     window: hold-to-talk records a new capture carrying the returned
     `conversation_id`. Sleep when the window lapses. As built (C7): a **Power tap
     during the window ends the conversation immediately** (with a click and the
     resting outcome screen) rather than waiting out the 30 seconds; a REC press that
     stopped reply playback and stays held rolls directly into the follow-up
     recording.
   - `failed` → error screen; the WAV stays on SD.
   - No terminal state within 60 seconds → sleep. The bridge redirects a late reply to
     the notification queue; nothing is lost.
4. **Offline.** If no profile joins, display "Saved, will upload later" and sleep. Every
   future wake retries pending uploads first; idempotent capture IDs make blind retry
   safe.

### 5.3 Reply audio

Replies are downloaded to `/reply.tmp.wav` in full, then played — no streaming
playback. This survives Wi-Fi hiccups mid-download, and the codec is configured from
the downloaded WAV's actual header: uploads are always 16 kHz, but replies arrive at
the speech provider's native rate (currently 24 kHz), and the header — not an
assumption — drives the I2S clock. The file is deleted after playback; the server
retains the reply for its own retention window.

### 5.4 Sync cycle

Runs on every timer wake and at the end of every capture or UI session, in protocol
§7.3's exact order:

1. Retry pending uploads (oldest first).
2. `GET /notifications`; chime only if an urgent one is present.
3. `GET /dashboard?rev=<cached>`; redraw only when changed. Every dashboard response
   (changed or unchanged) carries `sync_interval` in seconds; the device adopts it as
   the next timer-wake interval and caches it in NVS, so sync cadence is
   server-controlled without a firmware update.
4. Backfill transcripts into sidecars via batch status poll for captures whose sidecar
   lacks a transcript or terminal state; a capture the bridge reports `unknown` is
   marked not-uploaded for the next retry pass.
5. `POST /notifications/ack` for notifications actually rendered to the panel.
6. Sleep.

Every response's `server_time` corrects the RTC whenever drift exceeds two seconds.

### 5.5 Network

- **Profiles.** `wifi.json` on SD, ordered by priority (§7.5). NVS caches the last
  successful BSSID and channel; wake attempts that targeted join first (about a second
  faster than scanning), falling back to a full scan across profiles on failure.
- **HTTP.** `esp_http_client`. HTTPS validates against the built-in certificate bundle
  (Let's Encrypt roots included). A plain `http://` base URL is honored only for
  development against a mock bridge on the local network; the configured URL decides.
- **Retries.** Per protocol §5.8: `401` → auth-error screen, no retry; other `4xx` →
  no retry; `5xx`, timeout, connection failure → exponential backoff (1 s base,
  doubling, capped at 30 s within a session; across sessions the wake schedule is the
  retry cadence). Every request carries a timeout and the `X-Battery` header.

---

## 6. Storage and configuration

### 6.1 SD card layout

```
/config.json               bridge base URL, bearer token, initial sync interval
                           seconds (superseded by the server's `sync_interval`),
                           log level, optional silence-timeout seconds, optional
                           `timezone` (POSIX TZ string, added in C7: the status
                           clock renders local time; absent = UTC)
/wifi.json                 ordered profiles: ssid, password, optional static IP
/rec/<capture-id>.wav      original audio, retained (§7.4 of the protocol design)
/rec/<capture-id>.json     sidecar — the device's per-capture source of truth
/rec/index                 append-only, one line per capture, for fast listing
/reply.tmp.wav             transient reply download
```

Sidecar fields: `state` (device view: `not_uploaded` | `uploaded` | terminal bridge
state), `transcript` (backfilled), `recorded_at`, `uploaded_at`, `conversation_id`
(when assigned). Editing `/config.json` or `/wifi.json` means moving the SD card to a
computer — no reflash. Credentials never leave the card; the bearer token never appears
on the display.

### 6.2 NVS keys

Only small hot state: last-good BSSID + channel, last dashboard `rev`, current sync
interval (as last served by the bridge), recently acked notification IDs (bounded
ring, for §5.7 at-least-once dedup), monotonic boot counter.

### 6.3 Capture IDs

`c-YYYYMMDD-HHMMSS-xxxx` from the RTC plus four random hex characters (protocol §3).
If the RTC has never been set — first boot or a fully dead battery — the fallback is
`c-b<bootcount>-<monotonic-ms>-xxxx`: still unique, no clock required, and within the
bridge's accepted ID alphabet (`[A-Za-z0-9_-]`, max 128). When the RTC is unset the
device also omits `X-Recorded-At` — the bridge treats it as nullable — rather than
sending a nonsense epoch. The ID is the SD filename and the end-to-end idempotency
key; the device never generates it twice.

---

## 7. User interface

### 7.1 Screens

| Screen | Content |
|---|---|
| **Dashboard** (home, and the resting/sleep image) | Hermes-published title and items (done items struck through per their state/style hints), notification banner strip (occupies the last row band while up), status line: battery, Wi-Fi fan glyph, pending-upload count, clock (12-hour, no AM/PM, no leading zero, local per the config `timezone`) |
| **Menu** (as built, C7 — the reference product's interaction model) | Dashboard / Recordings / Settings / Sleep, opened by a Power tap from the resting dashboard |
| **Recordings** | Newest-first two-line transcript previews, or `(pending)` / `(not uploaded)`. **Conversation captures are excluded** — matched on the sidecar's `conversation_id`, a protocol-level mode field stamped at capture time, never a reading of the transcript, so the content-blindness invariant holds. They stay on the card and in every sync path |
| **Entry view** | Full paged transcript of one recording; Record-short plays the WAV |
| **Settings** | MAC address, firmware version, bridge host, sync interval — display-only in MVP |

### 7.2 Gestures

As built (C7 rounds 5–7). The original design used cursor-first navigation with a
double-tap screen cycle; the bench showed the operator's muscle memory (and the
reference product) expect a **menu-first grammar**, and retiring the double-tap lets a
tap resolve on its release edge instead of after a 250 ms wait. Every **accepted**
press gets an instant audio click (low blip = Power/next, high blip = Record/select)
before the e-paper refresh, so feedback never waits on the ink; a press that changes
nothing is silent.

| Gesture | Action |
|---|---|
| Record — hold (≥350 ms) | Capture — always, from any screen. The one sacred gesture |
| Record — short | Select / context action: menu → open row; dashboard (opened) → mark item under cursor complete; recordings → open entry; entry view → play WAV; dashboard banner visible → dismiss |
| Record — press during playback | Stop playback (held past the stop, it rolls into the follow-up recording) |
| Power — short | Next: resting dashboard → open menu; menu/lists → cursor down (wraps); entry view → next page. Advances only when ≥ 2 positions exist, else climbs one level — a tap always lands a visible change |
| Power — long (≥600 ms, fires on release) | Back / up one level (entry → recordings → menu → resting dashboard) |
| Power — tap during the reply follow-up window | End the conversation immediately |
| Power — hold 5 s (fires while held, after an on-screen countdown warning at 2 s) | Power off (release the VBAT latch, after a deliberate blank full refresh — e-paper keeps its last image unpowered) |

Both buttons run an edge-latched FSM with a release debounce
(`GEST_RELEASE_DEBOUNCE_MS`) so contact chatter can neither split a press into two
gestures nor misclassify a hold; press duration is measured to the moment the button
opened, never to the end of the debounce window.

Marking complete sends `POST /complete` immediately and redraws the item as done on
`{"ok": true}` — no confirmation step. The agent is truth (§5.6) and can resurrect an
item, so a mis-press is recoverable; the completion is idempotent.

### 7.3 Notifications

Fetched during sync. Rendered into the Dashboard banner area — e-paper persists
unpowered, so the banner is readable whenever the user glances at the device. `urgent`
adds a chime through the speaker; unknown priorities render as `normal` (§5.7). A
notification is acked only after the framebuffer containing it has been pushed to the
panel; a crash mid-render redelivers on the next poll, and the NVS acked-ID ring
deduplicates.

### 7.4 Refresh discipline

As built (C7 finding A): **partial refresh is the default for every within-session
update** — cursor moves, strikes, banners, page turns, and screen transitions alike. A
full refresh (the visible black/white strobe) happens only (1) on the session's first
draw after deep sleep, where the controller's previous-image RAM was lost, and (2) as a
ghost-clear every `UI_GHOST_CLEAR_EVERY` (12, untuned) partials. Exactly one full per
wake is the invariant; the several black flashes visible inside that one full are the
SSD1681's OTP waveform inversions, not extra refreshes. An unchanged dashboard `rev` —
or a changed `rev` whose rendered content is pixel-identical — costs zero display
activity. The display power rail is gated on only while the panel is in use.

---

## 8. Power

- **Deep sleep** whenever nothing is in progress. Wake sources: EXT1 any-low on both
  buttons, plus the RTC timer at the configured sync interval (default 10 minutes).
- **Rails.** E-paper and audio rails are gated off except during actual use (active-low
  gates). The VBAT hold pin stays latched high across deep sleep via `gpio_hold`.
- **Session limits.** UI sessions sleep after 30 seconds idle; the conversation
  follow-up window is 30 seconds; a hard cap (~90 seconds beyond legitimate recording,
  playback, or transfer time) plus the task watchdog force-sleeps any wedged session so
  a bug can never drain the battery overnight.
- **Battery.** Percent from the GPIO 4 ADC through a calibrated divider curve, sent as
  `X-Battery` on every request (§3). Below 15%: low-battery glyph, sync interval
  stretched 4×. Below 5%: sync only on button wake.

---

## 9. Error handling

The organizing fact, from protocol §8.1: **the only unrecoverable window is between
speech and the SD write** — which is why capture starts writing before anything else
initializes. Everything downstream is a retry. Every failure renders a status; nothing
fails silently.

| Fault | Behavior |
|---|---|
| No network / all joins fail | "Saved, will upload later"; capture durable; retried every wake |
| Upload interrupted, response lost | Sidecar unconfirmed → re-upload next wake; duplicate arrival is a no-op |
| Bridge reports `unknown` | Capture marked not-uploaded; re-uploaded from SD |
| `401` | Auth-error screen; no retry |
| Other `4xx` | Error status; no retry — the request will not succeed unchanged |
| `5xx` / timeout / connect failure | Exponential backoff within the session; wake schedule thereafter |
| No terminal state in 60 s | Sleep; bridge reroutes a late reply to notifications |
| SD card missing or full | Explicit full-screen error; recording refused; nothing overwritten |
| Codec, display, or SD init failure | Error screen where possible, always serial detail; never a silent wedge |
| Firmware hang | Task watchdog + awake-time cap force a clean sleep |

Serial logs are structured and keyed by capture ID — the same convention as the bridge,
so one identifier traces a recording across both codebases. Log level comes from
`/config.json`.

---

## 10. Testing

Three layers; the first is the TDD loop every implementation task runs in.

**1. Host unit tests.** `htp_client`, `app_core`, and `ui` compile as Linux binaries on
the development host with fake transport, storage, and clock. The bridge's golden
contract fixtures (`bridge/tests/fixtures/contract/*.json`) are consumed directly by
`htp_client` parser tests — the same files the bridge's own suite asserts, so the two
ends of the wire cannot drift apart. State-machine tests script the transport fake:
retry and backoff sequences, poll branching into all three terminal states, sync-cycle
ordering, notification dedup, Wi-Fi profile selection, RTC-less capture IDs, WAV header
round-trips, sidecar and index round-trips. `ui` widgets are snapshot-tested against
expected framebuffers.

**2. Host integration against the live mock.** A libcurl-backed `transport` runs
`app_core` against `bridge --mock` over real HTTP on localhost — the full client loop,
end to end, with no hardware involved. The mock already serves playable WAV bytes for
`reply.wav` (a generated silent WAV), so no bridge-side work is needed.

**3. On-target checkpoints.** Builds happen on the Linux development host; flashing and
observation happen at the operator's machine over USB, with serial output reported
back. Stage gates, in order: boot + display + buttons → record-to-SD (WAV validated on
a computer) → upload to `bridge --mock` reachable on the local network → full capture
loop against the real bridge → sync, dashboard, notifications → Recordings and
Settings → power measurement and the protocol design's §10 hardware checklist (power
pulled mid-recording, Wi-Fi killed mid-upload, SD filled, battery depleted overnight
with pending uploads, roaming between configured networks).

### Build and flash workflow

A documented setup script installs the pinned ESP-IDF release on the development host.
`idf.py build` produces the binaries and a generated flash-arguments file;
`firmware/tools/` carries the exact `esptool` one-liner and serial-monitor instructions
for flashing from any machine with USB access to the device.

---

## 11. Open items for implementation

All closed at wrap-up (Task 19):

1. **Exact IDF version pin** — **v5.5** (installed by `firmware/tools/setup-idf.sh`,
   which records any change).
2. **E-paper controller confirmation** — SSD1681 confirmed on hardware (C2). Mode-2
   partial refresh works fast and flash-free; the datasheet-derived LUT fallback was
   never needed and no LUT is uploaded.
3. **Battery curve calibration** — **provisional**: the endpoints remain the Li-ion
   nominals (3300/4200 mV in `battery.c`); no discharge measurement was taken before
   the bench wrapped. The calibration procedure is written out in `firmware/README.md`.
4. **Silence-timeout default** — MVP ships with release-to-stop only;
   `silence_timeout_s` is parsed but not acted on. Wiring it later is a small change
   in `audio_record_to`.

---

## 12. Out of scope

Unchanged from the protocol design §12: continuous conversation, multiple
personalities, camera, BLE, GPS, wake words, on-device speech recognition, haptics,
multiple dashboards, plugins, Home Assistant, radio interfaces, offline operation
beyond store-and-forward — and additionally OTA updates (§3) and any use of the SHTC3
sensor. The device uses its factory MAC address; the firmware neither overrides nor
randomizes it.
