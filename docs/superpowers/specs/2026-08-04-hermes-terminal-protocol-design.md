# Hermes Terminal Protocol (HTP) — Design

Version 0.1 — 2026-08-04

## 1. Purpose and scope

The Hermes Terminal Protocol defines how a thin terminal device talks to the Hermes
personal AI system. The Hermes Pocket Terminal (ESP32-S3 handheld) is the first client;
the protocol is written so that a watch app, a desktop widget, or a second hardware
device can speak it later without server changes.

This document covers two things:

1. **The wire protocol** — endpoints, schemas, state transitions, and error handling.
2. **The HTP Bridge** — the service that implements the protocol and connects it to
   Hermes Agent, speech-to-text, and text-to-speech.

Device firmware architecture is described only where it constrains the protocol.
Firmware implementation is a separate plan.

### Governing principle

The terminal never knows what a task, note, or reminder is. It understands only generic
primitives: display a list, display text, display status, record audio, play audio,
upload, download, sleep, wake. Every semantic decision belongs to Hermes.

---

## 2. System architecture

Three components:

```
┌──────────────────┐   HTP over HTTPS    ┌──────────────────────┐
│ Pocket Terminal  │ ──────────────────▶ │  HTP Bridge          │
│ (ESP32-S3)       │ ◀────────────────── │  (server host)       │
│                  │                     │                      │
│ • record → SD    │                     │ • HTP endpoints      │
│ • upload WAV     │                     │ • device auth        │
│ • poll status    │                     │ • cloud STT/TTS      │
│ • play audio     │                     │ • dashboard snapshot │
│ • Recordings menu│                     │ • notification queue │
└──────────────────┘                     └──────┬───────▲───────┘
                                     text via   │       │  MCP tools:
                                     API server │       │  publish_dashboard,
                                     mode       ▼       │  queue_notification
                                         ┌─────────────┴────────┐
                                         │ Hermes Agent          │
                                         │ (brain, task memory,  │
                                         │  skills, routing)     │
                                         └──────────────────────┘
```

**Device → Bridge:** HTP only. All communication is device-initiated; there is no push.
Small JSON plus raw audio bytes.

**Bridge → Hermes Agent:** transcribed text in, reply text out, over Hermes Agent's local
API server mode.

**Hermes Agent → Bridge:** structured state — dashboard items with stable IDs, queued
notifications — pushed through MCP tools that the bridge exposes.

**Bridge → cloud:** speech-to-text and text-to-speech API calls. Generated audio is
cached on the bridge for the device to download.

The bridge performs no interpretation of content and makes no LLM calls of its own. It
moves audio and text, translates between protocols, and holds materialized state.

### Why a bridge

Hermes Agent provides messaging channels, an OpenAI-compatible API server mode, an
inbound webhook channel intended for service events, and an experimental relay connector
system. None of these provide what the terminal needs: idempotent audio upload,
transcription, TTS audio delivery, a task list with stable IDs, or a pollable
notification queue.

Pointing the device directly at Hermes Agent would push all of that compensation into
firmware, which contradicts the governing principle. The bridge absorbs it instead, and
HTP stays a stable contract the device can depend on. If Hermes Agent's API changes or is
replaced, firmware does not change.

If the Hermes Relay connector system matures, the bridge can adopt it internally as its
path to the agent. HTP is unaffected by that choice.

---

## 3. Protocol conventions

### Transport and reachability

