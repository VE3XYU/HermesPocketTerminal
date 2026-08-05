# HTP Bridge

The HTP Bridge implements the device-facing half of the Hermes Terminal Protocol,
sitting between a Hermes Pocket Terminal and a Hermes Agent instance. It authenticates
devices, stores captured audio, drives transcription and reply synthesis, and exposes an
MCP server so Hermes Agent can publish dashboard content and queue notifications. See the
[protocol design spec](../docs/superpowers/specs/2026-08-04-hermes-terminal-protocol-design.md)
for the full picture.

## Install

```bash
python -m venv .venv
.venv/bin/pip install -e .
```

## Configure

Copy the example configuration and fill in real values:

```bash
cp htp-bridge.example.toml config.toml
```

Generate a device token with:

```bash
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

Put one `[[devices]]` block per pocket terminal in `config.toml`, each with its own
token. Fill in the `[agent]` and `[speech]` sections to point at your running Hermes
Agent and speech provider.

Reply WAVs are written at whatever sample rate the speech provider natively returns
(24 kHz for OpenAI TTS, for example) — the 16 kHz/16-bit/mono constraint in the protocol
spec applies to uploads only. Firmware must read the WAV header on playback rather than
assume a fixed rate.

## Run

```bash
htp-bridge --config config.toml
```

For firmware development without a database, speech provider, or Hermes Agent running,
serve canned responses instead:

```bash
htp-bridge --mock
```

`--mock` accepts any bearer token and answers every HTP endpoint with fixed data, so a
device can be brought up against a laptop with nothing else running.

## Connect Hermes Agent

Point Hermes Agent's MCP client at `http://127.0.0.1:8787/mcp` and instruct it to call
`publish_dashboard` whenever its task list changes, so pocket terminals stay in sync
without polling the agent directly.

## Deploy

1. Copy `deploy/htp-bridge.service` to `/etc/systemd/system/htp-bridge.service` and
   adjust the paths inside it (`/opt/htp-bridge`, `/etc/htp-bridge/config.toml`) to match
   your install.
2. Copy `deploy/Caddyfile.example` into your Caddy configuration, replacing
   `terminal.example.com` with your real hostname.
3. Enable and start the service:

   ```bash
   systemctl daemon-reload
   systemctl enable --now htp-bridge
   ```

4. Verify:
   - `curl http://127.0.0.1:8787/healthz` from the host itself. `/healthz` is
     intentionally not proxied by Caddy — it is unauthenticated and reports
     per-device battery and last-seen time, so it must only be reachable locally,
     never over HTTPS from outside.
   - `curl -i https://terminal.example.com/htp/v1/dashboard` from outside the host
     returns `401` without a bearer token, confirming the public route is protected.

## Test

```bash
python -m pytest
```

## Verify against your Hermes Agent

These are the open items from spec §11 — confirm each against the live system before
relying on this bridge in production:

- [ ] Hermes Agent API server specifics — exact endpoint shape, whether it accepts a
      model parameter, and whether it offers native conversation sessions.
- [ ] Hermes Agent MCP client configuration — how the running instance is pointed at
      the bridge's MCP server, and the mechanism for instructing it to republish the
      dashboard.
- [ ] Speech provider selection — specific STT and TTS vendors and voices.
- [ ] Public hostname and certificate — domain, DNS, and proxy configuration.
