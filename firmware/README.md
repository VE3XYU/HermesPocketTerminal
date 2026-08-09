# HTP Terminal Firmware

Firmware for the Hermes Pocket Terminal — an ESP32-S3 handheld that records voice
captures to SD, uploads them to the HTP Bridge over Wi-Fi, plays back spoken replies,
and renders a server-published dashboard on a 1.54″ e-paper panel. The device is a
deliberately thin client: it never interprets content, renders only generic primitives,
and matches on opaque IDs. The authoritative behavior spec is
[`docs/superpowers/specs/2026-08-05-htp-firmware-design.md`](../docs/superpowers/specs/2026-08-05-htp-firmware-design.md)
(reconciled with the as-built device at wrap-up); the wire protocol lives in the HTP
protocol design next to it.

**Hardware ground truth** (established on the bench): ESP32-S3-PICO-1 (LGA56), **N8R8**
— 8 MB embedded GD flash + 8 MB octal AP PSRAM. RTC is a **PCF85063** at I2C 0x51 (not
the PCF8563 the design first assumed — different register map). Display is an SSD1681
200×200 e-paper; **Mode-2 partial refresh works on this panel** — fast and flash-free;
the datasheet-derived LUT fallback was never needed and no custom LUT is uploaded.

---

## Build

One-time toolchain install on the build host (pins **ESP-IDF v5.5**; the script records
any change):

```bash
bash firmware/tools/setup-idf.sh
source ~/esp/esp-idf/export.sh        # per shell; activate.py works too
idf.py -C firmware build
```

