# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

The **Hermes Pocket Terminal** — an ESP32-S3 handheld that acts as a thin client to the Hermes personal AI system — and the **HTP Bridge**, the server that connects it to Hermes Agent. Both are built: the bridge lives under `bridge/` (Python/FastAPI, with a `--mock` mode), the device firmware under `firmware/` (pure ESP-IDF, with three pure-C host-tested components). Each was executed from its TDD implementation plan; the firmware is hardware-verified through bench checkpoints C1–C7.

The documents, in dependency order:

1. `hermes-pocket-terminal-spec.md` — device spec: what the terminal does and deliberately does not do.
2. `docs/superpowers/specs/2026-08-04-hermes-terminal-protocol-design.md` — the authoritative protocol design: the HTP wire protocol (endpoints, schemas, capture states, idempotency) and the bridge architecture. Section references elsewhere (§4, §5.3…) point into this file.
3. `docs/superpowers/specs/2026-08-05-htp-firmware-design.md` — the firmware design (architecture, runtime model, UI, power), reconciled with the as-built device at wrap-up.
4. `docs/superpowers/plans/` — the executed implementation plans for both (checkbox tracking, one commit per task).

`firmware/README.md` is the operator manual: build, flashing, SD provisioning, calibration, tuning knobs, and the reliability-checklist status.

`reference/pala_note/` is third-party firmware kept locally for hardware reference only. It is gitignored and must never be committed or redistributed.

## Architecture

Three components: **device → bridge** speaks HTP (device-initiated HTTPS only, JSON + raw WAV, no push, no WebSockets); **bridge → Hermes Agent** sends transcribed text via the agent's OpenAI-compatible API; **Hermes Agent → bridge** publishes dashboard/notification state through MCP tools the bridge exposes.

Two invariants govern every design decision:

- **The terminal never knows what a task, note, or reminder is.** It renders generic primitives (list, text, status, audio) and matches on opaque IDs, never text.
- **The bridge never interprets content and makes no LLM calls.** It routes solely on a configured salutation prefix in the transcript ("hey hermes" → conversation with TTS reply; absent → note, `done` immediately after transcription).

The capture ID (device-generated, also the SD-card filename) is the end-to-end idempotency key: re-uploads are no-ops, retries are always safe. Reliability guarantees are enumerated in design §8 — the reliability test suite (plan Task 15) asserts those behaviors, not just happy paths.

## Bridge development

```bash
cd bridge
pip install -e ".[dev]"            # Python 3.11+ required (stdlib tomllib)
python -m pytest tests/ -v         # full suite
python -m pytest tests/test_captures.py -v        # one file
python -m pytest tests/ -k test_name -v           # one test
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
```

Firmware's binding rules: `components/htp_client`, `components/app_core`, and `components/ui` are pure C — no ESP-IDF includes, no direct OS/hardware calls; everything arrives through injected port structs. Verify before any commit touching them (must print nothing): `grep -rn "esp_\|freertos\|driver/" firmware/components/htp_client/src firmware/components/app_core/src firmware/components/ui/src`. All host tests green before every commit. Fixed-size buffers; heap only inside cJSON and transport bodies. Commits use `feat(firmware): ...` / `fix(firmware): ...` / `test(firmware): ...` / `docs(firmware): ...` subjects.

The plans' **Global Constraints** sections bind all code. The bridge ones most often violated by default habits:

- Time and ID generation are injected (`clock: Callable[[], int]`), never called inline — no `time.time()`, `datetime.now()`, or `uuid4()` in business logic.
- Tests never touch the network; speech/agent clients are faked, provider tests mock the HTTP transport.
- Timestamps are integer Unix epoch seconds everywhere — never floats or ISO strings.
- Errors are `{"error": "<snake_case_slug>"}` with the matching HTTP status.
- Every external call has a timeout.
- Bridge commits use `feat(bridge): ...` style (per the plan); doc commits use plain imperative subjects (per repo history).

## Public repository hygiene

This repo is public. Before committing any document, generalize away environment specifics: employer/workplace network details, host machine names, and operator access paths (write "the Linux host", not the actual setup). Credentials, tokens, and `wifi.json` are gitignored — keep it that way. The device uses its factory MAC address by design; do not add MAC spoofing or randomization.
