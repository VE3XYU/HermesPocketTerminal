# Hermes Pocket Terminal

## Technical Specification

Version 0.1

## 1. Purpose

The Hermes Pocket Terminal is a dedicated handheld device that provides a low-friction interface to the Hermes personal AI system.

The device is not intended to function as an independent assistant. Its primary responsibilities are:

- Capture voice input
- Display Hermes information
- Play Hermes audio responses
- Operate with very low power consumption
- Remain responsive enough to encourage constant use

Hermes remains responsible for all intelligence, storage, classification and decision making.

---

# 2. Design Goals

Priority order:

1. Instant capture
2. Simplicity
3. Reliability
4. Low power
5. Small size
6. Extensibility

The firmware should remain intentionally minimal.

---

# 3. Functional Requirements

## FR-001 Voice Capture

The device shall begin recording within approximately 250 ms of the user pressing the Record button.

Recording continues until:

- button released
- silence timeout
- maximum recording length reached

Recorded audio is uploaded immediately.

---

## FR-002 Upload

Recorded audio shall be uploaded to Hermes using HTTPS.

The device shall retry failed uploads.

Uploads shall be idempotent.

---

## FR-003 Confirmation

The device shall display upload status.

States include:

- Recording
- Uploading
- Uploaded
- Failed
- Offline

---

## FR-004 Task Display

The device shall display a list provided by Hermes.

The firmware does not interpret list contents.

Hermes determines:

- ordering
- formatting
- priority
- completion state

---

## FR-005 Task Completion

The device shall allow a list item to be marked complete.

The request shall include a unique Hermes item identifier.

No text matching shall occur on the device.

---

## FR-006 Conversation Mode

The device shall support interactive requests.

Workflow:

```
record

↓

upload

↓

Hermes processes

↓

Hermes returns TTS

↓

device downloads

↓

play audio
```

---

## FR-007 Notification Queue

The device shall periodically poll Hermes for pending notifications.

Notification polling interval shall be configurable.

Default:

```
10 minutes
```

---

## FR-008 Audio Playback

Hermes shall return pre-generated audio.

The device performs no TTS generation.

---

# 4. Non-Functional Requirements

## Startup

Wake from sleep:

< 500 ms

---

## Battery Life

Target:

Multiple days under normal usage.

Deep sleep whenever idle.

---

## Network

Wi-Fi only (MVP)

Future:

BLE

LTE

---

## Display

Monochrome e-paper.

Display updates should be minimized.

---

# 5. System Responsibilities

## Device

Responsible for:

- microphone
- speaker
- buttons
- display
- Wi-Fi
- uploads
- downloads
- power management
- local cache

Not responsible for:

- transcription
- AI
- note organization
- task management
- reminders
- calendar logic
- model selection

---

## Hermes

Responsible for:

- authentication
- transcription
- LLM routing
- task extraction
- reminder generation
- scheduling
- notification queue
- storage
- search
- conversation memory

---

# 6. Communication Protocol

All communication is initiated by the device.

Endpoints might resemble:

```
POST /voice

GET /notifications

GET /dashboard

POST /complete

GET /conversation/{id}
```

Responses should be JSON except for streamed audio.

---

# 7. Firmware Principles

The firmware should never know what a "task", "note", or "reminder" is.

It only understands generic UI primitives:

```
Display List

Display Text

Display Status

Play Audio

Record Audio

Upload File

Download File

Sleep

Wake
```

Everything else is defined by Hermes.

---

# 8. AI Routing

Model selection is exclusively performed by Hermes.

Example routing table:

| Intent | Model |
|---------|-------|
| Transcription | Whisper |
| Simple query | Fast model |
| Knowledge lookup | Fast model |
| Coding | Codex |
| Deep reasoning | Reasoning model |
| Creative writing | Claude |

The handheld never requests a specific model.

---

# 9. MVP Scope

Version 1 should include only:

- Record button
- Voice upload
- Cloud speech-to-text transcription
- Hermes ingestion
- E-paper status
- Display task list
- Mark task complete
- Notification polling
- Audio playback

Nothing more.

---

# 10. Future Features

Deferred until after MVP:

- Continuous conversation
- Multiple personalities
- Camera
- BLE
- GPS
- Local wake-word detection
- Local speech recognition
- Haptic feedback
- Multiple dashboards
- Plugins
- Home Assistant integration
- Radio interfaces
- Offline mode

---

One architectural change I'd make is to avoid thinking of this as a "voice recorder" project. Instead, think of it as **Hermes Terminal Protocol (HTP)** plus a hardware implementation. If you define the protocol first—the messages, commands, and state transitions—the ESP32 becomes just the first client. Later, you could build an Apple Watch app, a desktop widget, or even another hardware device that speaks the same protocol. That's the sort of separation that tends to pay dividends as a project grows.