The build emits `firmware/build/htp_terminal.bin` plus a generated flash-arguments
file. `firmware/sdkconfig` is generated and gitignored; the committed source of truth
is `sdkconfig.defaults` (target, octal PSRAM, 8 MB flash, **`CONFIG_FATFS_LFN_HEAP=y`**
— required: the SD layout uses names like `config.json.tmp` that are illegal under
FatFs's default 8.3-only naming).

## Host tests

The three pure components (`htp_client`, `app_core`, `ui`) build and test on a plain
Linux host with no ESP-IDF installed:

```bash
cmake -S firmware/tests/host -B firmware/tests/host/build
cmake --build firmware/tests/host/build
ctest --test-dir firmware/tests/host/build --output-on-failure
```

The last test (`test_mock_bridge`) runs the full client loop against `bridge --mock`
over localhost HTTP and expects the bridge venv at `bridge/.venv`; configure with
`-DHTP_INTEGRATION=OFF` to skip it. Purity check (must print nothing):

```bash
grep -rn "esp_\|freertos\|driver/" \
  firmware/components/htp_client/src firmware/components/app_core/src firmware/components/ui/src
```

## Flashing

See [`tools/FLASHING.md`](tools/FLASHING.md). `tools/pack-flash.sh` zips the three
binaries with an exact `python3 -m esptool ... write-flash` one-liner
(`FLASH-COMMAND.txt`), so any machine with USB access can flash without a toolchain.
Note: the USB-Serial-JTAG port **disappears during deep sleep** — that is the radio of
normal operation, not a dead board; wake the device (either button) or use a
reconnect-in-a-loop monitor while iterating.

---

## SD card provisioning

The card must be **FAT32**. Two files at the root configure everything; editing them
means moving the card to a computer — or using serial provisioning (below). **These
files never leave the card**: the token is never displayed, never logged in full, and
never sent anywhere except as the `Authorization` header to your own bridge.
`wifi.json` and real tokens are gitignored repo-wide — never commit either.

### `/config.json`

```json
{
  "bridge_url": "https://htp.example.net",
  "token": "<64-char-token>",
  "sync_interval_s": 600,
  "log_level": "info",
  "timezone": "EST5EDT,M3.2.0,M11.1.0"
}
```

- `bridge_url` — no trailing slash needed (one is stripped). Plain `http://` is honored
  only for development against a mock bridge on the local network.
- `token` — the bridge bearer token, verbatim.
- `sync_interval_s` — initial timer-wake interval; superseded by the `sync_interval`
  every dashboard response carries (server-controlled cadence).
- `timezone` — optional POSIX TZ string for the status clock (12-hour, no AM/PM, no
  leading zero). Absent = UTC.
- `silence_timeout_s` — parsed but not acted on in MVP (release-to-stop only).

### `/wifi.json`

```json
{
  "networks": [
    { "ssid": "<your-ssid>", "password": "<your-password>" },
    { "ssid": "<work-ssid>", "password": "<work-password>",
      "static": { "ip": "192.0.2.20", "gateway": "192.0.2.1", "netmask": "255.255.255.0" } }
  ]
}
```

Up to 8 profiles, ordered by priority. The `static` block is optional (all three fields
or nothing). The last successful BSSID+channel is cached in NVS for a ~1 s fast join on
wake. The device uses its **factory MAC address** by design (no spoofing or
randomization) — register that MAC where your network requires it.

### Serial provisioning (no card reader needed)

The firmware can (re)provision the card entirely over the USB serial console. It enters
provisioning only on one of three triggers:

1. SD mount failure,
2. missing/unparseable `/config.json`,
3. **Power held continuously for ~1.5 s at boot** (then type `YES` to confirm —
   anything else boots normally).

The flow prompts you to paste `config.json` and then `wifi.json`, each ended by a line
containing only `EOF`; every paste is parse-validated **before** anything is written,
and writes are atomic (tmp + rename). Prompts time out after 60 s.

**Format-consent contract:** firmware updates never touch the card. A format is
offered only when the card cannot be mounted (or a write fails, once per boot), and it
executes only after you type `FORMAT` at the serial prompt within 60 s. There is no
remote path to a format — or to any card write beyond the device's own recordings and
state files.

### What lives on the card

```
/config.json, /wifi.json      provisioning (above)
/rec/<capture-id>.wav         original audio, retained
/rec/<capture-id>.json        per-capture sidecar (state, transcript, conversation_id)
/rec/index                    append-only listing index (compacted, see below)
/reply.tmp.wav                transient reply download
/dash.bin                     dashboard snapshot for instant wake paint
```

**Recordings-index compaction:** the index is append-only and its reader sees only the
first 16 KB, so once the file exceeds 8 KB an append first compacts it down to the
newest 128 entries (atomic rewrite; on any failure it falls back to a plain append —
the index is never destroyed). The upload-retry scan and the pending-uploads count
cover **everything the index retains** (they page through it 32 ids at a time), so any
capture the index still lists will eventually upload — a backlog larger than one
sync's 32-upload budget drains across successive wakes, newest first. Trade-off: ids
older than what the index retains (at least the newest 128, guaranteed by compaction)
are forgotten for listing and upload-retry; the WAV and sidecar files themselves stay
on the card indefinitely. This is implemented and host-tested (`test_rec_index`: a 400-append run
that crosses the compaction threshold keeps newest-first listing exact and the on-disk
file under the cap; a 200-append run under a forced write-failure fake proves the
never-destroyed fallback) — it postdates checkpoint C7, so on-device confirmation
would need a card carrying several hundred real recordings to trigger compaction
naturally, which has not happened.

**Capture IDs** are the SD filenames and the end-to-end idempotency key. With the RTC
set they look like `c-YYYYMMDD-HHMMSS-xxxx`; if the PCF85063 has never been set (first
boot, dead battery) the fallback is `c-b<bootcount>-<monotonic-ms>-xxxx` and the upload
omits `X-Recorded-At`. The RTC is corrected from the bridge's `server_time` whenever
drift exceeds 2 s.

---

## Using the device

Deep sleep is the resting state; either button wakes it (so does the sync timer).

- **Record — hold (≥ 350 ms):** capture, from anywhere. Speak, release. Recording
  starts ~250–400 ms after the press; the ring buffer absorbs SD stalls.
- **Power — tap:** from the resting dashboard, opens the **menu**
  (Dashboard / Recordings / Settings / Sleep). In lists: cursor down. In an entry:
  next page. A tap always lands a visible change — with fewer than two positions to
  move between it climbs a level instead.
- **Record — tap:** select / act (open row, complete item, play recording, dismiss
  banner).
- **Power — long (≥ 600 ms):** back / up one level.
- **Power — hold 5 s:** power off (countdown warning at 2 s; the panel is deliberately
  blanked before the rail drops).
- During reply playback: **REC stops playback** (keep holding to answer immediately);
  in the 30 s follow-up window, **hold REC** to continue the conversation or **tap
  PWR** to end it now.

Every accepted press clicks (low blip = next, high blip = select) before the 300–500 ms
e-paper refresh runs. Defined edge behaviors (blessed at wrap-up): a PWR press made
*during* reply playback is not sampled — if it straddles the end of playback it
classifies as a tap and ends the conversation; a REC hold that stopped playback rolls
directly into the follow-up recording.

**Display behavior worth knowing:** each wake performs exactly **one** full refresh
(the teardown log prints `display: fulls=N partials=M` so you can verify). The 3–5
black flashes visible inside that one full are the SSD1681's factory (OTP) waveform
inversions — panel physics, not extra refreshes. Everything else is partial, with a
ghost-clear full every 12 partials.

---

## Battery calibration (provisional)

The voltage→percent line in
[`components/board/src/battery.c`](components/board/src/battery.c) uses **unmeasured
Li-ion nominals**: `BATT_MV_EMPTY 3300` / `BATT_MV_FULL 4200` through a nominal 2:1
divider. No discharge measurement was taken before the bench wrapped, so treat the
percentage as indicative.

To calibrate later:

1. Charge to full. Boot with the serial monitor attached and read the battery line the
   firmware logs on every session (INFO level):
   `battery: raw=... pin=...mV batt=<MV_FULL>mV pct=...`
2. Run the device down until it browns out (or as near empty as you care to define);
   on a wake near the end, read the same line: `batt=<MV_EMPTY>mV`.
3. Edit the two constants at the top of `battery.c` — `BATT_MV_FULL` to the step-1
   value, `BATT_MV_EMPTY` to the step-2 value — rebuild, reflash.

The mapping is linear between the endpoints; that is deliberate MVP scope (the
percentage drives only the header glyph and the bridge's `X-Battery` telemetry, plus
the <15% interval stretch and <5% sync skip).

---

## Reliability checklist (design §10)

Status at wrap-up. **PROVEN** rows carry the hardware evidence; **PENDING** rows have
their operator procedure written out so a future bench session can run them in one
sitting. No row below claims verification that did not happen.

| Scenario | Status | Evidence / procedure |
|---|---|---|
| Network killed mid-upload → saved → next wake delivers exactly once | **PROVEN** (C6 run 3, bridge-stop variant) | With the bridge stopped: 3 connect failures with backoff → "Saved, will upload later" → sleep. Bridge restarted: next wake logged `uploads=1` (note landed), the wake after `uploads=0` (backlog clear). Same durability chain from SD onward as an AP outage. |
| Device moved between two configured Wi-Fi networks | **PARTIAL** | Fast join proven repeatedly (~1 s, C6 run 4). Only one network was ever configured on hardware, so multi-profile *selection* is host-tested only (`test_wifi_select`). To finish: add a second profile to `wifi.json`, visit both locations, confirm the serial join line names the right SSID at each and the second visit at each uses the fast join. |
| Power removed mid-recording | **PENDING** (torn-file refusal host-tested) | Procedure: start a recording, pull power mid-sentence, reboot. Expect: the partial WAV exists under `/rec/`; because the sidecar and index are written only after a clean stop, the orphan is **not** listed or uploaded, and nothing crashes. The design's hard requirement — a torn WAV with a zeroed header must be **refused** by upload, not streamed — is implemented and host-tested (`test_capture_flow`: refusal, sidecar → `failed/bad_wav_header`, pending count drains); on-device confirmation would need a hand-planted torn file plus sidecar. |
| Bridge stopped mid-conversation | **PENDING** | Procedure: start a conversation ("hey hermes …"), stop the bridge before the reply is fetched. Expect "Still working - check later" within ~60 s, then sleep. Restart the bridge: the late reply arrives as a notification on a later sync (bridge redirect behavior). |
| SD card filled | **PENDING** | Procedure: fill the card with junk until free space is exhausted, attempt a recording. Expect the explicit SD-full screen, recording refused, nothing overwritten. |
| Battery depleted overnight with a pending upload | **PENDING** | Procedure: with the bridge unreachable, record a note ("Saved, will upload later"), let the battery run flat overnight. On charge + boot expect the capture to upload; note whether capture IDs stayed RTC-stamped (`c-YYYYMMDD-…`, RTC survived) or fell back to `c-b<bootcount>-…` (RTC lost) — both acceptable. |

---

## Configuration reference / tuning knobs

Values shipped at wrap-up. "Untuned" means the value works but was never bench-swept.

| Knob | Where | Value | Notes |
|---|---|---|---|
| `HTP_DEV_LINGER` | `main/main.c` | **0** (release) | 1 = bench mode: after a session the device stays awake with the USB console alive, buttons still act (REC = new capture, PWR tap = UI session), `sleep`/PWR-hold-2s/10-min idle reach real deep sleep. Flipped to 0 at wrap-up so sessions end in deep sleep for battery. |
| `GEST_RELEASE_DEBOUNCE_MS` | `app_core/include/gesture.h` | 60 | Release-chatter absorption; widened from 30 at wrap-up for the double-beep issue (below). |
| `UI_GHOST_CLEAR_EVERY` | `app_core/include/ui_flow.h` | 12 | Partials between ghost-clear fulls. **Cadence never received an operator verdict** — tune against observed ghost buildup, do not trust 12. |
| `AUDIO_MIC_GAIN_DB` | `board/src/audio.c` | 30 | Bench-accepted (no gain complaint at C4); reference firmware uses 45 if recordings ever come back quiet. |
| Click voices | `board/src/audio.c` | next 1 kHz/30 ms, select 2 kHz/45 ms, 3 ms attack / 8 ms decay | Envelope ends at exactly 0 (speaker-pop fix). |
| `AUDIO_OUT_VOL` | `board/src/audio.c` | 85 | esp_codec_dev volume curve, untuned. |
| Idle timeouts | `main/main.c` | UI session 30 s, follow-up window 30 s, awake caps 90/210/300 s | Awake caps bound wedges per phase, not interaction. |
| Sync interval | server-controlled | default 600 s | ×4 below 15% battery; timer syncs skipped below 5%. |
| Battery endpoints | `board/src/battery.c` | 3300/4200 mV | Provisional — see calibration above. |

Notes that would otherwise be lost:

- The REC release during a recording is sampled per 128 ms audio chunk, so
  stop-on-release can lag the finger by up to ~128 ms. Deliberate (chunked capture),
  inaudible in practice.
- The urgent-notification **chime requires codec bring-up**: in a sync-only session the
  codec is opened lazily (~300 ms) before the chime plays; if the codec fails to come
  up the chime is skipped silently.
- `esp_codec_dev` prints two spurious `i2s_channel_disable: not enabled yet` error
  lines per codec open. Library noise, not a defect.
- Idle hiss with the session-long open codec was probed for and **never reported** by
  the bench, so no PA gate was added; if hiss shows up on future hardware, gating the
  PA between sounds is the known follow-up.

---

## Known issues

- **Double beep on long-press release (parked, mitigation unverified on hardware).**
  Repro: hold PWR or REC noticeably longer than a tap, release → two blips. Root cause
  (host-reproduced in `test_gesture`): release chatter with re-strike gaps ≥ ~50 ms
  split classification into two gestures under the old 30 ms debounce. Wrap-up widened
  `GEST_RELEASE_DEBOUNCE_MS` to 60 (absorbs sampled gaps to ~70 ms) and host tests
  pin the fix, but **the bench wrapped before a hardware retest** — if the double beep
  survives, the same knob is the next lever, and the second recorded hypothesis (two
  click call sites on one release) has already been traced and cleared.
- Recordings listed before their first sync show `(not uploaded)` /`(pending)`
  previews until a sync backfills transcripts; un-synced conversation captures list
  until the sidecar learns its `conversation_id`. Cosmetic, self-heals on sync.
- A tap whose release lands inside a 300–500 ms panel refresh is unsampled and can
  classify long (buttons are not polled while the ink moves). Inherent to sampled
  input on a blocking display; accepted.

## Troubleshooting

| Symptom | Meaning / fix |
|---|---|
| "SD card error" screen | Card missing/unmountable. Insert or reseat the card; on next boot the serial console offers the gated FORMAT flow (type `FORMAT` within 60 s). |
| "Config error" screen | `/config.json` missing or unparseable — reprovision (card reader or serial paste). |
| "WiFi config error" screen | `/wifi.json` missing or unparseable. |
| "Auth error - check token" screen | Bridge returned 401. The token on the card does not match the bridge; fix `/config.json`. Never retried automatically (by design). |
| "Saved, will upload later" | No network or bridge unreachable. The capture is durable on SD; every future wake retries it first. Nothing to do but restore connectivity. |
| "Recording unreadable - not uploaded" | The WAV's header never got patched (power lost mid-recording) or was corrupted. The file is refused, marked failed, and stays on the card for manual recovery. |
| "Still working - check later" | No terminal state within the 60 s poll window. The bridge redirects the late reply to notifications; it arrives on a later sync. |
| "Too short" | Release came < ~250 ms after recording started; discarded by design. |
| "SD full" screen | Card out of space; recording refused, nothing overwritten. |
| Chipmunk / slow-motion audio | A WAV played at the wrong rate. The firmware always drives the codec from the file header's rate — if you hear this, the header is wrong (hand-made WAV?), not the clock. |
| Ghosting on the panel | Partial-refresh accumulation. A full refresh clears it — happens automatically every 12 partials and on every wake's first draw; tune `UI_GHOST_CLEAR_EVERY` if buildup is objectionable. |
| Boot pauses ~60 s at a provisioning prompt | You held PWR through boot (≥ 1.5 s) — that requests re-provisioning. **Tap** to wake, don't hold; the prompt times out into a normal boot. |
| USB serial port vanishes | Device entered deep sleep (normal). Wake it, or bench-build with `HTP_DEV_LINGER 1`. |