HTTPS only, to a public hostname. A reverse proxy (Caddy recommended, for automatic
Let's Encrypt certificates) terminates TLS and forwards only `/htp/v1/*` to the bridge.
Let's Encrypt roots ship in ESP32 CA bundles, so device TLS works without custom trust
configuration, and the device works on any Wi-Fi network.

Hermes Agent is never exposed to the internet. Only HTP is.

**Rejected: ZeroTier and other overlay networks.** ZeroTier has no official ESP32 client;
only an experimental community port of `libzt` exists. Beyond maturity, an overlay
requires re-establishing a peer session after every deep-sleep wake, adding seconds of
radio-on time to every capture. That conflicts with both the 250 ms capture target and
multi-day battery life. An overlay network may still be used for operator access to the
server; that is unrelated to HTP. Because HTP is ordinary HTTPS, a future ESP32 overlay
client could be placed underneath it without protocol changes.

### Authentication

Each device holds a long random bearer token, provisioned once via firmware config or a
file on the SD card, and sends it on every request:

```
Authorization: Bearer <token>
```

The bridge keeps a token-to-device registry. Tokens are compared in constant time.
Revocation is removing a registry entry. There is no OAuth, no token refresh, and no
session state — nothing a battery-powered microcontroller must maintain.

### Versioning

All endpoints live under `/htp/v1/`. Breaking changes introduce `/htp/v2/`. Additive
fields may appear at any time.

**Clients MUST ignore unrecognized JSON fields.** This single rule is what allows the
server to evolve without firmware updates.

### Encoding

- Requests and responses are JSON, kept flat and small so a microcontroller can parse
  them into fixed-size buffers.
- Audio is transferred as raw bytes with an appropriate `Content-Type`, never
  base64-encoded inside JSON.
- Upload format: 16 kHz, 16-bit, mono WAV — what the device codec produces natively, so
  the device performs no encoding.
- Download format: WAV, same parameters. The device plays PCM directly. Compressed
  formats are a future option.
- Timestamps are integer Unix epoch seconds, UTC.

### Idempotency

An idempotent operation produces the same result whether performed once or many times.

Every recording carries a device-generated **capture ID** (timestamp plus random suffix,
e.g. `c-20260804-101502-3fa9`). Re-uploading the same capture ID does not create a second
capture; the bridge recognizes the ID and returns the current state of the existing one.

This matters because the device often cannot tell whether an upload succeeded — Wi-Fi
drops mid-request, or the reply is lost. Idempotency lets the device retry blindly and
forever without risk of delivering the same recording to Hermes twice.

The same capture ID is used as the SD card filename, so the device's local archive and
the server's records share one identifier end to end.

### Additional headers

Every request includes:

```
X-Battery: 78        # integer percent, 0-100
```

The bridge records it as device telemetry. The firmware does nothing else with it. This
lets Hermes answer questions about the terminal's charge and warn when it is low.

---

## 4. Endpoint catalog

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/htp/v1/captures` | Upload a recording |
| `GET`  | `/htp/v1/captures?ids=a,b,c` | Batch status poll |
| `GET`  | `/htp/v1/captures/{id}/reply.wav` | Download reply audio |
| `GET`  | `/htp/v1/dashboard` | Fetch the materialized list |
| `POST` | `/htp/v1/complete` | Mark a dashboard item complete |
| `GET`  | `/htp/v1/notifications` | Fetch pending notifications |
| `POST` | `/htp/v1/notifications/ack` | Confirm notifications were displayed |

All exchanges are short-lived request/response. There are no WebSockets and no
long-lived connections; nothing fights deep sleep.

### Design notes

**Batch status polling.** One request covers any number of outstanding captures. A radio
round-trip costs the same battery whether it asks about one capture or twenty, and this
is the mechanism that backfills transcripts into the device's Recordings menu.

**Dashboard revisions.** The dashboard response carries a revision tag. The device sends
the revision it last drew; if nothing has changed, the bridge returns a minimal
"unchanged" response and the device skips the e-paper refresh entirely.

**Notification acknowledgement.** Delivery is not complete until the device has actually
displayed the notification. Fetch-then-acknowledge means a device that dies mid-render
simply receives the notification again on its next poll.

**Server time.** Dashboard and notification responses include `server_time`, keeping the
device RTC synchronized without an NTP client.

---

## 5. Schemas

### 5.1 Capture upload

```
POST /htp/v1/captures
Authorization: Bearer <token>
Content-Type: audio/wav
X-Capture-Id: c-20260804-101502-3fa9
X-Capture-Mode: auto            # auto | note | converse
X-Recorded-At: 1754300102
X-Conversation-Id: v-4b81       # optional; present only on follow-ups
X-Battery: 78

<WAV bytes>
```

Response (immediate, before any processing):

```json
{ "id": "c-20260804-101502-3fa9", "state": "received" }
```

The device may display "Uploaded" and sleep at this point. Transcription and agent
processing continue without it.

`X-Capture-Mode` is `auto` in normal operation — the bridge decides disposition by
salutation detection (§6.2). The explicit `note` and `converse` values are reserved for
future clients that need to force behavior.

`X-Conversation-Id` is omitted on a first capture. When a capture is routed to
conversation, the bridge returns a `conversation_id` in that capture's status; the device
echoes it on the next capture to continue the same exchange. The device treats it as an
opaque string and discards it when the conversation ends.

Maximum recording length is 120 seconds (approximately 3.8 MB), enforced on both device
and bridge.

### 5.2 Capture states

```
received → transcribing ─┬─→ done                            (note disposition)
                         └─→ processing → reply_ready        (conversation disposition)

any state → failed
```

Note disposition reaches `done` as soon as transcription completes, without entering
`processing` — the device does not wait for agent ingestion.

| State | Meaning |
|-------|---------|
| `received` | WAV stored on the bridge; not yet transcribed |
| `transcribing` | Speech-to-text in progress |
| `processing` | Transcribed; Hermes Agent is handling it |
| `done` | Complete; no spoken reply is coming |
| `reply_ready` | Reply audio available for download |
| `failed` | Processing failed; see `error` |
| `unknown` | The bridge has no record of this capture ID |

### 5.3 Batch status poll

```
GET /htp/v1/captures?ids=c-...-3fa9,c-...-a1b2,c-...-ffff
```

```json
{
  "server_time": 1754300102,
  "captures": [
    { "id": "c-...-3fa9", "state": "done",
      "transcript": "Add milk to the shopping list" },
    { "id": "c-...-a1b2", "state": "reply_ready",
      "transcript": "Hey Hermes, what's on my calendar today?",
      "conversation_id": "v-4b81" },
    { "id": "c-...-ffff", "state": "unknown" }
  ]
}
```

`transcript` appears as soon as transcription completes, in any subsequent state. This is
what the device backfills into its Recordings menu.

`unknown` means the bridge never received that capture — for example, the device
uploaded moments before a crash and the request never completed. The device marks such
captures as not uploaded and retries from the SD card.

A `failed` capture includes a reason:

```json
{ "id": "c-...-7cd", "state": "failed", "error": "transcription_failed" }
```

### 5.4 Reply audio

```
GET /htp/v1/captures/{id}/reply.wav
```

Returns `audio/wav` bytes, or `404` if the capture has no reply.

### 5.5 Dashboard

```
GET /htp/v1/dashboard?rev=7c1a
```

Changed:

```json
{
  "rev": "8d2b",
  "server_time": 1754300102,
  "title": "Today",
  "items": [
    { "id": "t-9f2", "text": "Buy milk", "done": false },
    { "id": "t-c41", "text": "Call dentist", "done": true, "style": "dim" }
  ]
}
```

Unchanged:

```json
{ "rev": "7c1a", "unchanged": true, "server_time": 1754300102 }
```

- `id` is a stable, opaque Hermes identifier. The device never parses or matches on item
  text.
- `style` is an optional generic rendering hint (`bold`, `dim`). Unknown values render as
  normal text, so Hermes may introduce new styles without firmware updates.
- The bridge caps the list at 32 items and truncates `text` server-side to what the
  display can show. The device never performs layout calculation on unbounded input.

### 5.6 Complete

```
POST /htp/v1/complete
{ "item_id": "t-9f2" }
```

```json
{ "ok": true, "rev": "8d2c" }
```

The bridge immediately marks the item complete in its snapshot, so the next dashboard
fetch reflects it — the user sees instant feedback. The bridge then informs Hermes Agent,
which updates its memory and republishes. If the agent's republished state disagrees, the
agent wins: the snapshot is a cache, the agent is truth.

Completing an already-complete item is not an error.

### 5.7 Notifications

```
GET /htp/v1/notifications
```

```json
{
  "server_time": 1754300102,
  "notifications": [
    { "id": "n-118",
      "text": "Meeting with Alex at 10:00 AM",
      "priority": "urgent",
      "created": 1754299500 }
  ]
}
```

`priority` is `urgent` (chime and display) or `normal` (display only). Unknown priorities
are treated as `normal`.

**Authoring rule: notification text must use absolute time references.** Delivery is
delayed by up to one full poll interval, so a notification reading "in 15 minutes" is
wrong by the time it is displayed. This rule is enforced through the instructions the
bridge gives Hermes Agent alongside the `queue_notification` tool.

```
POST /htp/v1/notifications/ack
{ "ids": ["n-118"] }
```

Notifications are delivered at least once. Unacknowledged notifications are redelivered
on the next poll; the device deduplicates by ID.

### 5.8 Errors

```json
{ "error": "unauthorized" }
```

Returned with the matching HTTP status. Client retry rules:

| Status | Device behavior |
|--------|-----------------|
| `401` | Display an authentication error; do not retry |
| Other `4xx` | Do not retry — the request will not succeed unchanged |
| `5xx`, timeout, connection failure | Retry with exponential backoff |

---

## 6. HTP Bridge

One service (Python with FastAPI recommended, for speech API SDK coverage and MCP
support; final stack decision belongs to the implementation plan), backed by SQLite and a
directory of audio files.

### 6.1 Components

**HTP API layer.** Implements the seven endpoints. Validates bearer tokens, reads and
writes the state store, and never blocks on slow work — an upload returns as soon as the
WAV is written to disk.

**Capture pipeline.** An asynchronous worker; the only non-trivial machinery in the
system.

```
WAV stored → speech-to-text → salutation check
                                  ├─ absent  → state: done
                                  │             → text to Hermes Agent (ingestion)
                                  └─ present → strip salutation
                                                → text to Hermes Agent → reply text
                                                → text-to-speech → store reply.wav
                                                → state: reply_ready
```

Each stage updates the capture's state row, which is exactly what the device's status
poll reads. Any stage failure sets `failed` with a reason. No stage may hang
indefinitely; every external call has a timeout.

**Agent client.** Talks to Hermes Agent's API server on localhost.

**MCP server.** Exposes tools that Hermes Agent calls to publish state (§6.4).

**State store and audio cache.** SQLite in WAL mode with tables `devices`, `captures`,
`dashboard`, `notifications`. Uploaded WAVs are retained (retention configurable) as a
server-side archive. Generated reply audio is pruned after a few days; it is disposable.

### 6.2 Salutation detection

The device has no wake-word capability and needs none. Recording is always
button-initiated. Disposition is decided by the bridge from the **transcript prefix**:

- The bridge holds a configurable list of salutations, e.g. `["hey hermes", "hermes"]`.
- Matching is case- and punctuation-insensitive, because speech-to-text output varies:
  "Hey Hermes," / "hey hermes." / "Hey, Hermes!" must all match.
- **Salutation present** → conversation. The salutation is stripped and the remainder
  sent to Hermes Agent; the reply is synthesized and offered as `reply_ready`.
- **Salutation absent** → note. State becomes `done` immediately after transcription, so
  the device can confirm and sleep within seconds. The text is still forwarded to Hermes
  Agent for ingestion, which happens after the device is already asleep.

The salutation list is bridge configuration. Changing the assistant's name does not
require a firmware update.

If a plain note later warrants a response, Hermes Agent can raise it through
`queue_notification`; it arrives on the next sync rather than as a spoken reply.

### 6.3 Agent integration

Every message to Hermes Agent is wrapped in pocket-terminal context instructing it to:

- Reply in one or two short plain-text sentences suitable for text-to-speech.
- Use no markdown, no lists, and no preamble.
- Use absolute time references, never relative ones.

If Hermes Agent's API accepts a model parameter, pocket-terminal traffic is pinned to a
fast model for low-latency, to-the-point answers. The device never requests a model and
has no knowledge that models exist.

For conversation continuity, the bridge assigns a conversation ID when a capture is
routed to conversation and returns it in that capture's status. A follow-up capture
carrying that ID is sent to the agent with the recent exchange history included. Whether
the agent's API offers native session handling is verified during implementation; the
history-window approach works either way.

A reply that completes after the device has stopped polling is enqueued through the same
notification path, so the answer reaches the user on the next sync instead of being
discarded.

### 6.4 MCP tools

The bridge exposes an MCP server that Hermes Agent connects to:

| Tool | Purpose |
|------|---------|
| `publish_dashboard(title, items)` | Replace the dashboard snapshot with structured items carrying stable IDs |
| `queue_notification(text, priority)` | Enqueue a notification for the next device poll |
| `get_device_status()` | Return battery level and last-seen time per device |

Hermes Agent's memory is the source of truth for tasks; the bridge holds a materialized
snapshot so that device polls are answered instantly without waking the LLM.

Implementation requires one agent-side step: instructing Hermes Agent, through its skill
or system-prompt mechanism, to call `publish_dashboard` whenever its task memory changes.
A scheduled daily nudge re-synchronizes the snapshot if the agent ever omits a
republish.

### 6.5 Speech services

Speech-to-text and text-to-speech use cloud APIs, selected in configuration. The pipeline
treats them as replaceable: each is a single call with a timeout and retry policy, so
changing providers is a configuration change, not a redesign.

### 6.6 Configuration and deployment

A single configuration file holds: device tokens, salutation list, speech provider
selection and API keys, Hermes Agent API URL, model hint, audio retention policy, and the
default sync interval served to devices.

Deployment is a systemd unit on the Linux host running Hermes Agent, with Caddy in front
terminating TLS on the public hostname and forwarding only `/htp/v1/*`.

---

## 7. Device behavior

Firmware detail is specified only where it constrains or explains the protocol.

### 7.1 Sleep and wake

Deep sleep is the default state. The device wakes for three reasons:

1. **Record button** — EXT wake on GPIO 0, the hardware fast path that makes the 250 ms
   capture target achievable.
2. **Power/menu button** — user interface.
3. **RTC timer** — scheduled sync, default every 10 minutes, configurable.

### 7.2 Capture

One gesture, always: press and hold record, speak, release.

On press, the device wakes, begins writing the WAV to the SD card immediately, and
displays a recording indicator. On release it stops, then uploads, displays the result,
runs a sync cycle, and returns to sleep. Typical awake time is a few seconds.

If the network is unreachable, the recording is already safe on the SD card. The device
displays "Saved, will upload later" and sleeps. Every subsequent wake retries pending
uploads first; idempotency makes those retries safe.

Because disposition is decided server-side, the device does not know at upload time
whether a reply is coming. It therefore always polls capture status after upload, about
once per second, and branches on the result:

- `done` — display the confirmation and sleep. This is the common case and typically
  resolves within a few seconds, since notes reach `done` directly from transcription.
- `reply_ready` — download and play the reply, then accept a follow-up (hold to talk
  again, echoing the returned conversation ID) and sleep after roughly 30 seconds of
  inactivity.
- `failed` — display the error and sleep; the recording remains on the SD card.

If neither terminal state is reached within 60 seconds, the device sleeps. Any reply that
arrives afterward is redirected by the bridge to the notification queue rather than lost.

### 7.3 Scheduled sync

A timer wake is silent and performs, in order:

1. Retry pending uploads.
2. Fetch notifications; chime only if an urgent one is present.
3. Fetch the dashboard with the last known revision; redraw only if changed.
4. Backfill missing transcripts via batch status poll.
5. Acknowledge displayed notifications.
6. Sleep.

If nothing has changed, the display does not refresh and the wake costs roughly two to
three seconds of radio time.

### 7.4 Local recording archive

Every recording is retained on the SD card for later verification, accompanied by its
transcript.

```
/rec/<capture-id>.wav     original audio
/rec/<capture-id>.json    sidecar: state, transcript, timestamps, upload status
/rec/index                append-only index for fast menu listing
```

The sidecar is what sync backfills as transcripts become available.

A **Recordings** menu lists entries newest-first, showing time and either the transcript's
opening words, "(transcript pending)", or "(not uploaded)". Opening an entry shows the
full scrollable transcript and offers playback of the original WAV through the speaker.

A full SD card refuses new recordings with an explicit error rather than silently
overwriting.

### 7.5 Wi-Fi profiles

The device stores multiple network profiles in `wifi.json` on the SD card — an ordered
list, each with SSID, password, and optional static IP configuration. Editing requires no
reflash. Credentials never leave the device and never appear in HTP.

**Connection strategy.** Scanning is expensive radio time, so the device caches the BSSID
and channel of its last successful connection and attempts that directly on wake. A
targeted join on a known channel is roughly a second faster than scan-and-select, on
every wake. On failure it falls back to a full scan and selects the highest-priority
profile present. The common case — same network as last time — is fast; changing
locations costs one slow wake.

**MAC address.** The device uses its own factory MAC on every network; the firmware
neither overrides nor randomizes it. The Settings screen displays it, since some networks
require the address to be known in advance.

**Protocol impact: none.** Device identity is the bearer token, not the network or MAC
address. The terminal is the same device on every network.

---

## 8. Reliability

### 8.1 The durability chain

A capture exists in four places in sequence: SD card, bridge disk, transcript in SQLite,
Hermes Agent memory. Every hop is idempotent and retried until acknowledged, so any link
may fail without data loss.

**The only unrecoverable window in the system is the moment between speech and the SD
write.** Everything after it converges.

| Failure | Behavior |
|---------|----------|
| Network unreachable | Capture stays on SD marked not-uploaded; retried on every future wake, indefinitely |
| Power loss or crash mid-upload | WAV is on SD; sidecar shows unconfirmed; re-uploaded on next boot; capture ID makes a duplicate arrival a no-op |
| Device believes it uploaded but bridge has no record | Batch poll returns `unknown`; device re-uploads from SD |
| Speech API outage | Captures queue in `transcribing`; retried with exponential backoff; transcript backfills later |
| Hermes Agent down or hung | Notes queue for ingestion and retry. Conversations time out to `failed`; device reports "Hermes didn't answer — saved as note" |
| Bridge crash or VM reboot | systemd restarts; SQLite WAL means each capture resumes from its last recorded state |
| SD card full | New recordings refused with an explicit error; nothing is overwritten |
| Clock drift | Re-synchronized from `server_time` on every response |
| Dashboard snapshot drifts from agent memory | Daily scheduled republish reconverges it |
| Device offline for over 24 hours | Bridge flags it; Hermes can report the terminal is unreachable on another channel |

### 8.2 Delivery guarantees

- **Captures:** at-least-once upload, exactly-once ingestion (enforced by capture ID).
- **Notifications:** at-least-once display; the device deduplicates by notification ID.
- **Completions:** idempotent; repeats are harmless.
- **Dashboard:** eventually consistent with agent memory, reconverged by republish.

### 8.3 Observability

Structured logs are keyed by capture ID, so a single ID traces one recording's entire
life from upload through ingestion. A `/healthz` endpoint reports pipeline backlog,
per-stage failure counts, and last-seen time per device.

---

## 9. Security

The exposed surface is deliberately small.

- **One path prefix** (`/htp/v1/*`) is reachable from the internet. Hermes Agent is not
  exposed.
- **TLS only**, via the reverse proxy with automatically renewed certificates.
- **Per-device bearer tokens**, long and random, compared in constant time, individually
  revocable.
- **Hard request size caps** (audio upload limit enforced at proxy and bridge) and rate
  limiting at the proxy.
- **Cloud API keys** exist only in bridge configuration; the device never holds them.
- **Wi-Fi credentials** never traverse HTP.

Worst case if a device token leaks: the holder can read the dashboard and submit audio
for processing. There is no command surface, no shell access, and no path to Hermes Agent
itself. Revocation is immediate.

Voice audio is sent to a third-party speech provider for transcription and synthesis.
This is an accepted trade-off for response quality; the pipeline's provider abstraction
leaves local models available as a future change.

---

## 10. Testing strategy

**Bridge unit and integration tests.** Each pipeline stage is tested with mocked speech
and agent services. A stub OpenAI-compatible agent server supports integration runs
without the real Hermes Agent.

**Reliability tests** assert the guarantees in §8 rather than merely happy paths:

- Kill the process mid-pipeline at each stage; assert the capture resumes correctly.
- Upload the same capture ID five times; assert exactly one ingestion.
- Drop acknowledgements; assert notifications redeliver and the device-side dedupe holds.
- Fail each external service in turn; assert no capture is lost and no stage hangs.

**Contract fixtures.** Golden HTP request and response pairs test the bridge from the
outside and later serve as the reference for firmware development. The bridge provides a
`--mock` mode serving canned responses, so firmware work needs neither cloud services nor
a running agent.

**End-to-end checklist**, executed against real hardware before the system is considered
done. Each scenario has a defined expected outcome drawn from §8.1:

- Power removed mid-recording.
- Wi-Fi disabled mid-upload.
- Hermes Agent stopped mid-conversation.
- SD card filled to capacity.
- Battery fully depleted overnight with pending uploads.
- Device moved between all configured Wi-Fi networks.

---

## 11. Open items for implementation

These are verified against the live system during implementation. None affect the
protocol design.

1. **Hermes Agent API server specifics** — exact endpoint shape, whether it accepts a
   model parameter, and whether it offers native conversation sessions.
2. **Hermes Agent MCP client configuration** — how the running instance is pointed at the
   bridge's MCP server, and the mechanism for instructing it to republish the dashboard.
3. **Speech provider selection** — specific STT and TTS vendors and voices.
4. **Public hostname and certificate** — domain, DNS, and proxy configuration.

---

## 12. Out of scope

Deferred, consistent with the device specification: continuous conversation, multiple
personalities, camera, BLE, GPS, local wake-word detection, on-device speech recognition,
haptic feedback, multiple dashboards, plugins, Home Assistant integration, radio
interfaces, and offline operation beyond the store-and-forward behavior described in §8.
