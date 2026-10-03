# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

The **Hermes Pocket Terminal** — an ESP32-S3 handheld that acts as a thin client to the Hermes personal AI system — and the **HTP Bridge**, the server that connects it to Hermes Agent. Both are built: the bridge lives under `bridge/` (Python/FastAPI, with a `--mock` mode), the device firmware under `firmware/` (pure ESP-IDF, with three pure-C host-tested components). Each was executed from its TDD implementation plan; the firmware is hardware-verified through bench checkpoints C1–C7.

The documents, in dependency order:

1. `hermes-pocket-terminal-spec.md` — device spec: what the terminal does and deliberately does not do.
2. `docs/superpowers/specs/2026-08-04-hermes-terminal-protocol-design.md` — the authoritative protocol design: the HTP wire protocol (endpoints, schemas, capture states, idempotency) and the bridge architecture. Bare section references (§4, §8…) point into this file.
3. `docs/superpowers/specs/2026-08-05-htp-firmware-design.md` — the firmware design (architecture, runtime model, UI, power), reconciled with the as-built device at wrap-up.
4. `docs/superpowers/plans/` — the executed implementation plans (checkbox tracking, one commit per task). Later features follow the same pattern: a dated design in `specs/` plus a plan in `plans/` (e.g. bridge pipeline timing).

`firmware/README.md` is the operator manual: build, flashing, SD provisioning, calibration, tuning knobs, and the reliability-checklist status. `bridge/README.md` covers config, deployment, and the `htp-timings` readout.

`reference/pala_note/` is third-party firmware kept locally for hardware reference only (pin maps, init order). It is gitignored; never commit it, redistribute it, or copy code from it.

## Architecture

Three components: **device → bridge** speaks HTP (device-initiated HTTPS only, JSON + raw WAV, no push, no WebSockets); **bridge → Hermes Agent** sends transcribed text via the agent's OpenAI-compatible API; **Hermes Agent → bridge** publishes dashboard/notification state through MCP tools the bridge exposes at `/mcp`.

Two invariants govern every design decision:

- **The terminal never knows what a task, note, or reminder is.** It renders generic primitives (list, text, status, audio) and matches on opaque IDs, never text.
- **The bridge never interprets content and makes no LLM calls.** It routes solely on a configured salutation prefix in the transcript ("hey hermes" → conversation with TTS reply; absent → note, `done` immediately after transcription).

The capture ID (device-generated, also the SD-card filename) is the end-to-end idempotency key: re-uploads are no-ops, retries are always safe. Reliability guarantees are enumerated in design §8 — `bridge/tests/test_reliability.py` asserts those behaviors, not just happy paths.

**Contract fixtures** (`bridge/tests/fixtures/contract/*.json`) are the golden HTP responses both ends test against: the bridge's `tests/test_contract.py` checks them, and the firmware host tests load the same files (`FIXDIR` in `firmware/tests/host/CMakeLists.txt`). A failure means the wire format changed — update the fixture deliberately, as a protocol change, and re-run both suites. A missing fixture is written on first run and the test skips; re-run to verify.

### Bridge internals (`bridge/src/htp_bridge/`)

- `main.build_deps()` is the single wiring point: it builds the stores, `Pipeline`, and `AgentClient` into a `Deps` dataclass for `api.create_app()`, and `create_full_app()` mounts the MCP server (`mcp_server.HermesTools`) at `/mcp`.
- `pipeline.Pipeline` does the per-capture work: transcribe → `salutation.detect` → note ingest, or conversation + TTS. Uploads schedule it as a background task (`api._run_pipeline`). `Pipeline.resume()` must finish **before** the app serves requests, or replies can be double-delivered. A background loop retries redirects and ingestion every 60 s and prunes audio hourly.
- Notes write their terminal state (`done`, with an `ingest_failed` flag) *before* calling `agent.ingest`. That write-ahead keeps interrupted ingests visible to `sweep_ingestion()`; don't reorder it, and never blindly overwrite a capture's state on error.
- `mock.py` (`htp-bridge --mock`) answers every HTP endpoint with canned data and no DB, speech, or agent; it accepts any token. It mirrors the real request validation, so update it when endpoint validation changes.
- Speech and agent clients have `Fake*` counterparts; `tests/conftest.py` wires them and a `FakeClock` into a real `Deps`.

## Bridge development

```bash
cd bridge
pip install -e ".[dev]"            # Python 3.11+ required (stdlib tomllib)
python -m pytest tests/ -v         # full suite
python -m pytest tests/test_captures.py -v        # one file
python -m pytest tests/ -k test_name -v           # one test
htp-bridge --mock                  # canned HTP server for firmware work
htp-bridge --config config.toml    # real run; binds 127.0.0.1:8787
htp-timings --config config.toml   # per-stage latency summary
```

## Firmware development

```bash
bash firmware/tools/setup-idf.sh                  # one-time; pins ESP-IDF v5.5
source ~/esp/esp-idf/export.sh
idf.py -C firmware build                          # device build

# host tests: plain cmake + gcc, no ESP-IDF needed
cmake -S firmware/tests/host -B firmware/tests/host/build
cmake --build firmware/tests/host/build
ctest --test-dir firmware/tests/host/build --output-on-failure

bash firmware/tools/run-mock.sh [port]            # bridge --mock on :18787 for integration tests
```

`run-mock.sh` expects the bridge installed in `bridge/.venv`.

Firmware's binding rules: `components/htp_client`, `components/app_core`, and `components/ui` are pure C — no ESP-IDF includes, no direct OS/hardware calls; everything arrives through injected port structs. Verify before any commit touching them (must print nothing): `grep -rn "esp_\|freertos\|driver/" firmware/components/htp_client/src firmware/components/app_core/src firmware/components/ui/src`. All host tests green before every commit. Fixed-size buffers; heap only inside cJSON and transport bodies. Changes that touch hardware paths need the operator to flash and report serial output — don't claim them verified from host tests alone.

## Constraints most often violated by default habits

The plans' **Global Constraints** sections bind all code. The ones habits break most:

- Time, IDs, and randomness are injected (`clock: Callable[[], int]` in the bridge, port structs in firmware), never called inline — no `time.time()`, `datetime.now()`, `uuid4()`, `time()`, or `rand()` in business logic.
- Tests never touch the network; speech/agent clients are faked, provider tests mock the HTTP transport.
- Timestamps are integer Unix epoch seconds everywhere — never floats or ISO strings.
- Errors are `{"error": "<snake_case_slug>"}` with the matching HTTP status.
- Every external call has a timeout.
- Upload WAVs are 16 kHz/16-bit/mono; reply WAVs arrive at the TTS provider's native rate, so playback reads the WAV header.
- `/healthz` is unauthenticated and loopback-only — never proxy it.
- Commit subjects: `feat(bridge|firmware): ...`, `fix(...)`, `test(...)`, `docs(...)` for component work; plain imperative subjects for repo-wide docs (specs, plans, README).

## Public repository hygiene

This repo is public. Before committing any document, generalize away environment specifics: employer/workplace network details, host machine names, real LAN IPs and SSIDs, and operator access paths (write "the Linux host", not the actual setup; use `192.0.2.x` and `<placeholder>` in examples). Credentials, tokens, `wifi.json`, and the bridge's `config.toml` (it holds device tokens) are gitignored — keep it that way. The device uses its factory MAC address by design; do not add MAC spoofing or randomization.
