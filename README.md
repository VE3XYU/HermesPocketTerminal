# Hermes Pocket Terminal

A pocket-sized, battery-powered voice terminal for a personal AI assistant. Press a button, speak, and the recording is transcribed, routed, and answered by the assistant — the device itself stays deliberately dumb: it records, uploads, displays, and plays audio. Everything intelligent happens server-side.

## What's in this repo

- [`hermes-pocket-terminal-spec.md`](hermes-pocket-terminal-spec.md) — the device specification (ESP32-S3 handheld, e-paper, multi-day battery).
- [`docs/superpowers/specs/`](docs/superpowers/specs/) — the **Hermes Terminal Protocol (HTP)** design: the wire protocol and the bridge architecture.
- [`docs/superpowers/plans/`](docs/superpowers/plans/) — the implementation plan the bridge was built from.
- `bridge/` — the **HTP Bridge**: a Python/FastAPI service implementing the protocol, with tests, a mock mode for firmware development, and deployment files ([in review — PR #1](https://github.com/VE3XYU/HermesPocketTerminal/pull/1)). Once merged, see `bridge/README.md` for setup and deployment.

## Status

The protocol design and bridge server are complete; device firmware is the next phase. The golden request/response fixtures in `bridge/tests/fixtures/contract/` are the wire reference firmware will be developed against.

## License

[CC BY-NC 4.0](LICENSE.md)
