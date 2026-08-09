# Hermes Pocket Terminal

A pocket-sized, battery-powered voice terminal for a personal AI assistant. Press a button, speak, and the recording is transcribed, routed, and answered by the assistant — the device itself stays deliberately dumb: it records, uploads, displays, and plays audio. Everything intelligent happens server-side.

## What's in this repo

- [`hermes-pocket-terminal-spec.md`](hermes-pocket-terminal-spec.md) — the device specification (ESP32-S3 handheld, e-paper, multi-day battery).
- [`docs/superpowers/specs/`](docs/superpowers/specs/) — the **Hermes Terminal Protocol (HTP)** design (wire protocol + bridge architecture) and the **firmware design** (device architecture, runtime model, UI).
- [`docs/superpowers/plans/`](docs/superpowers/plans/) — the implementation plans the bridge and the firmware were built from.
- `bridge/` — the **HTP Bridge**: a Python/FastAPI service implementing the protocol, with tests, a mock mode for firmware development, and deployment files. See `bridge/README.md` for setup and deployment.
- `firmware/` — the **device firmware**: pure ESP-IDF (no Arduino layer), three pure-C host-tested components behind injected ports, plus the board support layer. See `firmware/README.md` for build, flashing, and SD provisioning.

## Status

Protocol, bridge, and device firmware are all complete; the firmware is bench-verified end to end on real hardware (capture → upload → transcription → dashboard/notifications → spoken conversation replies). The golden request/response fixtures in `bridge/tests/fixtures/contract/` remain the wire reference both ends are tested against.

## License

[CC BY-NC 4.0](LICENSE.md)
