# HTP Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the HTP Bridge — the service that implements the Hermes Terminal Protocol for terminal devices and connects it to Hermes Agent, cloud speech-to-text, and cloud text-to-speech.

**Architecture:** A FastAPI application exposing seven HTTP endpoints under `/htp/v1/`, backed by SQLite (WAL mode) and a directory of audio files. An asynchronous capture pipeline moves each recording through transcription, salutation-based routing, agent conversation, and speech synthesis, updating a state row that device status polls read. A separate MCP server lets Hermes Agent publish dashboard state and queue notifications. The bridge performs no content interpretation and makes no LLM calls of its own.

**Tech Stack:** Python 3.11+, FastAPI, uvicorn, httpx, the `mcp` SDK, SQLite via stdlib `sqlite3`, TOML config via stdlib `tomllib`, pytest with pytest-asyncio.

**Source spec:** `docs/superpowers/specs/2026-08-04-hermes-terminal-protocol-design.md`. Section references below (§4, §5.3, etc.) point into it.

## Global Constraints

Every task's requirements implicitly include this section.

- **Python 3.11 or newer.** `tomllib` is stdlib from 3.11; do not add a TOML dependency.
- **All device-facing endpoints live under `/htp/v1/`.** No endpoint outside this prefix is exposed to devices.
- **The bridge never interprets capture content.** It routes on a configured salutation prefix only. No keyword matching, no intent classification, no LLM calls originating in the bridge.
- **Errors are returned as `{"error": "<slug>"}`** with the matching HTTP status. Slugs are lowercase snake_case.
- **Timestamps are integer Unix epoch seconds, UTC.** Never floats, never ISO strings, on the wire or in the database.
- **Audio is 16 kHz, 16-bit, mono WAV** in both directions. The bridge never transcodes.
- **The capture ID is the idempotency key.** Re-uploading an existing capture ID must never create a second capture or trigger a second pipeline run.
- **Time is injected, never called directly, in any module under test.** Every store and the pipeline take a `clock: Callable[[], int]` defaulting to `lambda: int(time.time())`. Tests pass a fake clock.
- **No `datetime.now()`, `time.time()`, or `uuid4()` calls inline in business logic.** ID generation is injected the same way as the clock.
- **Every external call has a timeout.** No HTTP request to a speech provider or to Hermes Agent may block indefinitely.
- **Tests never touch the network.** Speech and agent clients are used through fakes in all tests except the provider-specific tests, which mock the HTTP transport.
- **Commit after every task**, following the repository's existing commit-message convention.

## File Structure

```
bridge/
  pyproject.toml                  # package metadata, deps, pytest config
  htp-bridge.example.toml         # documented example configuration
  src/htp_bridge/
    __init__.py
    config.py                     # TOML load + validation → frozen dataclasses
    db.py                         # schema, WAL connection, read/write helpers
    devices.py                    # device registry, token auth, battery telemetry
    captures.py                   # capture rows: create (idempotent), state, transcripts
    storage.py                    # WAV files on disk: save, locate, prune
    dashboard.py                  # single-row dashboard snapshot + revision
    notifications.py              # notification queue
    salutation.py                 # salutation prefix detection (pure functions)
    speech.py                     # SpeechProvider protocol + OpenAI implementation
    agent.py                      # Hermes Agent client (OpenAI-compatible)
    pipeline.py                   # async capture pipeline, resume, redirect sweep
    api.py                        # FastAPI app factory, seven endpoints, auth dep
    mcp_server.py                 # MCP tools for Hermes Agent
    mock.py                       # --mock app serving canned responses
    main.py                       # entrypoint: wiring, background tasks, uvicorn
  tests/
    __init__.py                   # makes `from tests.conftest import AUTH` importable
    conftest.py                   # shared fixtures: temp db, fake clock, fakes
    test_config.py
    test_db.py
    test_devices.py
    test_captures.py
    test_storage.py
    test_api_captures.py
    test_dashboard.py
    test_notifications.py
    test_salutation.py
    test_speech.py
    test_agent.py
    test_pipeline.py
    test_mcp_server.py
    test_reliability.py
    fixtures/contract/            # golden HTP request/response pairs
  deploy/
    htp-bridge.service            # systemd unit
    Caddyfile.example             # TLS reverse proxy config
  README.md
```

---

### Task 1: Project scaffold and configuration

**Files:**
- Create: `bridge/pyproject.toml`
- Create: `bridge/src/htp_bridge/__init__.py`
- Create: `bridge/src/htp_bridge/config.py`
- Create: `bridge/htp-bridge.example.toml`
- Test: `bridge/tests/test_config.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `load_config(path: Path) -> Config`; frozen dataclasses `Config`, `ServerConfig`, `DeviceConfig`, `SalutationConfig`, `AgentConfig`, `SpeechConfig`, `StorageConfig`, `DashboardConfig`; exception `ConfigError`.

- [ ] **Step 1: Create the package skeleton**

```bash
mkdir -p bridge/src/htp_bridge bridge/tests/fixtures/contract bridge/deploy
touch bridge/src/htp_bridge/__init__.py
```

Write `bridge/pyproject.toml`:

```toml
[project]
name = "htp-bridge"
version = "0.1.0"
description = "Hermes Terminal Protocol bridge"
requires-python = ">=3.11"
dependencies = [
    "fastapi>=0.115",
    "uvicorn[standard]>=0.30",
    "httpx>=0.27",
    "mcp>=1.2",
]

[project.optional-dependencies]
dev = ["pytest>=8.0", "pytest-asyncio>=0.24"]

[project.scripts]
htp-bridge = "htp_bridge.main:main"

[build-system]
requires = ["setuptools>=68"]
build-backend = "setuptools.build_meta"

[tool.setuptools.packages.find]
where = ["src"]

[tool.pytest.ini_options]
asyncio_mode = "auto"
testpaths = ["tests"]
```

- [ ] **Step 2: Write the failing test**

Write `bridge/tests/test_config.py`:

```python
import pytest

from htp_bridge.config import ConfigError, load_config

VALID = """
[server]
host = "127.0.0.1"
port = 8787
max_upload_bytes = 4200000
sync_interval_seconds = 600

[[devices]]
id = "pocket-01"
token = "tok-aaaaaaaaaaaaaaaaaaaa"

[salutations]
prefixes = ["hey hermes", "hermes"]

[agent]
base_url = "http://127.0.0.1:8080/v1"
model = "hermes-fast"
api_key = ""
timeout_seconds = 60

[speech]
provider = "openai"
api_key = "sk-test"
stt_model = "whisper-1"
tts_model = "tts-1"
tts_voice = "alloy"
timeout_seconds = 60

[storage]
db_path = "/tmp/htp.db"
audio_dir = "/tmp/htp-audio"
upload_retention_days = 365
reply_retention_days = 7

[dashboard]
max_items = 32
max_text_chars = 40
"""


def write(tmp_path, text):
    path = tmp_path / "config.toml"
    path.write_text(text)
    return path


def test_loads_valid_config(tmp_path):
    cfg = load_config(write(tmp_path, VALID))
    assert cfg.server.port == 8787
    assert cfg.devices[0].id == "pocket-01"
    assert cfg.salutations.prefixes == ["hey hermes", "hermes"]
    assert cfg.agent.model == "hermes-fast"
    assert cfg.dashboard.max_items == 32


def test_rejects_config_with_no_devices(tmp_path):
    text = VALID.replace('[[devices]]\nid = "pocket-01"\ntoken = "tok-aaaaaaaaaaaaaaaaaaaa"\n', "")
    with pytest.raises(ConfigError, match="at least one device"):
        load_config(write(tmp_path, text))


def test_rejects_short_token(tmp_path):
    text = VALID.replace("tok-aaaaaaaaaaaaaaaaaaaa", "short")
    with pytest.raises(ConfigError, match="at least 16 characters"):
        load_config(write(tmp_path, text))


def test_rejects_duplicate_device_ids(tmp_path):
    text = VALID + '\n[[devices]]\nid = "pocket-01"\ntoken = "tok-bbbbbbbbbbbbbbbbbbbb"\n'
    with pytest.raises(ConfigError, match="duplicate device id"):
        load_config(write(tmp_path, text))


def test_rejects_empty_salutation_list(tmp_path):
    text = VALID.replace('prefixes = ["hey hermes", "hermes"]', "prefixes = []")
    with pytest.raises(ConfigError, match="at least one salutation"):
        load_config(write(tmp_path, text))


def test_missing_section_names_the_section(tmp_path):
    text = VALID.replace("[agent]", "[agent-disabled]")
    with pytest.raises(ConfigError, match="agent"):
        load_config(write(tmp_path, text))
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_config.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.config'`

- [ ] **Step 4: Write the implementation**

Write `bridge/src/htp_bridge/config.py`:

```python
from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path


class ConfigError(Exception):
    """Raised when a configuration file is missing required values or invalid."""


@dataclass(frozen=True)
class ServerConfig:
    host: str
    port: int
    max_upload_bytes: int
    sync_interval_seconds: int


@dataclass(frozen=True)
class DeviceConfig:
    id: str
    token: str


@dataclass(frozen=True)
class SalutationConfig:
    prefixes: list[str]


@dataclass(frozen=True)
class AgentConfig:
    base_url: str
    model: str | None
    api_key: str
    timeout_seconds: int


@dataclass(frozen=True)
class SpeechConfig:
    provider: str
    api_key: str
    stt_model: str
    tts_model: str
    tts_voice: str
    timeout_seconds: int
    base_url: str = "https://api.openai.com/v1"


@dataclass(frozen=True)
class StorageConfig:
    db_path: Path
    audio_dir: Path
    upload_retention_days: int
    reply_retention_days: int


@dataclass(frozen=True)
class DashboardConfig:
    max_items: int
    max_text_chars: int


@dataclass(frozen=True)
class Config:
    server: ServerConfig
    devices: list[DeviceConfig]
    salutations: SalutationConfig
    agent: AgentConfig
    speech: SpeechConfig
    storage: StorageConfig
    dashboard: DashboardConfig


MIN_TOKEN_LENGTH = 16


def _section(data: dict, name: str) -> dict:
    value = data.get(name)
    if not isinstance(value, dict):
        raise ConfigError(f"missing or invalid [{name}] section")
    return value


def _require(section: dict, key: str, section_name: str):
    if key not in section:
        raise ConfigError(f"missing '{key}' in [{section_name}]")
    return section[key]


def _devices(data: dict) -> list[DeviceConfig]:
    raw = data.get("devices") or []
    if not raw:
        raise ConfigError("configuration must define at least one device")
    devices: list[DeviceConfig] = []
    seen: set[str] = set()
    for entry in raw:
        device_id = _require(entry, "id", "devices")
        token = _require(entry, "token", "devices")
        if len(token) < MIN_TOKEN_LENGTH:
            raise ConfigError(
                f"device '{device_id}' token must be at least {MIN_TOKEN_LENGTH} characters"
            )
        if device_id in seen:
            raise ConfigError(f"duplicate device id '{device_id}'")
        seen.add(device_id)
        devices.append(DeviceConfig(id=device_id, token=token))
    return devices


def load_config(path: Path) -> Config:
    try:
        data = tomllib.loads(Path(path).read_text())
    except FileNotFoundError as exc:
        raise ConfigError(f"configuration file not found: {path}") from exc
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(f"invalid TOML in {path}: {exc}") from exc

    server = _section(data, "server")
    salutations = _section(data, "salutations")
    agent = _section(data, "agent")
    speech = _section(data, "speech")
    storage = _section(data, "storage")
    dashboard = _section(data, "dashboard")

    prefixes = salutations.get("prefixes") or []
    if not prefixes:
        raise ConfigError("configuration must define at least one salutation prefix")

    return Config(
        server=ServerConfig(
            host=server.get("host", "127.0.0.1"),
            port=int(server.get("port", 8787)),
            max_upload_bytes=int(server.get("max_upload_bytes", 4_200_000)),
            sync_interval_seconds=int(server.get("sync_interval_seconds", 600)),
        ),
        devices=_devices(data),
        salutations=SalutationConfig(prefixes=[str(p) for p in prefixes]),
        agent=AgentConfig(
            base_url=str(_require(agent, "base_url", "agent")).rstrip("/"),
            model=agent.get("model") or None,
            api_key=str(agent.get("api_key", "")),
            timeout_seconds=int(agent.get("timeout_seconds", 60)),
        ),
        speech=SpeechConfig(
            provider=str(speech.get("provider", "openai")),
            api_key=str(_require(speech, "api_key", "speech")),
            stt_model=str(speech.get("stt_model", "whisper-1")),
            tts_model=str(speech.get("tts_model", "tts-1")),
            tts_voice=str(speech.get("tts_voice", "alloy")),
            timeout_seconds=int(speech.get("timeout_seconds", 60)),
            base_url=str(speech.get("base_url", "https://api.openai.com/v1")).rstrip("/"),
        ),
        storage=StorageConfig(
            db_path=Path(_require(storage, "db_path", "storage")),
            audio_dir=Path(_require(storage, "audio_dir", "storage")),
            upload_retention_days=int(storage.get("upload_retention_days", 365)),
            reply_retention_days=int(storage.get("reply_retention_days", 7)),
        ),
        dashboard=DashboardConfig(
            max_items=int(dashboard.get("max_items", 32)),
            max_text_chars=int(dashboard.get("max_text_chars", 40)),
        ),
    )
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd bridge && pip install -e ".[dev]" && python -m pytest tests/test_config.py -v`
Expected: 6 passed

- [ ] **Step 6: Write the example configuration**

Write `bridge/htp-bridge.example.toml` — the `VALID` block from Step 2, with these edits: `db_path = "/var/lib/htp-bridge/htp.db"`, `audio_dir = "/var/lib/htp-bridge/audio"`, `token = "replace-with-a-long-random-string"`, `api_key = "replace-with-your-speech-api-key"`. Add a comment above `[[devices]]` reading `# One block per device. Generate tokens with: python -c "import secrets; print(secrets.token_urlsafe(32))"`.

- [ ] **Step 7: Commit**

```bash
git add bridge/pyproject.toml bridge/src/htp_bridge bridge/tests/test_config.py bridge/htp-bridge.example.toml
git commit -m "feat(bridge): add project scaffold and configuration loading"
```

---

### Task 2: Database layer

**Files:**
- Create: `bridge/src/htp_bridge/db.py`
- Test: `bridge/tests/test_db.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `class Database` with `__init__(path: Path)`, context managers `write()` and `read()` yielding a `sqlite3.Connection`, and `close()`. Module constant `SCHEMA`.

The schema below is the complete data model for the bridge. Later tasks add no columns.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_db.py`:

```python
import sqlite3

import pytest

from htp_bridge.db import Database


def test_creates_schema_on_first_open(tmp_path):
    db = Database(tmp_path / "htp.db")
    with db.read() as conn:
        names = {r["name"] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"captures", "dashboard", "notifications", "device_status"} <= names
    db.close()


def test_uses_write_ahead_logging(tmp_path):
    db = Database(tmp_path / "htp.db")
    with db.read() as conn:
        mode = conn.execute("PRAGMA journal_mode").fetchone()[0]
    assert mode.lower() == "wal"
    db.close()


def test_write_commits(tmp_path):
    path = tmp_path / "htp.db"
    db = Database(path)
    with db.write() as conn:
        conn.execute(
            "INSERT INTO notifications (id, text, priority, created_at) VALUES (?,?,?,?)",
            ("n-1", "hello", "normal", 100),
        )
    db.close()

    reopened = Database(path)
    with reopened.read() as conn:
        assert conn.execute("SELECT COUNT(*) FROM notifications").fetchone()[0] == 1
    reopened.close()


def test_write_rolls_back_on_error(tmp_path):
    db = Database(tmp_path / "htp.db")
    with pytest.raises(sqlite3.IntegrityError):
        with db.write() as conn:
            conn.execute(
                "INSERT INTO notifications (id, text, priority, created_at) VALUES (?,?,?,?)",
                ("n-1", "a", "normal", 100),
            )
            conn.execute(
                "INSERT INTO notifications (id, text, priority, created_at) VALUES (?,?,?,?)",
                ("n-1", "b", "normal", 100),
            )
    with db.read() as conn:
        assert conn.execute("SELECT COUNT(*) FROM notifications").fetchone()[0] == 0
    db.close()


def test_opening_twice_is_safe(tmp_path):
    path = tmp_path / "htp.db"
    Database(path).close()
    db = Database(path)
    with db.read() as conn:
        assert conn.execute("SELECT COUNT(*) FROM captures").fetchone()[0] == 0
    db.close()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_db.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.db'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/db.py`:

```python
from __future__ import annotations

import sqlite3
import threading
from contextlib import contextmanager
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS captures (
    id                   TEXT PRIMARY KEY,
    device_id            TEXT NOT NULL,
    state                TEXT NOT NULL,
    transcript           TEXT,
    error                TEXT,
    conversation_id      TEXT,
    reply_text           TEXT,
    recorded_at          INTEGER,
    created_at           INTEGER NOT NULL,
    updated_at           INTEGER NOT NULL,
    reply_ready_at       INTEGER,
    reply_downloaded_at  INTEGER,
    redirected           INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_captures_state ON captures(state);
CREATE INDEX IF NOT EXISTS idx_captures_conversation ON captures(conversation_id, created_at);

CREATE TABLE IF NOT EXISTS dashboard (
    id         INTEGER PRIMARY KEY CHECK (id = 1),
    rev        TEXT NOT NULL,
    title      TEXT NOT NULL,
    items_json TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS notifications (
    id         TEXT PRIMARY KEY,
    text       TEXT NOT NULL,
    priority   TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    acked_at   INTEGER
);
CREATE INDEX IF NOT EXISTS idx_notifications_acked ON notifications(acked_at, created_at);

CREATE TABLE IF NOT EXISTS device_status (
    device_id TEXT PRIMARY KEY,
    battery   INTEGER,
    last_seen INTEGER
);
"""


class Database:
    """A single SQLite connection guarded by a lock.

    The bridge is low-traffic and every statement is short, so one serialized
    connection is simpler and more predictable than a pool. WAL mode keeps
    committed state durable across crashes, which is what pipeline resume
    depends on.
    """

    def __init__(self, path: Path) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(str(path), check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
        self._conn.executescript(SCHEMA)
        self._conn.commit()

    @contextmanager
    def write(self):
        with self._lock:
            try:
                yield self._conn
                self._conn.commit()
            except Exception:
                self._conn.rollback()
                raise

    @contextmanager
    def read(self):
        with self._lock:
            yield self._conn

    def close(self) -> None:
        with self._lock:
            self._conn.close()
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_db.py -v`
Expected: 5 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/db.py bridge/tests/test_db.py
git commit -m "feat(bridge): add SQLite schema and connection layer"
```

---

### Task 3: Device registry and telemetry

**Files:**
- Create: `bridge/src/htp_bridge/devices.py`
- Create: `bridge/tests/conftest.py`
- Test: `bridge/tests/test_devices.py`

**Interfaces:**
- Consumes: `Database` (Task 2), `DeviceConfig` (Task 1).
- Produces: `class DeviceRegistry` with `authenticate(token: str) -> DeviceConfig | None`, `record_telemetry(device_id: str, battery: int | None) -> None`, `statuses() -> list[DeviceStatus]`; dataclass `DeviceStatus(device_id: str, battery: int | None, last_seen: int | None)`.
- Produces (fixtures): `fake_clock`, `db`, `device_config` in `conftest.py`.

- [ ] **Step 1: Write the shared fixtures**

Write `bridge/tests/conftest.py`:

```python
import pytest

from htp_bridge.config import DeviceConfig
from htp_bridge.db import Database


class FakeClock:
    """A clock tests can advance deliberately. Injected wherever time matters."""

    def __init__(self, now: int = 1_000_000) -> None:
        self.now = now

    def __call__(self) -> int:
        return self.now

    def advance(self, seconds: int) -> None:
        self.now += seconds


@pytest.fixture
def fake_clock():
    return FakeClock()


@pytest.fixture
def db(tmp_path):
    database = Database(tmp_path / "htp.db")
    yield database
    database.close()


@pytest.fixture
def device_config():
    return DeviceConfig(id="pocket-01", token="tok-aaaaaaaaaaaaaaaaaaaa")
```

- [ ] **Step 2: Write the failing test**

Write `bridge/tests/test_devices.py`:

```python
from htp_bridge.config import DeviceConfig
from htp_bridge.devices import DeviceRegistry


def registry(db, fake_clock, devices=None):
    devices = devices or [DeviceConfig(id="pocket-01", token="tok-aaaaaaaaaaaaaaaaaaaa")]
    return DeviceRegistry(db, devices, clock=fake_clock)


def test_authenticates_known_token(db, fake_clock):
    reg = registry(db, fake_clock)
    assert reg.authenticate("tok-aaaaaaaaaaaaaaaaaaaa").id == "pocket-01"


def test_rejects_unknown_token(db, fake_clock):
    assert registry(db, fake_clock).authenticate("tok-wrong-token-value") is None


def test_rejects_empty_token(db, fake_clock):
    assert registry(db, fake_clock).authenticate("") is None


def test_records_battery_and_last_seen(db, fake_clock):
    reg = registry(db, fake_clock)
    reg.record_telemetry("pocket-01", 78)
    status = reg.statuses()[0]
    assert status.device_id == "pocket-01"
    assert status.battery == 78
    assert status.last_seen == fake_clock.now


def test_telemetry_updates_in_place(db, fake_clock):
    reg = registry(db, fake_clock)
    reg.record_telemetry("pocket-01", 78)
    fake_clock.advance(60)
    reg.record_telemetry("pocket-01", 61)
    statuses = reg.statuses()
    assert len(statuses) == 1
    assert statuses[0].battery == 61
    assert statuses[0].last_seen == fake_clock.now


def test_missing_battery_still_updates_last_seen(db, fake_clock):
    reg = registry(db, fake_clock)
    reg.record_telemetry("pocket-01", None)
    status = reg.statuses()[0]
    assert status.battery is None
    assert status.last_seen == fake_clock.now


def test_out_of_range_battery_is_ignored(db, fake_clock):
    reg = registry(db, fake_clock)
    reg.record_telemetry("pocket-01", 150)
    assert reg.statuses()[0].battery is None
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_devices.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.devices'`

- [ ] **Step 4: Write the implementation**

Write `bridge/src/htp_bridge/devices.py`:

```python
from __future__ import annotations

import hmac
import time
from dataclasses import dataclass
from typing import Callable

from htp_bridge.config import DeviceConfig
from htp_bridge.db import Database


@dataclass(frozen=True)
class DeviceStatus:
    device_id: str
    battery: int | None
    last_seen: int | None


class DeviceRegistry:
    def __init__(
        self,
        db: Database,
        devices: list[DeviceConfig],
        clock: Callable[[], int] = lambda: int(time.time()),
    ) -> None:
        self._db = db
        self._devices = list(devices)
        self._clock = clock

    def authenticate(self, token: str) -> DeviceConfig | None:
        """Return the device owning this token, comparing in constant time."""
        if not token:
            return None
        for device in self._devices:
            if hmac.compare_digest(device.token, token):
                return device
        return None

    def record_telemetry(self, device_id: str, battery: int | None) -> None:
        if battery is not None and not 0 <= battery <= 100:
            battery = None
        now = self._clock()
        with self._db.write() as conn:
            conn.execute(
                """
                INSERT INTO device_status (device_id, battery, last_seen) VALUES (?,?,?)
                ON CONFLICT(device_id) DO UPDATE SET
                    battery = COALESCE(excluded.battery, device_status.battery),
                    last_seen = excluded.last_seen
                """,
                (device_id, battery, now),
            )

    def statuses(self) -> list[DeviceStatus]:
        with self._db.read() as conn:
            rows = conn.execute(
                "SELECT device_id, battery, last_seen FROM device_status ORDER BY device_id"
            ).fetchall()
        return [DeviceStatus(r["device_id"], r["battery"], r["last_seen"]) for r in rows]
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_devices.py -v`
Expected: 7 passed

Note: `test_out_of_range_battery_is_ignored` passes because an out-of-range value is normalized to `None`, and `COALESCE` then leaves the stored value untouched — which is `NULL` for a first write.

- [ ] **Step 6: Commit**

```bash
git add bridge/src/htp_bridge/devices.py bridge/tests/conftest.py bridge/tests/test_devices.py
git commit -m "feat(bridge): add device registry with constant-time token auth"
```

---

### Task 4: Capture store

This task owns the idempotency guarantee (§3 "Idempotency"). Everything else in the
bridge depends on `create()` being atomically safe to call twice.

**Files:**
- Create: `bridge/src/htp_bridge/captures.py`
- Test: `bridge/tests/test_captures.py`

**Interfaces:**
- Consumes: `Database` (Task 2).
- Produces: dataclass `Capture`; `class CaptureStore` with `create()`, `get()`, `get_many()`, `set_state()`, `set_transcript()`, `set_conversation()`, `set_reply()`, `mark_downloaded()`, `unfinished_ids()`, `redirect_candidates()`, `mark_redirected()`, `conversation_history()`, `ingestion_backlog()`. Constants `TERMINAL_STATES`, `ACTIVE_STATES`, `INGEST_FAILED`.

A note reaches `done` before the agent has ingested it, so ingestion failure cannot be
represented as a state. It is recorded as `error = 'ingest_failed'` on an otherwise
`done` capture, and `ingestion_backlog()` is what the retry sweep reads.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_captures.py`:

```python
from htp_bridge.captures import CaptureStore


def store(db, fake_clock):
    return CaptureStore(db, clock=fake_clock)


def test_create_returns_capture_marked_new(db, fake_clock):
    cap, created = store(db, fake_clock).create(
        capture_id="c-1", device_id="pocket-01", recorded_at=999, conversation_id=None
    )
    assert created is True
    assert cap.id == "c-1"
    assert cap.state == "received"
    assert cap.created_at == fake_clock.now


def test_create_is_idempotent(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=999, conversation_id=None)
    s.set_state("c-1", "done")

    cap, created = s.create(
        capture_id="c-1", device_id="pocket-01", recorded_at=999, conversation_id=None
    )

    assert created is False
    assert cap.state == "done", "a repeat upload must not reset an in-flight capture"
    assert len(s.get_many(["c-1"])) == 1


def test_get_many_preserves_requested_order_and_skips_unknown(db, fake_clock):
    s = store(db, fake_clock)
    for cid in ("c-1", "c-2"):
        s.create(capture_id=cid, device_id="pocket-01", recorded_at=1, conversation_id=None)

    found = s.get_many(["c-2", "c-missing", "c-1"])

    assert [c.id for c in found] == ["c-2", "c-1"]


def test_set_transcript_persists_and_bumps_updated_at(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    fake_clock.advance(5)
    s.set_transcript("c-1", "Add milk to the shopping list")

    cap = s.get("c-1")
    assert cap.transcript == "Add milk to the shopping list"
    assert cap.updated_at == fake_clock.now


def test_set_state_failed_records_error(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_state("c-1", "failed", error="transcription_failed")

    cap = s.get("c-1")
    assert cap.state == "failed"
    assert cap.error == "transcription_failed"


def test_set_reply_marks_ready_and_stamps_time(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_reply("c-1", "Added milk to your shopping list.")

    cap = s.get("c-1")
    assert cap.state == "reply_ready"
    assert cap.reply_text == "Added milk to your shopping list."
    assert cap.reply_ready_at == fake_clock.now


def test_unfinished_ids_returns_only_non_terminal_captures(db, fake_clock):
    s = store(db, fake_clock)
    for cid, state in (("c-1", "received"), ("c-2", "transcribing"), ("c-3", "processing")):
        s.create(capture_id=cid, device_id="pocket-01", recorded_at=1, conversation_id=None)
        s.set_state(cid, state)
    s.create(capture_id="c-4", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_state("c-4", "done")
    s.create(capture_id="c-5", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_reply("c-5", "hi")

    assert sorted(s.unfinished_ids()) == ["c-1", "c-2", "c-3"]


def test_redirect_candidates_respect_grace_and_download(db, fake_clock):
    s = store(db, fake_clock)
    for cid in ("c-old", "c-fresh", "c-downloaded"):
        s.create(capture_id=cid, device_id="pocket-01", recorded_at=1, conversation_id=None)
        s.set_reply(cid, "answer")
    s.mark_downloaded("c-downloaded")

    fake_clock.advance(100)
    s.set_reply("c-fresh", "answer")  # re-stamps reply_ready_at to now

    candidates = [c.id for c in s.redirect_candidates(older_than=fake_clock.now - 90)]

    assert candidates == ["c-old"]


def test_mark_redirected_removes_from_candidates(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_reply("c-1", "answer")
    fake_clock.advance(100)
    s.mark_redirected("c-1")

    assert s.redirect_candidates(older_than=fake_clock.now) == []


def test_conversation_history_returns_completed_exchanges_oldest_first(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id="v-1")
    s.set_transcript("c-1", "what time is dinner")
    s.set_reply("c-1", "Dinner is at 7 PM.")
    fake_clock.advance(10)
    s.create(capture_id="c-2", device_id="pocket-01", recorded_at=2, conversation_id="v-1")
    s.set_transcript("c-2", "and dessert")
    s.set_reply("c-2", "Dessert is at 8 PM.")
    fake_clock.advance(10)
    s.create(capture_id="c-3", device_id="pocket-01", recorded_at=3, conversation_id="v-1")
    s.set_transcript("c-3", "thanks")

    history = s.conversation_history("v-1", exclude_id="c-3")

    assert history == [
        ("what time is dinner", "Dinner is at 7 PM."),
        ("and dessert", "Dessert is at 8 PM."),
    ]


def test_conversation_history_is_empty_for_unknown_conversation(db, fake_clock):
    assert store(db, fake_clock).conversation_history("v-nope", exclude_id="c-1") == []


def test_ingestion_backlog_lists_done_captures_flagged_failed(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_transcript("c-1", "Add milk")
    s.set_state("c-1", "done", error="ingest_failed")
    s.create(capture_id="c-2", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_state("c-2", "done")

    assert [c.id for c in s.ingestion_backlog()] == ["c-1"]


def test_clearing_the_error_removes_it_from_the_backlog(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    s.set_state("c-1", "done", error="ingest_failed")
    s.set_state("c-1", "done", error=None)

    assert s.ingestion_backlog() == []
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_captures.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.captures'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/captures.py`:

```python
from __future__ import annotations

import sqlite3
import time
from dataclasses import dataclass
from typing import Callable

from htp_bridge.db import Database

ACTIVE_STATES = ("received", "transcribing", "processing")
TERMINAL_STATES = ("done", "reply_ready", "failed")
INGEST_FAILED = "ingest_failed"


@dataclass(frozen=True)
class Capture:
    id: str
    device_id: str
    state: str
    transcript: str | None
    error: str | None
    conversation_id: str | None
    reply_text: str | None
    recorded_at: int | None
    created_at: int
    updated_at: int
    reply_ready_at: int | None
    reply_downloaded_at: int | None
    redirected: bool


def _to_capture(row: sqlite3.Row) -> Capture:
    return Capture(
        id=row["id"],
        device_id=row["device_id"],
        state=row["state"],
        transcript=row["transcript"],
        error=row["error"],
        conversation_id=row["conversation_id"],
        reply_text=row["reply_text"],
        recorded_at=row["recorded_at"],
        created_at=row["created_at"],
        updated_at=row["updated_at"],
        reply_ready_at=row["reply_ready_at"],
        reply_downloaded_at=row["reply_downloaded_at"],
        redirected=bool(row["redirected"]),
    )


class CaptureStore:
    def __init__(self, db: Database, clock: Callable[[], int] = lambda: int(time.time())) -> None:
        self._db = db
        self._clock = clock

    def create(
        self,
        *,
        capture_id: str,
        device_id: str,
        recorded_at: int | None,
        conversation_id: str | None,
    ) -> tuple[Capture, bool]:
        """Create a capture, or return the existing one untouched.

        The INSERT OR IGNORE makes this atomic: a duplicate upload can never
        produce a second row or reset an in-flight capture's state.
        """
        now = self._clock()
        with self._db.write() as conn:
            cursor = conn.execute(
                """
                INSERT OR IGNORE INTO captures
                    (id, device_id, state, recorded_at, conversation_id, created_at, updated_at)
                VALUES (?,?,?,?,?,?,?)
                """,
                (capture_id, device_id, "received", recorded_at, conversation_id, now, now),
            )
            created = cursor.rowcount == 1
            row = conn.execute("SELECT * FROM captures WHERE id=?", (capture_id,)).fetchone()
        return _to_capture(row), created

    def get(self, capture_id: str) -> Capture | None:
        with self._db.read() as conn:
            row = conn.execute("SELECT * FROM captures WHERE id=?", (capture_id,)).fetchone()
        return _to_capture(row) if row else None

    def get_many(self, capture_ids: list[str]) -> list[Capture]:
        """Return captures in the order requested, silently skipping unknown IDs."""
        if not capture_ids:
            return []
        placeholders = ",".join("?" for _ in capture_ids)
        with self._db.read() as conn:
            rows = conn.execute(
                f"SELECT * FROM captures WHERE id IN ({placeholders})", tuple(capture_ids)
            ).fetchall()
        by_id = {row["id"]: _to_capture(row) for row in rows}
        return [by_id[cid] for cid in capture_ids if cid in by_id]

    def set_state(self, capture_id: str, state: str, *, error: str | None = None) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET state=?, error=?, updated_at=? WHERE id=?",
                (state, error, self._clock(), capture_id),
            )

    def set_transcript(self, capture_id: str, transcript: str) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET transcript=?, updated_at=? WHERE id=?",
                (transcript, self._clock(), capture_id),
            )

    def set_conversation(self, capture_id: str, conversation_id: str) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET conversation_id=?, updated_at=? WHERE id=?",
                (conversation_id, self._clock(), capture_id),
            )

    def set_reply(self, capture_id: str, reply_text: str) -> None:
        now = self._clock()
        with self._db.write() as conn:
            conn.execute(
                """
                UPDATE captures
                   SET state='reply_ready', reply_text=?, reply_ready_at=?, error=NULL, updated_at=?
                 WHERE id=?
                """,
                (reply_text, now, now, capture_id),
            )

    def mark_downloaded(self, capture_id: str) -> None:
        now = self._clock()
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET reply_downloaded_at=?, updated_at=? WHERE id=?",
                (now, now, capture_id),
            )

    def unfinished_ids(self) -> list[str]:
        """Captures that were mid-pipeline when the process last stopped."""
        placeholders = ",".join("?" for _ in ACTIVE_STATES)
        with self._db.read() as conn:
            rows = conn.execute(
                f"SELECT id FROM captures WHERE state IN ({placeholders}) ORDER BY created_at",
                ACTIVE_STATES,
            ).fetchall()
        return [row["id"] for row in rows]

    def redirect_candidates(self, *, older_than: int) -> list[Capture]:
        """Replies that went unclaimed long enough to belong in the notification queue."""
        with self._db.read() as conn:
            rows = conn.execute(
                """
                SELECT * FROM captures
                 WHERE state='reply_ready'
                   AND redirected=0
                   AND reply_downloaded_at IS NULL
                   AND reply_ready_at IS NOT NULL
                   AND reply_ready_at < ?
                 ORDER BY reply_ready_at
                """,
                (older_than,),
            ).fetchall()
        return [_to_capture(row) for row in rows]

    def mark_redirected(self, capture_id: str) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET redirected=1, updated_at=? WHERE id=?",
                (self._clock(), capture_id),
            )

    def ingestion_backlog(self) -> list[Capture]:
        """Notes the device considers finished but the agent has not accepted yet."""
        with self._db.read() as conn:
            rows = conn.execute(
                "SELECT * FROM captures WHERE state='done' AND error=? ORDER BY created_at",
                (INGEST_FAILED,),
            ).fetchall()
        return [_to_capture(row) for row in rows]

    def conversation_history(self, conversation_id: str, *, exclude_id: str) -> list[tuple[str, str]]:
        """Completed (user, assistant) exchanges in this conversation, oldest first."""
        with self._db.read() as conn:
            rows = conn.execute(
                """
                SELECT transcript, reply_text FROM captures
                 WHERE conversation_id=?
                   AND id != ?
                   AND transcript IS NOT NULL
                   AND reply_text IS NOT NULL
                 ORDER BY created_at
                """,
                (conversation_id, exclude_id),
            ).fetchall()
        return [(row["transcript"], row["reply_text"]) for row in rows]
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_captures.py -v`
Expected: 13 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/captures.py bridge/tests/test_captures.py
git commit -m "feat(bridge): add capture store with idempotent creation"
```

---

### Task 5: Audio storage

**Files:**
- Create: `bridge/src/htp_bridge/storage.py`
- Test: `bridge/tests/test_storage.py`

**Interfaces:**
- Consumes: `StorageConfig` (Task 1).
- Produces: `class AudioStorage` with `save_upload(capture_id, data) -> Path`, `save_reply(capture_id, data) -> Path`, `upload_path(capture_id) -> Path`, `reply_path(capture_id) -> Path`, `has_reply(capture_id) -> bool`, `prune() -> int`; function `is_valid_capture_id(value: str) -> bool`.

Capture IDs arrive from an untrusted client and become filenames, so validation lives
here alongside the paths it protects.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_storage.py`:

```python
import pytest

from htp_bridge.config import StorageConfig
from htp_bridge.storage import AudioStorage, is_valid_capture_id


@pytest.fixture
def storage(tmp_path, fake_clock):
    cfg = StorageConfig(
        db_path=tmp_path / "htp.db",
        audio_dir=tmp_path / "audio",
        upload_retention_days=365,
        reply_retention_days=7,
    )
    return AudioStorage(cfg, clock=fake_clock)


@pytest.mark.parametrize("value", ["c-20260804-101502-3fa9", "c-1", "v-abc_DEF-123"])
def test_accepts_reasonable_ids(value):
    assert is_valid_capture_id(value)


@pytest.mark.parametrize(
    "value",
    ["", "../escape", "c-1/../../etc/passwd", "c 1", "c-1.wav", "a" * 129, "c-1\x00"],
)
def test_rejects_unsafe_ids(value):
    assert not is_valid_capture_id(value)


def test_save_upload_writes_bytes_and_returns_path(storage):
    path = storage.save_upload("c-1", b"RIFFfake")
    assert path.read_bytes() == b"RIFFfake"
    assert path == storage.upload_path("c-1")


def test_save_upload_is_overwrite_safe(storage):
    storage.save_upload("c-1", b"first")
    storage.save_upload("c-1", b"second")
    assert storage.upload_path("c-1").read_bytes() == b"second"


def test_has_reply_reflects_file_presence(storage):
    assert storage.has_reply("c-1") is False
    storage.reply_path("c-1").write_bytes(b"RIFFreply")
    assert storage.has_reply("c-1") is True


def test_uploads_and_replies_use_separate_files(storage):
    assert storage.upload_path("c-1") != storage.reply_path("c-1")


def test_prune_removes_only_expired_files(storage, fake_clock):
    storage.save_upload("c-old", b"x")
    storage.save_reply("c-old", b"y")
    fake_clock.advance(8 * 86400)
    storage.save_upload("c-new", b"x")
    storage.save_reply("c-new", b"y")

    removed = storage.prune()

    assert not storage.reply_path("c-old").exists(), "reply older than 7 days should go"
    assert storage.upload_path("c-old").exists(), "upload retention is 365 days"
    assert storage.reply_path("c-new").exists()
    assert removed == 1
```

Note: retention is measured against file modification times, and the tests run on a fake
clock. `save_upload` and `save_reply` therefore stamp mtimes from the injected clock via
`os.utime` — which is why the prune test writes through those methods rather than
`Path.write_bytes`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_storage.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.storage'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/storage.py`:

```python
from __future__ import annotations

import os
import re
import time
from pathlib import Path
from typing import Callable

from htp_bridge.config import StorageConfig

_ID_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,128}$")


def is_valid_capture_id(value: str) -> bool:
    """Capture IDs become filenames, so restrict them to a safe alphabet."""
    return bool(_ID_PATTERN.match(value or ""))


class AudioStorage:
    def __init__(
        self,
        config: StorageConfig,
        clock: Callable[[], int] = lambda: int(time.time()),
    ) -> None:
        self._uploads = Path(config.audio_dir) / "uploads"
        self._replies = Path(config.audio_dir) / "replies"
        self._uploads.mkdir(parents=True, exist_ok=True)
        self._replies.mkdir(parents=True, exist_ok=True)
        self._upload_retention = config.upload_retention_days * 86400
        self._reply_retention = config.reply_retention_days * 86400
        self._clock = clock

    def upload_path(self, capture_id: str) -> Path:
        return self._uploads / f"{capture_id}.wav"

    def reply_path(self, capture_id: str) -> Path:
        return self._replies / f"{capture_id}.wav"

    def save_upload(self, capture_id: str, data: bytes) -> Path:
        path = self.upload_path(capture_id)
        temporary = path.with_suffix(".wav.part")
        temporary.write_bytes(data)
        temporary.replace(path)
        self._stamp(path)
        return path

    def save_reply(self, capture_id: str, data: bytes) -> Path:
        path = self.reply_path(capture_id)
        temporary = path.with_suffix(".wav.part")
        temporary.write_bytes(data)
        temporary.replace(path)
        self._stamp(path)
        return path

    def has_reply(self, capture_id: str) -> bool:
        return self.reply_path(capture_id).exists()

    def prune(self) -> int:
        """Delete audio past its retention window. Returns the number removed."""
        now = self._clock()
        removed = 0
        for directory, retention in ((self._uploads, self._upload_retention), (self._replies, self._reply_retention)):
            for path in directory.glob("*.wav"):
                if now - int(path.stat().st_mtime) > retention:
                    path.unlink(missing_ok=True)
                    removed += 1
        return removed

    def _stamp(self, path: Path) -> None:
        """Set mtime from the injected clock so retention is testable."""
        now = self._clock()
        os.utime(path, (now, now))
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_storage.py -v`
Expected: 15 passed (the two parametrized tests expand to 3 and 7 cases)

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/storage.py bridge/tests/test_storage.py
git commit -m "feat(bridge): add audio storage with ID validation and retention"
```

---

### Task 6: Dashboard store

**Files:**
- Create: `bridge/src/htp_bridge/dashboard.py`
- Test: `bridge/tests/test_dashboard.py`

**Interfaces:**
- Consumes: `Database` (Task 2), `DashboardConfig` (Task 1).
- Produces: dataclasses `DashboardItem(id, text, done, style)` and `DashboardSnapshot(rev, title, items, updated_at)`; `class DashboardStore` with `publish(title, items) -> str`, `current() -> DashboardSnapshot | None`, `complete(item_id) -> str | None`.

`publish` accepts raw dicts from the MCP tool and normalizes them: caps the item count,
truncates text, drops unusable entries, and computes the revision (§5.5).

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_dashboard.py`:

```python
from htp_bridge.config import DashboardConfig
from htp_bridge.dashboard import DashboardStore


def store(db, fake_clock, *, max_items=32, max_text_chars=40):
    cfg = DashboardConfig(max_items=max_items, max_text_chars=max_text_chars)
    return DashboardStore(db, cfg, clock=fake_clock)


def test_current_is_none_before_first_publish(db, fake_clock):
    assert store(db, fake_clock).current() is None


def test_publish_stores_items_and_returns_revision(db, fake_clock):
    s = store(db, fake_clock)
    rev = s.publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])

    snapshot = s.current()
    assert snapshot.rev == rev
    assert snapshot.title == "Today"
    assert snapshot.items[0].id == "t-1"
    assert snapshot.items[0].text == "Buy milk"
    assert snapshot.items[0].done is False
    assert snapshot.updated_at == fake_clock.now


def test_revision_is_stable_for_identical_content(db, fake_clock):
    s = store(db, fake_clock)
    items = [{"id": "t-1", "text": "Buy milk", "done": False}]
    first = s.publish("Today", items)
    fake_clock.advance(500)
    second = s.publish("Today", items)
    assert first == second, "an unchanged list must not force the device to redraw"


def test_revision_changes_when_content_changes(db, fake_clock):
    s = store(db, fake_clock)
    first = s.publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    second = s.publish("Today", [{"id": "t-1", "text": "Buy oat milk", "done": False}])
    assert first != second


def test_publish_replaces_previous_items(db, fake_clock):
    s = store(db, fake_clock)
    s.publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    s.publish("Today", [{"id": "t-2", "text": "Call dentist", "done": False}])
    assert [i.id for i in s.current().items] == ["t-2"]


def test_publish_caps_item_count(db, fake_clock):
    s = store(db, fake_clock, max_items=2)
    s.publish("Today", [{"id": f"t-{n}", "text": "x", "done": False} for n in range(5)])
    assert len(s.current().items) == 2


def test_publish_truncates_long_text(db, fake_clock):
    s = store(db, fake_clock, max_text_chars=10)
    s.publish("Today", [{"id": "t-1", "text": "A very long task description", "done": False}])
    text = s.current().items[0].text
    assert len(text) == 10
    assert text.endswith("…")


def test_publish_skips_items_without_id_or_text(db, fake_clock):
    s = store(db, fake_clock)
    s.publish(
        "Today",
        [
            {"id": "t-1", "text": "Keep", "done": False},
            {"text": "No id"},
            {"id": "t-3", "text": ""},
        ],
    )
    assert [i.id for i in s.current().items] == ["t-1"]


def test_publish_preserves_known_style_and_drops_unknown(db, fake_clock):
    s = store(db, fake_clock)
    s.publish(
        "Today",
        [
            {"id": "t-1", "text": "Bold one", "done": False, "style": "bold"},
            {"id": "t-2", "text": "Odd one", "done": False, "style": "sparkly"},
        ],
    )
    items = s.current().items
    assert items[0].style == "bold"
    assert items[1].style is None


def test_complete_marks_item_and_returns_new_revision(db, fake_clock):
    s = store(db, fake_clock)
    old = s.publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])

    new = s.complete("t-1")

    assert new is not None and new != old
    assert s.current().items[0].done is True


def test_complete_is_idempotent(db, fake_clock):
    s = store(db, fake_clock)
    s.publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    first = s.complete("t-1")
    second = s.complete("t-1")
    assert first == second


def test_complete_returns_none_for_unknown_item(db, fake_clock):
    s = store(db, fake_clock)
    s.publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    assert s.complete("t-missing") is None


def test_complete_before_any_publish_returns_none(db, fake_clock):
    assert store(db, fake_clock).complete("t-1") is None
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_dashboard.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.dashboard'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/dashboard.py`:

```python
from __future__ import annotations

import hashlib
import json
import time
from dataclasses import dataclass, asdict
from typing import Any, Callable

from htp_bridge.config import DashboardConfig
from htp_bridge.db import Database

KNOWN_STYLES = {"bold", "dim"}


@dataclass(frozen=True)
class DashboardItem:
    id: str
    text: str
    done: bool
    style: str | None = None


@dataclass(frozen=True)
class DashboardSnapshot:
    rev: str
    title: str
    items: list[DashboardItem]
    updated_at: int


def _revision(title: str, items: list[DashboardItem]) -> str:
    """Content hash. Identical content must yield an identical revision so the
    device can skip an e-paper refresh."""
    payload = json.dumps(
        {"title": title, "items": [asdict(item) for item in items]},
        sort_keys=True,
        separators=(",", ":"),
    )
    return hashlib.sha256(payload.encode()).hexdigest()[:8]


class DashboardStore:
    def __init__(
        self,
        db: Database,
        config: DashboardConfig,
        clock: Callable[[], int] = lambda: int(time.time()),
    ) -> None:
        self._db = db
        self._config = config
        self._clock = clock

    def publish(self, title: str, items: list[dict[str, Any]]) -> str:
        normalized = self._normalize(items)
        rev = _revision(title, normalized)
        payload = json.dumps([asdict(item) for item in normalized])
        with self._db.write() as conn:
            conn.execute(
                """
                INSERT INTO dashboard (id, rev, title, items_json, updated_at) VALUES (1,?,?,?,?)
                ON CONFLICT(id) DO UPDATE SET
                    rev=excluded.rev,
                    title=excluded.title,
                    items_json=excluded.items_json,
                    updated_at=excluded.updated_at
                """,
                (rev, title, payload, self._clock()),
            )
        return rev

    def current(self) -> DashboardSnapshot | None:
        with self._db.read() as conn:
            row = conn.execute("SELECT * FROM dashboard WHERE id=1").fetchone()
        if row is None:
            return None
        items = [DashboardItem(**entry) for entry in json.loads(row["items_json"])]
        return DashboardSnapshot(
            rev=row["rev"], title=row["title"], items=items, updated_at=row["updated_at"]
        )

    def complete(self, item_id: str) -> str | None:
        """Mark an item done in the snapshot. Returns the new revision, or None
        if there is no such item. The agent's next publish overrides this."""
        snapshot = self.current()
        if snapshot is None:
            return None
        if not any(item.id == item_id for item in snapshot.items):
            return None
        updated = [
            DashboardItem(id=i.id, text=i.text, done=True, style=i.style) if i.id == item_id else i
            for i in snapshot.items
        ]
        return self.publish(snapshot.title, [asdict(item) for item in updated])

    def _normalize(self, items: list[dict[str, Any]]) -> list[DashboardItem]:
        normalized: list[DashboardItem] = []
        for entry in items or []:
            if not isinstance(entry, dict):
                continue
            item_id = str(entry.get("id") or "").strip()
            text = str(entry.get("text") or "").strip()
            if not item_id or not text:
                continue
            if len(text) > self._config.max_text_chars:
                text = text[: self._config.max_text_chars - 1] + "…"
            style = entry.get("style")
            normalized.append(
                DashboardItem(
                    id=item_id,
                    text=text,
                    done=bool(entry.get("done", False)),
                    style=style if style in KNOWN_STYLES else None,
                )
            )
            if len(normalized) >= self._config.max_items:
                break
        return normalized
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_dashboard.py -v`
Expected: 13 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/dashboard.py bridge/tests/test_dashboard.py
git commit -m "feat(bridge): add dashboard store with content-hash revisions"
```

---

### Task 7: Notification queue

**Files:**
- Create: `bridge/src/htp_bridge/notifications.py`
- Test: `bridge/tests/test_notifications.py`

**Interfaces:**
- Consumes: `Database` (Task 2).
- Produces: dataclass `Notification(id, text, priority, created_at)`; `class NotificationStore` with `enqueue(text, priority="normal") -> str`, `pending() -> list[Notification]`, `ack(ids) -> int`.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_notifications.py`:

```python
import itertools

import pytest

from htp_bridge.notifications import NotificationStore


@pytest.fixture
def store(db, fake_clock):
    counter = itertools.count(1)
    return NotificationStore(db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}")


def test_enqueue_returns_id_and_appears_in_pending(store, fake_clock):
    notification_id = store.enqueue("Meeting with Alex at 10:00 AM", "urgent")

    pending = store.pending()
    assert notification_id == "n-1"
    assert pending[0].id == "n-1"
    assert pending[0].text == "Meeting with Alex at 10:00 AM"
    assert pending[0].priority == "urgent"
    assert pending[0].created_at == fake_clock.now


def test_priority_defaults_to_normal(store):
    store.enqueue("A plain note")
    assert store.pending()[0].priority == "normal"


def test_unknown_priority_becomes_normal(store):
    store.enqueue("Odd", "screaming")
    assert store.pending()[0].priority == "normal"


def test_pending_is_oldest_first(store, fake_clock):
    store.enqueue("first")
    fake_clock.advance(10)
    store.enqueue("second")
    assert [n.text for n in store.pending()] == ["first", "second"]


def test_ack_removes_from_pending(store):
    first = store.enqueue("first")
    store.enqueue("second")

    acked = store.ack([first])

    assert acked == 1
    assert [n.text for n in store.pending()] == ["second"]


def test_ack_is_idempotent(store):
    first = store.enqueue("first")
    store.ack([first])
    assert store.ack([first]) == 0


def test_ack_ignores_unknown_ids(store):
    assert store.ack(["n-nope"]) == 0


def test_ack_with_empty_list_is_a_noop(store):
    store.enqueue("first")
    assert store.ack([]) == 0
    assert len(store.pending()) == 1


def test_unacked_notifications_redeliver(store):
    """The device may die before displaying; delivery is at-least-once."""
    store.enqueue("Meeting with Alex at 10:00 AM")
    assert len(store.pending()) == 1
    assert len(store.pending()) == 1, "fetching does not consume"
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_notifications.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.notifications'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/notifications.py`:

```python
from __future__ import annotations

import time
import uuid
from dataclasses import dataclass
from typing import Callable

from htp_bridge.db import Database

VALID_PRIORITIES = {"normal", "urgent"}


@dataclass(frozen=True)
class Notification:
    id: str
    text: str
    priority: str
    created_at: int


def _default_id() -> str:
    return f"n-{uuid.uuid4().hex[:8]}"


class NotificationStore:
    def __init__(
        self,
        db: Database,
        clock: Callable[[], int] = lambda: int(time.time()),
        id_factory: Callable[[], str] = _default_id,
    ) -> None:
        self._db = db
        self._clock = clock
        self._id_factory = id_factory

    def enqueue(self, text: str, priority: str = "normal") -> str:
        if priority not in VALID_PRIORITIES:
            priority = "normal"
        notification_id = self._id_factory()
        with self._db.write() as conn:
            conn.execute(
                "INSERT INTO notifications (id, text, priority, created_at) VALUES (?,?,?,?)",
                (notification_id, text, priority, self._clock()),
            )
        return notification_id

    def pending(self) -> list[Notification]:
        """Unacknowledged notifications, oldest first. Fetching does not consume:
        delivery is confirmed by an explicit ack from the device."""
        with self._db.read() as conn:
            rows = conn.execute(
                "SELECT * FROM notifications WHERE acked_at IS NULL ORDER BY created_at, id"
            ).fetchall()
        return [
            Notification(r["id"], r["text"], r["priority"], r["created_at"]) for r in rows
        ]

    def ack(self, ids: list[str]) -> int:
        if not ids:
            return 0
        placeholders = ",".join("?" for _ in ids)
        with self._db.write() as conn:
            cursor = conn.execute(
                f"UPDATE notifications SET acked_at=? WHERE acked_at IS NULL AND id IN ({placeholders})",
                (self._clock(), *ids),
            )
            return cursor.rowcount
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_notifications.py -v`
Expected: 9 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/notifications.py bridge/tests/test_notifications.py
git commit -m "feat(bridge): add notification queue with explicit acknowledgement"
```

---

### Task 8: Salutation detection

This is the entire routing decision for the bridge (§6.2). It is a pure function on the
transcript — no model, no heuristics beyond prefix matching.

**Files:**
- Create: `bridge/src/htp_bridge/salutation.py`
- Test: `bridge/tests/test_salutation.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `detect(transcript: str, prefixes: list[str]) -> str | None` — returns the remainder after the salutation, or `None` when no salutation is present. An empty string means the user said only the salutation.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_salutation.py`:

```python
import pytest

from htp_bridge.salutation import detect

PREFIXES = ["hey hermes", "hermes"]


@pytest.mark.parametrize(
    "transcript,expected",
    [
        ("Hey Hermes, what's on my calendar today?", "what's on my calendar today?"),
        ("hey hermes what's on my calendar", "what's on my calendar"),
        ("Hey, Hermes! What's on my calendar?", "What's on my calendar?"),
        ("HEY HERMES tell me a joke", "tell me a joke"),
        ("Hermes, remind me about dinner", "remind me about dinner"),
        ("  Hey Hermes   what time is it  ", "what time is it"),
    ],
)
def test_detects_salutation_and_returns_remainder(transcript, expected):
    assert detect(transcript, PREFIXES) == expected


@pytest.mark.parametrize(
    "transcript",
    [
        "Add milk to the shopping list",
        "Remember that Hermes is the messenger god",
        "Hermetic seals are underrated",
        "",
        "   ",
    ],
)
def test_returns_none_without_leading_salutation(transcript):
    assert detect(transcript, PREFIXES) is None


def test_salutation_alone_returns_empty_remainder():
    assert detect("Hey Hermes.", PREFIXES) == ""


def test_longest_matching_prefix_wins():
    """With both 'hermes' and 'hey hermes' configured, the longer must match first
    so the remainder is not left containing the word 'hermes'."""
    assert detect("Hey Hermes what's up", ["hermes", "hey hermes"]) == "what's up"


def test_custom_assistant_name():
    assert detect("Okay Athena, lights on", ["okay athena"]) == "lights on"


def test_empty_prefix_list_never_matches():
    assert detect("Hey Hermes what's up", []) is None


def test_blank_prefixes_are_ignored():
    assert detect("Hey Hermes what's up", ["", "   "]) is None
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_salutation.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.salutation'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/salutation.py`:

```python
from __future__ import annotations

import re

_WORD = re.compile(r"\S+")
_PUNCTUATION = re.compile(r"[^\w\s]", re.UNICODE)


def _normalize_word(word: str) -> str:
    return _PUNCTUATION.sub("", word).lower()


def detect(transcript: str, prefixes: list[str]) -> str | None:
    """Return the text following a leading salutation, or None if absent.

    Speech-to-text output varies in casing and punctuation ("Hey Hermes," /
    "hey hermes." / "Hey, Hermes!"), so matching is done on normalized words
    while the remainder is sliced from the original text to preserve it exactly.
    An empty return means the user said only the salutation.
    """
    if not transcript or not prefixes:
        return None

    spans = [(m.group(0), m.start()) for m in _WORD.finditer(transcript)]
    if not spans:
        return None
    words = [_normalize_word(word) for word, _ in spans]

    longest_match = 0
    for prefix in prefixes:
        prefix_words = [w for w in (_normalize_word(p) for p in prefix.split()) if w]
        if not prefix_words or len(prefix_words) > len(words):
            continue
        if words[: len(prefix_words)] == prefix_words:
            longest_match = max(longest_match, len(prefix_words))

    if longest_match == 0:
        return None
    if longest_match >= len(spans):
        return ""
    return transcript[spans[longest_match][1] :].strip()
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_salutation.py -v`
Expected: 16 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/salutation.py bridge/tests/test_salutation.py
git commit -m "feat(bridge): add salutation prefix detection"
```

---

### Task 9: Speech providers

**Files:**
- Create: `bridge/src/htp_bridge/speech.py`
- Test: `bridge/tests/test_speech.py`

**Interfaces:**
- Consumes: `SpeechConfig` (Task 1).
- Produces: `Protocol SpeechProvider` with `async transcribe(wav_path: Path) -> str` and `async synthesize(text: str) -> bytes`; `class OpenAISpeechProvider`; `class SpeechError(Exception)`; `class FakeSpeechProvider` (used by later tests); `build_speech_provider(config) -> SpeechProvider`.

`synthesize` returns bytes rather than writing a file, so the pipeline decides where audio
lands and `AudioStorage` stays the only module that touches disk.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_speech.py`:

```python
import json

import httpx
import pytest

from htp_bridge.config import SpeechConfig
from htp_bridge.speech import (
    FakeSpeechProvider,
    OpenAISpeechProvider,
    SpeechError,
    build_speech_provider,
)


@pytest.fixture
def speech_config():
    return SpeechConfig(
        provider="openai",
        api_key="sk-test",
        stt_model="whisper-1",
        tts_model="tts-1",
        tts_voice="alloy",
        timeout_seconds=30,
        base_url="https://api.example.com/v1",
    )


def provider_with(speech_config, handler):
    transport = httpx.MockTransport(handler)
    return OpenAISpeechProvider(speech_config, transport=transport)


async def test_transcribe_posts_audio_and_returns_text(speech_config, tmp_path):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["url"] = str(request.url)
        seen["auth"] = request.headers.get("authorization")
        seen["body"] = request.content
        return httpx.Response(200, json={"text": "Add milk to the shopping list"})

    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")

    text = await provider_with(speech_config, handler).transcribe(wav)

    assert text == "Add milk to the shopping list"
    assert seen["url"] == "https://api.example.com/v1/audio/transcriptions"
    assert seen["auth"] == "Bearer sk-test"
    assert b"whisper-1" in seen["body"]


async def test_transcribe_strips_surrounding_whitespace(speech_config, tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")
    handler = lambda request: httpx.Response(200, json={"text": "  hello  "})
    assert await provider_with(speech_config, handler).transcribe(wav) == "hello"


async def test_transcribe_raises_speech_error_on_http_failure(speech_config, tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")
    handler = lambda request: httpx.Response(500, text="upstream exploded")

    with pytest.raises(SpeechError):
        await provider_with(speech_config, handler).transcribe(wav)


async def test_transcribe_raises_speech_error_on_timeout(speech_config, tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")

    def handler(request):
        raise httpx.ConnectTimeout("too slow")

    with pytest.raises(SpeechError):
        await provider_with(speech_config, handler).transcribe(wav)


async def test_synthesize_requests_wav_and_returns_bytes(speech_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["url"] = str(request.url)
        seen["payload"] = json.loads(request.content)
        return httpx.Response(200, content=b"RIFFreply")

    audio = await provider_with(speech_config, handler).synthesize("Added milk.")

    assert audio == b"RIFFreply"
    assert seen["url"] == "https://api.example.com/v1/audio/speech"
    assert seen["payload"]["response_format"] == "wav"
    assert seen["payload"]["voice"] == "alloy"
    assert seen["payload"]["input"] == "Added milk."


async def test_synthesize_raises_speech_error_on_http_failure(speech_config):
    handler = lambda request: httpx.Response(429, text="slow down")
    with pytest.raises(SpeechError):
        await provider_with(speech_config, handler).synthesize("hello")


def test_build_speech_provider_returns_openai_implementation(speech_config):
    assert isinstance(build_speech_provider(speech_config), OpenAISpeechProvider)


def test_build_speech_provider_rejects_unknown_provider(speech_config):
    config = SpeechConfig(**{**speech_config.__dict__, "provider": "acme"})
    with pytest.raises(SpeechError, match="acme"):
        build_speech_provider(config)


async def test_fake_provider_returns_scripted_values(tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"x")
    fake = FakeSpeechProvider(transcripts={"c-1": "hello there"})

    assert await fake.transcribe(wav) == "hello there"
    assert await fake.synthesize("reply") == b"RIFF-fake-reply"
    assert fake.synthesized == ["reply"]


async def test_fake_provider_can_raise(tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"x")
    fake = FakeSpeechProvider(transcribe_error=SpeechError("stt down"))
    with pytest.raises(SpeechError):
        await fake.transcribe(wav)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_speech.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.speech'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/speech.py`:

```python
from __future__ import annotations

from pathlib import Path
from typing import Protocol

import httpx

from htp_bridge.config import SpeechConfig


class SpeechError(Exception):
    """Raised when a speech provider fails or is misconfigured."""


class SpeechProvider(Protocol):
    async def transcribe(self, wav_path: Path) -> str: ...

    async def synthesize(self, text: str) -> bytes: ...


class OpenAISpeechProvider:
    """Speech-to-text and text-to-speech over the OpenAI audio endpoints.

    `transport` is injectable so tests exercise the real request construction
    without touching the network.
    """

    def __init__(self, config: SpeechConfig, transport: httpx.BaseTransport | None = None) -> None:
        self._config = config
        self._transport = transport

    def _client(self) -> httpx.AsyncClient:
        return httpx.AsyncClient(
            timeout=self._config.timeout_seconds,
            transport=self._transport,
            headers={"Authorization": f"Bearer {self._config.api_key}"},
        )

    async def transcribe(self, wav_path: Path) -> str:
        try:
            async with self._client() as client:
                response = await client.post(
                    f"{self._config.base_url}/audio/transcriptions",
                    files={"file": (wav_path.name, wav_path.read_bytes(), "audio/wav")},
                    data={"model": self._config.stt_model},
                )
                response.raise_for_status()
                return str(response.json().get("text", "")).strip()
        except (httpx.HTTPError, ValueError, KeyError) as exc:
            raise SpeechError(f"transcription failed: {exc}") from exc

    async def synthesize(self, text: str) -> bytes:
        try:
            async with self._client() as client:
                response = await client.post(
                    f"{self._config.base_url}/audio/speech",
                    json={
                        "model": self._config.tts_model,
                        "voice": self._config.tts_voice,
                        "input": text,
                        "response_format": "wav",
                    },
                )
                response.raise_for_status()
                return response.content
        except httpx.HTTPError as exc:
            raise SpeechError(f"synthesis failed: {exc}") from exc


class FakeSpeechProvider:
    """Deterministic provider for tests elsewhere in the suite."""

    def __init__(
        self,
        transcripts: dict[str, str] | None = None,
        default_transcript: str = "",
        transcribe_error: Exception | None = None,
        synthesize_error: Exception | None = None,
    ) -> None:
        self._transcripts = transcripts or {}
        self._default = default_transcript
        self._transcribe_error = transcribe_error
        self._synthesize_error = synthesize_error
        self.synthesized: list[str] = []

    async def transcribe(self, wav_path: Path) -> str:
        if self._transcribe_error:
            raise self._transcribe_error
        return self._transcripts.get(wav_path.stem, self._default)

    async def synthesize(self, text: str) -> bytes:
        if self._synthesize_error:
            raise self._synthesize_error
        self.synthesized.append(text)
        return b"RIFF-fake-reply"


def build_speech_provider(config: SpeechConfig) -> SpeechProvider:
    if config.provider == "openai":
        return OpenAISpeechProvider(config)
    raise SpeechError(f"unknown speech provider '{config.provider}'")
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_speech.py -v`
Expected: 10 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/speech.py bridge/tests/test_speech.py
git commit -m "feat(bridge): add speech provider interface and OpenAI implementation"
```

---

### Task 10: Hermes Agent client

**Files:**
- Create: `bridge/src/htp_bridge/agent.py`
- Test: `bridge/tests/test_agent.py`

**Interfaces:**
- Consumes: `AgentConfig` (Task 1).
- Produces: `class AgentClient` with `async converse(text, history) -> str` and `async ingest(text, recorded_at) -> None`; `class AgentError(Exception)`; `class FakeAgentClient`; constant `POCKET_TERMINAL_INSTRUCTIONS`.

Targets an OpenAI-compatible `/chat/completions` endpoint. Spec §11 item 1 flags that the
running Hermes Agent's exact shape must be confirmed; this module is the single place that
changes if it differs.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_agent.py`:

```python
import json

import httpx
import pytest

from htp_bridge.agent import POCKET_TERMINAL_INSTRUCTIONS, AgentClient, AgentError, FakeAgentClient
from htp_bridge.config import AgentConfig


@pytest.fixture
def agent_config():
    return AgentConfig(
        base_url="http://agent.local/v1", model="hermes-fast", api_key="", timeout_seconds=30
    )


def client_with(agent_config, handler):
    return AgentClient(agent_config, transport=httpx.MockTransport(handler))


def reply(content: str) -> httpx.Response:
    return httpx.Response(200, json={"choices": [{"message": {"content": content}}]})


async def test_converse_returns_reply_text(agent_config):
    client = client_with(agent_config, lambda request: reply("Dinner is at 7 PM."))
    assert await client.converse("what time is dinner", []) == "Dinner is at 7 PM."


async def test_converse_sends_terminal_instructions_as_system_message(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        seen["url"] = str(request.url)
        return reply("ok")

    await client_with(agent_config, handler).converse("hello", [])

    messages = seen["payload"]["messages"]
    assert seen["url"] == "http://agent.local/v1/chat/completions"
    assert messages[0]["role"] == "system"
    assert messages[0]["content"] == POCKET_TERMINAL_INSTRUCTIONS
    assert messages[-1] == {"role": "user", "content": "hello"}


async def test_converse_includes_history_in_order(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("ok")

    history = [("what time is dinner", "Dinner is at 7 PM."), ("and dessert", "Dessert is at 8 PM.")]
    await client_with(agent_config, handler).converse("thanks", history)

    roles = [m["role"] for m in seen["payload"]["messages"]]
    assert roles == ["system", "user", "assistant", "user", "assistant", "user"]
    assert seen["payload"]["messages"][1]["content"] == "what time is dinner"
    assert seen["payload"]["messages"][2]["content"] == "Dinner is at 7 PM."


async def test_converse_pins_configured_model(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("ok")

    await client_with(agent_config, handler).converse("hello", [])
    assert seen["payload"]["model"] == "hermes-fast"


async def test_converse_omits_model_when_unset(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("ok")

    config = AgentConfig(base_url=agent_config.base_url, model=None, api_key="", timeout_seconds=30)
    await AgentClient(config, transport=httpx.MockTransport(handler)).converse("hello", [])
    assert "model" not in seen["payload"]


async def test_converse_raises_on_http_failure(agent_config):
    client = client_with(agent_config, lambda request: httpx.Response(503, text="down"))
    with pytest.raises(AgentError):
        await client.converse("hello", [])


async def test_converse_raises_on_timeout(agent_config):
    def handler(request):
        raise httpx.ReadTimeout("too slow")

    with pytest.raises(AgentError):
        await client_with(agent_config, handler).converse("hello", [])


async def test_converse_raises_on_empty_reply(agent_config):
    client = client_with(agent_config, lambda request: reply("   "))
    with pytest.raises(AgentError, match="empty"):
        await client.converse("hello", [])


async def test_ingest_marks_message_as_a_captured_note(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("noted")

    await client_with(agent_config, handler).ingest("Add milk", recorded_at=1754300102)

    content = seen["payload"]["messages"][-1]["content"]
    assert "Add milk" in content
    assert "pocket terminal" in content.lower()


async def test_ingest_raises_on_failure_so_callers_can_retry(agent_config):
    client = client_with(agent_config, lambda request: httpx.Response(500, text="down"))
    with pytest.raises(AgentError):
        await client.ingest("Add milk", recorded_at=1)


async def test_fake_agent_records_calls_and_returns_scripted_reply():
    fake = FakeAgentClient(reply_text="Added milk to your shopping list.")

    assert await fake.converse("add milk", []) == "Added milk to your shopping list."
    await fake.ingest("a note", recorded_at=5)

    assert fake.conversations == [("add milk", [])]
    assert fake.ingested == [("a note", 5)]


async def test_fake_agent_can_raise():
    fake = FakeAgentClient(error=AgentError("agent down"))
    with pytest.raises(AgentError):
        await fake.converse("hello", [])
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_agent.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.agent'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/agent.py`:

```python
from __future__ import annotations

from typing import Any

import httpx

from htp_bridge.config import AgentConfig

POCKET_TERMINAL_INSTRUCTIONS = (
    "You are replying to a message captured on a pocket terminal and read aloud by "
    "text-to-speech. Reply in one or two short plain-text sentences. Use no markdown, "
    "no lists, and no preamble. Refer to times absolutely (for example \"at 10:00 AM\"), "
    "never relatively (\"in 15 minutes\"), because delivery may be delayed."
)


class AgentError(Exception):
    """Raised when Hermes Agent is unreachable or returns an unusable response."""


class AgentClient:
    def __init__(self, config: AgentConfig, transport: httpx.BaseTransport | None = None) -> None:
        self._config = config
        self._transport = transport

    async def converse(self, text: str, history: list[tuple[str, str]]) -> str:
        messages: list[dict[str, str]] = [
            {"role": "system", "content": POCKET_TERMINAL_INSTRUCTIONS}
        ]
        for user_text, assistant_text in history:
            messages.append({"role": "user", "content": user_text})
            messages.append({"role": "assistant", "content": assistant_text})
        messages.append({"role": "user", "content": text})

        data = await self._post(messages)
        try:
            content = str(data["choices"][0]["message"]["content"]).strip()
        except (KeyError, IndexError, TypeError) as exc:
            raise AgentError(f"unexpected agent response shape: {exc}") from exc
        if not content:
            raise AgentError("agent returned an empty reply")
        return content

    async def ingest(self, text: str, recorded_at: int | None) -> None:
        """Deliver a note to the agent for classification and storage.

        Raises on failure so the caller can retry; the device is not waiting.
        """
        stamp = f" at epoch {recorded_at}" if recorded_at else ""
        messages = [
            {
                "role": "user",
                "content": (
                    f"The following was captured as a note on the pocket terminal{stamp}. "
                    f"Handle it according to its content; no spoken reply is expected.\n\n{text}"
                ),
            }
        ]
        await self._post(messages)

    async def _post(self, messages: list[dict[str, str]]) -> dict[str, Any]:
        payload: dict[str, Any] = {"messages": messages}
        if self._config.model:
            payload["model"] = self._config.model
        headers = {}
        if self._config.api_key:
            headers["Authorization"] = f"Bearer {self._config.api_key}"
        try:
            async with httpx.AsyncClient(
                timeout=self._config.timeout_seconds, transport=self._transport, headers=headers
            ) as client:
                response = await client.post(
                    f"{self._config.base_url}/chat/completions", json=payload
                )
                response.raise_for_status()
                return response.json()
        except (httpx.HTTPError, ValueError) as exc:
            raise AgentError(f"agent request failed: {exc}") from exc


class FakeAgentClient:
    """Scriptable agent for tests elsewhere in the suite."""

    def __init__(self, reply_text: str = "Done.", error: Exception | None = None) -> None:
        self._reply = reply_text
        self._error = error
        self.conversations: list[tuple[str, list[tuple[str, str]]]] = []
        self.ingested: list[tuple[str, int | None]] = []

    async def converse(self, text: str, history: list[tuple[str, str]]) -> str:
        if self._error:
            raise self._error
        self.conversations.append((text, history))
        return self._reply

    async def ingest(self, text: str, recorded_at: int | None) -> None:
        if self._error:
            raise self._error
        self.ingested.append((text, recorded_at))
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_agent.py -v`
Expected: 12 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/agent.py bridge/tests/test_agent.py
git commit -m "feat(bridge): add Hermes Agent client with terminal reply instructions"
```

---

### Task 11: Capture pipeline

The state machine from §6.1. This task also delivers crash resume and the two sweeps that
back the reliability guarantees in §8.

**Files:**
- Create: `bridge/src/htp_bridge/pipeline.py`
- Test: `bridge/tests/test_pipeline.py`

**Interfaces:**
- Consumes: `CaptureStore` (Task 4), `AudioStorage` (Task 5), `NotificationStore` (Task 7), `salutation.detect` (Task 8), `SpeechProvider`/`SpeechError` (Task 9), `AgentClient`/`AgentError` (Task 10).
- Produces: `class Pipeline` with `async process(capture_id) -> None`, `async resume() -> int`, `async sweep_redirects() -> int`, `async sweep_ingestion() -> int`.

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_pipeline.py`:

```python
import itertools

import pytest

from htp_bridge.agent import AgentError, FakeAgentClient
from htp_bridge.captures import CaptureStore
from htp_bridge.config import StorageConfig
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import FakeSpeechProvider, SpeechError
from htp_bridge.storage import AudioStorage

PREFIXES = ["hey hermes", "hermes"]


@pytest.fixture
def parts(db, fake_clock, tmp_path):
    captures = CaptureStore(db, clock=fake_clock)
    storage = AudioStorage(
        StorageConfig(
            db_path=tmp_path / "htp.db",
            audio_dir=tmp_path / "audio",
            upload_retention_days=365,
            reply_retention_days=7,
        ),
        clock=fake_clock,
    )
    counter = itertools.count(1)
    notifications = NotificationStore(
        db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}"
    )
    return captures, storage, notifications


def build(parts, fake_clock, *, speech, agent, grace=90):
    captures, storage, notifications = parts
    conversations = itertools.count(1)
    return Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=speech,
        agent=agent,
        salutation_prefixes=PREFIXES,
        reply_grace_seconds=grace,
        clock=fake_clock,
        conversation_id_factory=lambda: f"v-{next(conversations)}",
    )


def upload(parts, capture_id, *, conversation_id=None):
    captures, storage, _ = parts
    captures.create(
        capture_id=capture_id,
        device_id="pocket-01",
        recorded_at=1,
        conversation_id=conversation_id,
    )
    storage.save_upload(capture_id, b"RIFFfake")


async def test_note_reaches_done_without_entering_processing(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk to the shopping list"})
    agent = FakeAgentClient()
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "done"
    assert capture.transcript == "Add milk to the shopping list"
    assert capture.reply_text is None
    assert agent.ingested == [("Add milk to the shopping list", 1)]
    assert agent.conversations == [], "a note must never open a conversation"
    assert speech.synthesized == [], "a note must never be synthesized"


async def test_salutation_routes_to_conversation_and_strips_the_prefix(parts, fake_clock):
    captures, storage, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, what's on my calendar?"})
    agent = FakeAgentClient(reply_text="You have one meeting at 10:00 AM.")
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "reply_ready"
    assert capture.transcript == "Hey Hermes, what's on my calendar?"
    assert capture.reply_text == "You have one meeting at 10:00 AM."
    assert capture.conversation_id == "v-1"
    assert agent.conversations[0][0] == "what's on my calendar?"
    assert storage.has_reply("c-1")
    assert agent.ingested == []


async def test_salutation_only_falls_back_to_the_full_transcript(parts, fake_clock):
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes"})
    agent = FakeAgentClient()
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    assert agent.conversations[0][0] == "Hey Hermes"


async def test_follow_up_reuses_conversation_and_sends_history(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(
        transcripts={"c-1": "Hey Hermes, what time is dinner?", "c-2": "Hey Hermes, and dessert?"}
    )
    agent = FakeAgentClient(reply_text="Dinner is at 7 PM.")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent)

    upload(parts, "c-1")
    await pipeline.process("c-1")
    upload(parts, "c-2", conversation_id="v-1")
    await pipeline.process("c-2")

    assert captures.get("c-2").conversation_id == "v-1"
    assert agent.conversations[1][1] == [("Hey Hermes, what time is dinner?", "Dinner is at 7 PM.")]


async def test_transcription_failure_marks_capture_failed(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcribe_error=SpeechError("stt down"))
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=FakeAgentClient()).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "failed"
    assert capture.error == "transcription_failed"


async def test_agent_failure_on_conversation_marks_capture_failed_but_keeps_transcript(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, hello"})
    upload(parts, "c-1")

    await build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    ).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "failed"
    assert capture.error == "agent_unavailable"
    assert capture.transcript == "Hey Hermes, hello", "the recording is still transcribed"


async def test_synthesis_failure_marks_capture_failed(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(
        transcripts={"c-1": "Hey Hermes, hello"}, synthesize_error=SpeechError("tts down")
    )
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=FakeAgentClient()).process("c-1")

    assert captures.get("c-1").error == "synthesis_failed"


async def test_ingestion_failure_leaves_note_done_and_flags_backlog(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    upload(parts, "c-1")

    await build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    ).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "done", "the device already confirmed and slept"
    assert capture.error == "ingest_failed"
    assert [c.id for c in captures.ingestion_backlog()] == ["c-1"]


async def test_sweep_ingestion_retries_and_clears_the_flag(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    upload(parts, "c-1")
    await build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    ).process("c-1")

    healthy = FakeAgentClient()
    retried = await build(parts, fake_clock, speech=speech, agent=healthy).sweep_ingestion()

    assert retried == 1
    assert captures.get("c-1").error is None
    assert healthy.ingested == [("Add milk", 1)]


async def test_process_is_a_noop_for_terminal_captures(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = FakeAgentClient()
    upload(parts, "c-1")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent)

    await pipeline.process("c-1")
    await pipeline.process("c-1")

    assert len(agent.ingested) == 1, "reprocessing must not deliver the note twice"


async def test_process_ignores_unknown_capture(parts, fake_clock):
    pipeline = build(parts, fake_clock, speech=FakeSpeechProvider(), agent=FakeAgentClient())
    await pipeline.process("c-nope")


async def test_resume_reprocesses_captures_left_mid_pipeline(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = FakeAgentClient()
    upload(parts, "c-1")
    captures.set_state("c-1", "transcribing")  # simulate a crash mid-stage

    resumed = await build(parts, fake_clock, speech=speech, agent=agent).resume()

    assert resumed == 1
    assert captures.get("c-1").state == "done"


async def test_sweep_redirects_queues_unclaimed_reply_as_notification(parts, fake_clock):
    captures, _, notifications = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, what's for dinner?"})
    agent = FakeAgentClient(reply_text="Lasagne at 7 PM.")
    upload(parts, "c-1")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent, grace=90)
    await pipeline.process("c-1")

    fake_clock.advance(91)
    redirected = await pipeline.sweep_redirects()

    assert redirected == 1
    assert [n.text for n in notifications.pending()] == ["Lasagne at 7 PM."]
    assert captures.get("c-1").redirected is True


async def test_sweep_redirects_leaves_fresh_and_downloaded_replies_alone(parts, fake_clock):
    captures, _, notifications = parts
    speech = FakeSpeechProvider(
        transcripts={"c-1": "Hey Hermes, hello", "c-2": "Hey Hermes, hello"}
    )
    agent = FakeAgentClient(reply_text="Hi.")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent, grace=90)
    upload(parts, "c-1")
    await pipeline.process("c-1")
    captures.mark_downloaded("c-1")
    fake_clock.advance(91)
    upload(parts, "c-2")
    await pipeline.process("c-2")

    assert await pipeline.sweep_redirects() == 0
    assert notifications.pending() == []


async def test_sweep_redirects_runs_once_per_capture(parts, fake_clock):
    _, _, notifications = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, hello"})
    pipeline = build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(reply_text="Hi."), grace=90
    )
    upload(parts, "c-1")
    await pipeline.process("c-1")
    fake_clock.advance(91)

    await pipeline.sweep_redirects()
    await pipeline.sweep_redirects()

    assert len(notifications.pending()) == 1
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_pipeline.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.pipeline'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/pipeline.py`:

```python
from __future__ import annotations

import logging
import time
import uuid
from typing import Callable

from htp_bridge import salutation
from htp_bridge.agent import AgentError
from htp_bridge.captures import INGEST_FAILED, TERMINAL_STATES, CaptureStore
from htp_bridge.notifications import NotificationStore
from htp_bridge.speech import SpeechError
from htp_bridge.storage import AudioStorage

log = logging.getLogger(__name__)


def _default_conversation_id() -> str:
    return f"v-{uuid.uuid4().hex[:8]}"


class Pipeline:
    """Moves a capture from stored audio to a terminal state.

    Every stage records its outcome in the capture row before the next begins, so
    a process that dies mid-pipeline can be resumed from durable state rather than
    memory.
    """

    def __init__(
        self,
        *,
        captures: CaptureStore,
        storage: AudioStorage,
        notifications: NotificationStore,
        speech,
        agent,
        salutation_prefixes: list[str],
        reply_grace_seconds: int = 90,
        clock: Callable[[], int] = lambda: int(time.time()),
        conversation_id_factory: Callable[[], str] = _default_conversation_id,
    ) -> None:
        self._captures = captures
        self._storage = storage
        self._notifications = notifications
        self._speech = speech
        self._agent = agent
        self._prefixes = salutation_prefixes
        self._grace = reply_grace_seconds
        self._clock = clock
        self._new_conversation_id = conversation_id_factory

    async def process(self, capture_id: str) -> None:
        capture = self._captures.get(capture_id)
        if capture is None or capture.state in TERMINAL_STATES:
            return

        self._captures.set_state(capture_id, "transcribing")
        try:
            transcript = await self._speech.transcribe(self._storage.upload_path(capture_id))
        except SpeechError:
            log.exception("capture %s: transcription failed", capture_id)
            self._captures.set_state(capture_id, "failed", error="transcription_failed")
            return

        self._captures.set_transcript(capture_id, transcript)
        remainder = salutation.detect(transcript, self._prefixes)

        if remainder is None:
            await self._finish_note(capture_id, transcript, capture.recorded_at)
        else:
            await self._answer(capture_id, remainder or transcript, capture.conversation_id)

    async def _finish_note(self, capture_id: str, transcript: str, recorded_at: int | None) -> None:
        """Mark the note done first: the device is waiting, the agent is not."""
        self._captures.set_state(capture_id, "done")
        try:
            await self._agent.ingest(transcript, recorded_at)
        except AgentError:
            log.exception("capture %s: agent ingestion failed, queued for retry", capture_id)
            self._captures.set_state(capture_id, "done", error=INGEST_FAILED)

    async def _answer(self, capture_id: str, prompt: str, conversation_id: str | None) -> None:
        self._captures.set_state(capture_id, "processing")
        conversation_id = conversation_id or self._new_conversation_id()
        self._captures.set_conversation(capture_id, conversation_id)
        history = self._captures.conversation_history(conversation_id, exclude_id=capture_id)

        try:
            reply = await self._agent.converse(prompt, history)
        except AgentError:
            log.exception("capture %s: agent unavailable", capture_id)
            self._captures.set_state(capture_id, "failed", error="agent_unavailable")
            return

        try:
            audio = await self._speech.synthesize(reply)
        except SpeechError:
            log.exception("capture %s: synthesis failed", capture_id)
            self._captures.set_state(capture_id, "failed", error="synthesis_failed")
            return

        self._storage.save_reply(capture_id, audio)
        self._captures.set_reply(capture_id, reply)

    async def resume(self) -> int:
        """Reprocess captures that were mid-pipeline when the process last stopped."""
        pending = self._captures.unfinished_ids()
        for capture_id in pending:
            await self.process(capture_id)
        return len(pending)

    async def sweep_redirects(self) -> int:
        """Move unclaimed replies into the notification queue.

        The device sleeps after its polling window, so a reply that was never
        downloaded would otherwise be lost. Sending it as a notification means the
        answer arrives on the next sync instead.
        """
        cutoff = self._clock() - self._grace
        redirected = 0
        for capture in self._captures.redirect_candidates(older_than=cutoff):
            if not capture.reply_text:
                continue
            self._notifications.enqueue(capture.reply_text, "normal")
            self._captures.mark_redirected(capture.id)
            redirected += 1
        return redirected

    async def sweep_ingestion(self) -> int:
        """Retry notes the agent refused or was unavailable for."""
        retried = 0
        for capture in self._captures.ingestion_backlog():
            if not capture.transcript:
                continue
            try:
                await self._agent.ingest(capture.transcript, capture.recorded_at)
            except AgentError:
                log.warning("capture %s: agent still unavailable for ingestion", capture.id)
                continue
            self._captures.set_state(capture.id, "done", error=None)
            retried += 1
        return retried
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_pipeline.py -v`
Expected: 15 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/pipeline.py bridge/tests/test_pipeline.py
git commit -m "feat(bridge): add capture pipeline with resume and redirect sweeps"
```

---

### Task 12: HTTP API

All seven endpoints from §4, plus an unauthenticated `/healthz`.

**Files:**
- Create: `bridge/src/htp_bridge/api.py`
- Test: `bridge/tests/test_api_captures.py`
- Test: `bridge/tests/test_api_state.py`

**Interfaces:**
- Consumes: every store and the pipeline (Tasks 1–11).
- Produces: dataclass `Deps`; `create_app(deps: Deps, lifespan=None) -> FastAPI`. The
  `lifespan` parameter is unused by these tests but is what Task 14 uses to start the
  background sweeps and the mounted MCP app.

- [ ] **Step 1: Write the app fixture**

Create an empty `bridge/tests/__init__.py` so the shared constants below are importable as
`tests.conftest`:

```bash
touch bridge/tests/__init__.py
```

Append the following to `bridge/tests/conftest.py`, merging the import lines with the ones
already at the top of that file rather than duplicating them:

```python
import itertools

import pytest
from fastapi.testclient import TestClient

from htp_bridge.agent import FakeAgentClient
from htp_bridge.api import Deps, create_app
from htp_bridge.captures import CaptureStore
from htp_bridge.config import DashboardConfig, ServerConfig, StorageConfig
from htp_bridge.dashboard import DashboardStore
from htp_bridge.devices import DeviceRegistry
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import FakeSpeechProvider
from htp_bridge.storage import AudioStorage

TOKEN = "tok-aaaaaaaaaaaaaaaaaaaa"
AUTH = {"Authorization": f"Bearer {TOKEN}"}


@pytest.fixture
def app_context(db, fake_clock, tmp_path, device_config):
    storage_config = StorageConfig(
        db_path=tmp_path / "htp.db",
        audio_dir=tmp_path / "audio",
        upload_retention_days=365,
        reply_retention_days=7,
    )
    captures = CaptureStore(db, clock=fake_clock)
    storage = AudioStorage(storage_config, clock=fake_clock)
    counter = itertools.count(1)
    notifications = NotificationStore(db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}")
    dashboard = DashboardStore(
        db, DashboardConfig(max_items=32, max_text_chars=40), clock=fake_clock
    )
    devices = DeviceRegistry(db, [device_config], clock=fake_clock)
    speech = FakeSpeechProvider(default_transcript="Add milk to the shopping list")
    agent = FakeAgentClient()
    conversations = itertools.count(1)
    pipeline = Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=speech,
        agent=agent,
        salutation_prefixes=["hey hermes", "hermes"],
        clock=fake_clock,
        conversation_id_factory=lambda: f"v-{next(conversations)}",
    )
    deps = Deps(
        server=ServerConfig(
            host="127.0.0.1", port=8787, max_upload_bytes=1000, sync_interval_seconds=600
        ),
        captures=captures,
        storage=storage,
        dashboard=dashboard,
        notifications=notifications,
        devices=devices,
        pipeline=pipeline,
        agent=agent,
        clock=fake_clock,
    )
    client = TestClient(create_app(deps))
    return {
        "client": client,
        "captures": captures,
        "storage": storage,
        "dashboard": dashboard,
        "notifications": notifications,
        "devices": devices,
        "agent": agent,
        "speech": speech,
        "clock": fake_clock,
    }


@pytest.fixture
def client(app_context):
    return app_context["client"]
```

- [ ] **Step 2: Write the failing capture-endpoint tests**

Write `bridge/tests/test_api_captures.py`:

```python
from tests.conftest import AUTH

WAV = b"RIFF" + b"\x00" * 64


def upload(client, capture_id, *, body=WAV, headers=None, mode="auto"):
    request_headers = {
        **AUTH,
        "Content-Type": "audio/wav",
        "X-Capture-Id": capture_id,
        "X-Capture-Mode": mode,
        "X-Recorded-At": "1754300102",
        "X-Battery": "78",
    }
    request_headers.update(headers or {})
    return client.post("/htp/v1/captures", content=body, headers=request_headers)


def test_upload_accepts_recording_and_returns_received(client):
    response = upload(client, "c-1")
    assert response.status_code == 200
    assert response.json()["id"] == "c-1"
    assert response.json()["state"] in {"received", "done"}


def test_upload_stores_the_wav(client, app_context):
    upload(client, "c-1")
    assert app_context["storage"].upload_path("c-1").read_bytes() == WAV


def test_upload_records_battery_telemetry(client, app_context):
    upload(client, "c-1")
    assert app_context["devices"].statuses()[0].battery == 78


def test_upload_runs_the_pipeline(client, app_context):
    upload(client, "c-1")
    assert app_context["captures"].get("c-1").state == "done"
    assert app_context["agent"].ingested == [("Add milk to the shopping list", 1754300102)]


def test_repeat_upload_does_not_reprocess(client, app_context):
    upload(client, "c-1")
    upload(client, "c-1")
    assert len(app_context["agent"].ingested) == 1


def test_upload_without_token_is_unauthorized(client):
    response = client.post(
        "/htp/v1/captures", content=WAV, headers={"X-Capture-Id": "c-1", "Content-Type": "audio/wav"}
    )
    assert response.status_code == 401
    assert response.json() == {"error": "unauthorized"}


def test_upload_with_wrong_token_is_unauthorized(client):
    response = upload(client, "c-1", headers={"Authorization": "Bearer nope"})
    assert response.status_code == 401


def test_upload_rejects_missing_capture_id(client):
    response = client.post(
        "/htp/v1/captures", content=WAV, headers={**AUTH, "Content-Type": "audio/wav"}
    )
    assert response.status_code == 400
    assert response.json() == {"error": "missing_capture_id"}


def test_upload_rejects_unsafe_capture_id(client):
    response = upload(client, "../escape")
    assert response.status_code == 400
    assert response.json() == {"error": "invalid_capture_id"}


def test_upload_rejects_empty_body(client):
    response = upload(client, "c-1", body=b"")
    assert response.status_code == 400
    assert response.json() == {"error": "empty_capture"}


def test_upload_rejects_oversized_body(client):
    response = upload(client, "c-1", body=b"x" * 1001)
    assert response.status_code == 413
    assert response.json() == {"error": "capture_too_large"}


def test_status_poll_returns_requested_captures(client, app_context):
    upload(client, "c-1")
    response = client.get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH)

    body = response.json()
    assert body["captures"][0]["id"] == "c-1"
    assert body["captures"][0]["state"] == "done"
    assert body["captures"][0]["transcript"] == "Add milk to the shopping list"
    assert body["server_time"] == app_context["clock"].now


def test_status_poll_reports_unknown_ids(client):
    response = client.get("/htp/v1/captures", params={"ids": "c-missing"}, headers=AUTH)
    assert response.json()["captures"] == [{"id": "c-missing", "state": "unknown"}]


def test_status_poll_preserves_request_order(client):
    upload(client, "c-1")
    upload(client, "c-2")
    response = client.get("/htp/v1/captures", params={"ids": "c-2,c-missing,c-1"}, headers=AUTH)
    assert [c["id"] for c in response.json()["captures"]] == ["c-2", "c-missing", "c-1"]


def test_status_poll_includes_error_for_failed_captures(client, app_context):
    upload(client, "c-1")
    app_context["captures"].set_state("c-1", "failed", error="transcription_failed")
    response = client.get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH)
    assert response.json()["captures"][0]["error"] == "transcription_failed"


def test_status_poll_rejects_too_many_ids(client):
    response = client.get(
        "/htp/v1/captures", params={"ids": ",".join(f"c-{n}" for n in range(65))}, headers=AUTH
    )
    assert response.status_code == 400
    assert response.json() == {"error": "too_many_ids"}


def test_status_poll_with_no_ids_returns_empty_list(client):
    response = client.get("/htp/v1/captures", params={"ids": ""}, headers=AUTH)
    assert response.json()["captures"] == []


def test_reply_download_returns_audio_and_marks_downloaded(client, app_context):
    app_context["captures"].create(
        capture_id="c-9", device_id="pocket-01", recorded_at=1, conversation_id=None
    )
    app_context["storage"].save_reply("c-9", b"RIFFreply")
    app_context["captures"].set_reply("c-9", "Lasagne at 7 PM.")

    response = client.get("/htp/v1/captures/c-9/reply.wav", headers=AUTH)

    assert response.status_code == 200
    assert response.content == b"RIFFreply"
    assert response.headers["content-type"] == "audio/wav"
    assert app_context["captures"].get("c-9").reply_downloaded_at is not None


def test_reply_download_404s_when_absent(client):
    response = client.get("/htp/v1/captures/c-nope/reply.wav", headers=AUTH)
    assert response.status_code == 404
    assert response.json() == {"error": "reply_not_found"}


def test_conversation_id_is_returned_and_accepted(client, app_context):
    app_context["speech"]._default = "Hey Hermes, what's for dinner?"
    upload(client, "c-1")

    status = client.get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH).json()
    conversation_id = status["captures"][0]["conversation_id"]
    assert conversation_id == "v-1"

    upload(client, "c-2", headers={"X-Conversation-Id": conversation_id})
    assert app_context["captures"].get("c-2").conversation_id == "v-1"
```

- [ ] **Step 3: Write the failing dashboard/notification tests**

Write `bridge/tests/test_api_state.py`:

```python
from tests.conftest import AUTH


def test_healthz_needs_no_token(client):
    response = client.get("/healthz")
    assert response.status_code == 200
    assert response.json()["status"] == "ok"


def test_dashboard_is_empty_before_first_publish(client):
    body = client.get("/htp/v1/dashboard", headers=AUTH).json()
    assert body["items"] == []
    assert body["rev"] == "0"


def test_dashboard_returns_published_items(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])

    body = client.get("/htp/v1/dashboard", headers=AUTH).json()

    assert body["title"] == "Today"
    assert body["items"] == [{"id": "t-1", "text": "Buy milk", "done": False}]
    assert body["server_time"] == app_context["clock"].now


def test_dashboard_includes_style_only_when_set(client, app_context):
    app_context["dashboard"].publish(
        "Today",
        [
            {"id": "t-1", "text": "Plain", "done": False},
            {"id": "t-2", "text": "Dim", "done": True, "style": "dim"},
        ],
    )
    items = client.get("/htp/v1/dashboard", headers=AUTH).json()["items"]
    assert "style" not in items[0]
    assert items[1]["style"] == "dim"


def test_dashboard_returns_unchanged_for_matching_revision(client, app_context):
    rev = app_context["dashboard"].publish(
        "Today", [{"id": "t-1", "text": "Buy milk", "done": False}]
    )

    body = client.get("/htp/v1/dashboard", params={"rev": rev}, headers=AUTH).json()

    assert body == {"rev": rev, "unchanged": True, "server_time": app_context["clock"].now}


def test_dashboard_returns_full_body_for_stale_revision(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    body = client.get("/htp/v1/dashboard", params={"rev": "stale123"}, headers=AUTH).json()
    assert "items" in body


def test_dashboard_requires_a_token(client):
    assert client.get("/htp/v1/dashboard").status_code == 401


def test_complete_marks_item_done_and_returns_new_rev(client, app_context):
    old = app_context["dashboard"].publish(
        "Today", [{"id": "t-1", "text": "Buy milk", "done": False}]
    )

    response = client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH)

    assert response.status_code == 200
    assert response.json()["ok"] is True
    assert response.json()["rev"] != old
    assert app_context["dashboard"].current().items[0].done is True


def test_complete_tells_the_agent(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH)

    message = app_context["agent"].ingested[-1][0]
    assert "t-1" in message
    assert "Buy milk" in message


def test_complete_is_idempotent(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    first = client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH).json()
    second = client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH).json()
    assert first["rev"] == second["rev"]


def test_complete_404s_for_unknown_item(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    response = client.post("/htp/v1/complete", json={"item_id": "t-nope"}, headers=AUTH)
    assert response.status_code == 404
    assert response.json() == {"error": "unknown_item"}


def test_complete_rejects_missing_item_id(client):
    response = client.post("/htp/v1/complete", json={}, headers=AUTH)
    assert response.status_code == 400
    assert response.json() == {"error": "missing_item_id"}


def test_notifications_returns_pending_queue(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM", "urgent")

    body = client.get("/htp/v1/notifications", headers=AUTH).json()

    assert body["notifications"] == [
        {
            "id": "n-1",
            "text": "Meeting with Alex at 10:00 AM",
            "priority": "urgent",
            "created": app_context["clock"].now,
        }
    ]


def test_notifications_redeliver_until_acknowledged(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM")
    assert len(client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"]) == 1
    assert len(client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"]) == 1


def test_ack_removes_notifications(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM")

    response = client.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH)

    assert response.json() == {"ok": True, "acked": 1}
    assert client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"] == []


def test_ack_with_unknown_ids_is_accepted(client):
    response = client.post("/htp/v1/notifications/ack", json={"ids": ["n-nope"]}, headers=AUTH)
    assert response.json() == {"ok": True, "acked": 0}


def test_ack_rejects_missing_ids_field(client):
    response = client.post("/htp/v1/notifications/ack", json={}, headers=AUTH)
    assert response.status_code == 400
    assert response.json() == {"error": "missing_ids"}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cd bridge && python -m pytest tests/test_api_captures.py tests/test_api_state.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.api'`

- [ ] **Step 5: Write the implementation**

Write `bridge/src/htp_bridge/api.py`:

```python
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Callable

from fastapi import BackgroundTasks, Depends, FastAPI, Header, Request
from fastapi.responses import FileResponse, JSONResponse

from htp_bridge.agent import AgentError
from htp_bridge.captures import CaptureStore
from htp_bridge.config import DeviceConfig, ServerConfig
from htp_bridge.dashboard import DashboardStore
from htp_bridge.devices import DeviceRegistry
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.storage import AudioStorage, is_valid_capture_id

MAX_STATUS_IDS = 64
EMPTY_REVISION = "0"


@dataclass
class Deps:
    server: ServerConfig
    captures: CaptureStore
    storage: AudioStorage
    dashboard: DashboardStore
    notifications: NotificationStore
    devices: DeviceRegistry
    pipeline: Pipeline
    agent: Any
    clock: Callable[[], int] = field(default=lambda: int(time.time()))


class HTPError(Exception):
    def __init__(self, status_code: int, slug: str) -> None:
        self.status_code = status_code
        self.slug = slug


def _int_or_none(value: str | None) -> int | None:
    try:
        return int(value) if value is not None else None
    except ValueError:
        return None


def create_app(deps: Deps, lifespan=None) -> FastAPI:
    app = FastAPI(title="HTP Bridge", docs_url=None, redoc_url=None, lifespan=lifespan)

    @app.exception_handler(HTPError)
    async def _htp_error(request: Request, exc: HTPError) -> JSONResponse:
        return JSONResponse(status_code=exc.status_code, content={"error": exc.slug})

    def authenticate(
        authorization: str | None = Header(default=None),
        x_battery: str | None = Header(default=None),
    ) -> DeviceConfig:
        token = ""
        if authorization and authorization.lower().startswith("bearer "):
            token = authorization[7:].strip()
        device = deps.devices.authenticate(token)
        if device is None:
            raise HTPError(401, "unauthorized")
        deps.devices.record_telemetry(device.id, _int_or_none(x_battery))
        return device

    @app.get("/healthz")
    async def healthz() -> dict[str, Any]:
        return {
            "status": "ok",
            "server_time": deps.clock(),
            "pipeline_backlog": len(deps.captures.unfinished_ids()),
            "ingestion_backlog": len(deps.captures.ingestion_backlog()),
            "devices": [
                {"device_id": s.device_id, "battery": s.battery, "last_seen": s.last_seen}
                for s in deps.devices.statuses()
            ],
        }

    @app.post("/htp/v1/captures")
    async def upload_capture(
        request: Request,
        background: BackgroundTasks,
        device: DeviceConfig = Depends(authenticate),
        x_capture_id: str | None = Header(default=None),
        x_recorded_at: str | None = Header(default=None),
        x_conversation_id: str | None = Header(default=None),
    ) -> dict[str, str]:
        if not x_capture_id:
            raise HTPError(400, "missing_capture_id")
        if not is_valid_capture_id(x_capture_id):
            raise HTPError(400, "invalid_capture_id")

        body = await request.body()
        if not body:
            raise HTPError(400, "empty_capture")
        if len(body) > deps.server.max_upload_bytes:
            raise HTPError(413, "capture_too_large")

        capture, created = deps.captures.create(
            capture_id=x_capture_id,
            device_id=device.id,
            recorded_at=_int_or_none(x_recorded_at),
            conversation_id=x_conversation_id or None,
        )
        if created:
            deps.storage.save_upload(x_capture_id, body)
            background.add_task(deps.pipeline.process, x_capture_id)
            return {"id": x_capture_id, "state": "received"}
        return {"id": capture.id, "state": capture.state}

    @app.get("/htp/v1/captures")
    async def capture_status(
        ids: str = "", device: DeviceConfig = Depends(authenticate)
    ) -> dict[str, Any]:
        requested = [part for part in (ids or "").split(",") if part]
        if len(requested) > MAX_STATUS_IDS:
            raise HTPError(400, "too_many_ids")

        found = {capture.id: capture for capture in deps.captures.get_many(requested)}
        entries: list[dict[str, Any]] = []
        for capture_id in requested:
            capture = found.get(capture_id)
            if capture is None:
                entries.append({"id": capture_id, "state": "unknown"})
                continue
            entry: dict[str, Any] = {"id": capture.id, "state": capture.state}
            if capture.transcript is not None:
                entry["transcript"] = capture.transcript
            if capture.conversation_id:
                entry["conversation_id"] = capture.conversation_id
            if capture.error:
                entry["error"] = capture.error
            entries.append(entry)
        return {"server_time": deps.clock(), "captures": entries}

    @app.get("/htp/v1/captures/{capture_id}/reply.wav")
    async def reply_audio(
        capture_id: str, device: DeviceConfig = Depends(authenticate)
    ) -> FileResponse:
        if not is_valid_capture_id(capture_id) or not deps.storage.has_reply(capture_id):
            raise HTPError(404, "reply_not_found")
        deps.captures.mark_downloaded(capture_id)
        return FileResponse(deps.storage.reply_path(capture_id), media_type="audio/wav")

    @app.get("/htp/v1/dashboard")
    async def dashboard(
        rev: str = "", device: DeviceConfig = Depends(authenticate)
    ) -> dict[str, Any]:
        snapshot = deps.dashboard.current()
        now = deps.clock()
        if snapshot is None:
            return {"rev": EMPTY_REVISION, "server_time": now, "title": "", "items": []}
        if rev and rev == snapshot.rev:
            return {"rev": snapshot.rev, "unchanged": True, "server_time": now}

        items: list[dict[str, Any]] = []
        for item in snapshot.items:
            entry: dict[str, Any] = {"id": item.id, "text": item.text, "done": item.done}
            if item.style:
                entry["style"] = item.style
            items.append(entry)
        return {"rev": snapshot.rev, "server_time": now, "title": snapshot.title, "items": items}

    @app.post("/htp/v1/complete")
    async def complete(
        payload: dict[str, Any],
        background: BackgroundTasks,
        device: DeviceConfig = Depends(authenticate),
    ) -> dict[str, Any]:
        item_id = str(payload.get("item_id") or "").strip()
        if not item_id:
            raise HTPError(400, "missing_item_id")

        snapshot = deps.dashboard.current()
        item = next((i for i in snapshot.items if i.id == item_id), None) if snapshot else None
        if item is None:
            raise HTPError(404, "unknown_item")

        rev = deps.dashboard.complete(item_id)
        background.add_task(_notify_completion, deps, item_id, item.text)
        return {"ok": True, "rev": rev}

    @app.get("/htp/v1/notifications")
    async def notifications(device: DeviceConfig = Depends(authenticate)) -> dict[str, Any]:
        return {
            "server_time": deps.clock(),
            "notifications": [
                {
                    "id": n.id,
                    "text": n.text,
                    "priority": n.priority,
                    "created": n.created_at,
                }
                for n in deps.notifications.pending()
            ],
        }

    @app.post("/htp/v1/notifications/ack")
    async def acknowledge(
        payload: dict[str, Any], device: DeviceConfig = Depends(authenticate)
    ) -> dict[str, Any]:
        if "ids" not in payload or not isinstance(payload["ids"], list):
            raise HTPError(400, "missing_ids")
        acked = deps.notifications.ack([str(i) for i in payload["ids"]])
        return {"ok": True, "acked": acked}

    return app


async def _notify_completion(deps: Deps, item_id: str, text: str) -> None:
    """Tell the agent about a completion. Best effort: the snapshot already
    reflects it, and the agent's next publish is authoritative anyway."""
    try:
        await deps.agent.ingest(
            f"The user marked the dashboard item '{text}' (id {item_id}) complete "
            f"on the pocket terminal.",
            None,
        )
    except AgentError:
        pass
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd bridge && python -m pytest tests/test_api_captures.py tests/test_api_state.py -v`
Expected: 20 passed in `test_api_captures.py`, 17 passed in `test_api_state.py`

Note: `TestClient` runs background tasks synchronously once the response is produced, so
`test_upload_runs_the_pipeline` observes the finished state without extra waiting.

- [ ] **Step 7: Commit**

```bash
git add bridge/src/htp_bridge/api.py bridge/tests/__init__.py bridge/tests/conftest.py bridge/tests/test_api_captures.py bridge/tests/test_api_state.py
git commit -m "feat(bridge): add HTP endpoints with bearer auth"
```

---

### Task 13: MCP server for Hermes Agent

**Files:**
- Create: `bridge/src/htp_bridge/mcp_server.py`
- Test: `bridge/tests/test_mcp_server.py`

**Interfaces:**
- Consumes: `DashboardStore` (Task 6), `NotificationStore` (Task 7), `DeviceRegistry` (Task 3).
- Produces: `class HermesTools` with `publish_dashboard()`, `queue_notification()`, `get_device_status()`; `create_mcp_server(tools, name="htp-bridge") -> FastMCP`.

The tool logic lives in `HermesTools` as plain methods so it is testable without speaking
MCP; `create_mcp_server` only registers them. The docstrings are load-bearing — they are
how Hermes Agent learns the authoring rules, especially absolute time references (§5.7).

- [ ] **Step 1: Write the failing test**

Write `bridge/tests/test_mcp_server.py`:

```python
import itertools

import pytest

from htp_bridge.config import DashboardConfig
from htp_bridge.dashboard import DashboardStore
from htp_bridge.devices import DeviceRegistry
from htp_bridge.mcp_server import HermesTools, create_mcp_server
from htp_bridge.notifications import NotificationStore


@pytest.fixture
def tools(db, fake_clock, device_config):
    counter = itertools.count(1)
    return HermesTools(
        dashboard=DashboardStore(
            db, DashboardConfig(max_items=32, max_text_chars=40), clock=fake_clock
        ),
        notifications=NotificationStore(db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}"),
        devices=DeviceRegistry(db, [device_config], clock=fake_clock),
    )


def test_publish_dashboard_returns_revision_and_count(tools):
    result = tools.publish_dashboard("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    assert result["item_count"] == 1
    assert len(result["rev"]) == 8


def test_publish_dashboard_replaces_previous_list(tools):
    tools.publish_dashboard("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    tools.publish_dashboard("Today", [{"id": "t-2", "text": "Call dentist", "done": False}])
    assert tools.dashboard.current().items[0].id == "t-2"


def test_publish_dashboard_accepts_an_empty_list(tools):
    result = tools.publish_dashboard("Today", [])
    assert result["item_count"] == 0


def test_queue_notification_returns_id(tools):
    result = tools.queue_notification("Meeting with Alex at 10:00 AM", "urgent")
    assert result["id"] == "n-1"
    assert tools.notifications.pending()[0].priority == "urgent"


def test_queue_notification_defaults_to_normal_priority(tools):
    tools.queue_notification("A reminder")
    assert tools.notifications.pending()[0].priority == "normal"


def test_queue_notification_rejects_empty_text(tools):
    with pytest.raises(ValueError, match="empty"):
        tools.queue_notification("   ")


def test_get_device_status_reports_battery_and_last_seen(tools, fake_clock):
    tools.devices.record_telemetry("pocket-01", 78)
    status = tools.get_device_status()[0]
    assert status == {"device_id": "pocket-01", "battery": 78, "last_seen": fake_clock.now}


def test_get_device_status_is_empty_before_any_contact(tools):
    assert tools.get_device_status() == []


def test_queue_notification_docstring_states_the_absolute_time_rule(tools):
    assert "absolute" in HermesTools.queue_notification.__doc__.lower()


async def test_mcp_server_registers_the_three_tools(tools):
    server = create_mcp_server(tools)
    names = {tool.name for tool in await server.list_tools()}
    assert names == {"publish_dashboard", "queue_notification", "get_device_status"}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd bridge && python -m pytest tests/test_mcp_server.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.mcp_server'`

- [ ] **Step 3: Write the implementation**

Write `bridge/src/htp_bridge/mcp_server.py`:

```python
from __future__ import annotations

from typing import Any

from mcp.server.fastmcp import FastMCP

from htp_bridge.dashboard import DashboardStore
from htp_bridge.devices import DeviceRegistry
from htp_bridge.notifications import NotificationStore


class HermesTools:
    """The bridge's writable surface for Hermes Agent.

    Hermes Agent's memory is the source of truth; these tools publish a
    materialized copy that terminal devices can poll without waking the agent.
    """

    def __init__(
        self,
        dashboard: DashboardStore,
        notifications: NotificationStore,
        devices: DeviceRegistry,
    ) -> None:
        self.dashboard = dashboard
        self.notifications = notifications
        self.devices = devices

    def publish_dashboard(self, title: str, items: list[dict[str, Any]]) -> dict[str, Any]:
        """Replace the list shown on pocket terminals.

        Call this whenever the task list changes. The whole list is replaced, so
        send the complete current state, not a delta.

        Args:
            title: Short heading, e.g. "Today".
            items: Objects with "id" (stable identifier used when the user marks
                the item complete), "text" (short, truncated for a small display),
                "done" (boolean), and optional "style" of "bold" or "dim".
        """
        rev = self.dashboard.publish(title, items)
        return {"rev": rev, "item_count": len(self.dashboard.current().items)}

    def queue_notification(self, text: str, priority: str = "normal") -> dict[str, Any]:
        """Queue a message for delivery to pocket terminals.

        Devices poll on an interval and sleep in between, so delivery may be
        delayed by several minutes. Write the text with absolute time references
        ("Meeting with Alex at 10:00 AM"), never relative ones ("in 15 minutes"),
        which would be wrong by the time the user reads them.

        Args:
            text: One short sentence suitable for a small monochrome display.
            priority: "urgent" to chime on arrival, "normal" to display silently.
        """
        if not text or not text.strip():
            raise ValueError("notification text must not be empty")
        return {"id": self.notifications.enqueue(text.strip(), priority)}

    def get_device_status(self) -> list[dict[str, Any]]:
        """Report each terminal's last known battery level and contact time.

        `last_seen` is Unix epoch seconds. A device that has not been seen for
        many hours is probably out of range or discharged.
        """
        return [
            {"device_id": s.device_id, "battery": s.battery, "last_seen": s.last_seen}
            for s in self.devices.statuses()
        ]


def create_mcp_server(tools: HermesTools, name: str = "htp-bridge") -> FastMCP:
    server = FastMCP(name)
    server.add_tool(tools.publish_dashboard)
    server.add_tool(tools.queue_notification)
    server.add_tool(tools.get_device_status)
    return server
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd bridge && python -m pytest tests/test_mcp_server.py -v`
Expected: 10 passed

- [ ] **Step 5: Commit**

```bash
git add bridge/src/htp_bridge/mcp_server.py bridge/tests/test_mcp_server.py
git commit -m "feat(bridge): add MCP tools for dashboard and notifications"
```

---

### Task 14: Entrypoint, mock mode, and deployment

**Files:**
- Create: `bridge/src/htp_bridge/main.py`
- Create: `bridge/src/htp_bridge/mock.py`
- Create: `bridge/deploy/htp-bridge.service`
- Create: `bridge/deploy/Caddyfile.example`
- Create: `bridge/README.md`
- Test: `bridge/tests/test_main.py`
- Test: `bridge/tests/test_mock.py`

**Interfaces:**
- Consumes: everything.
- Produces: `build_deps(config) -> tuple[Deps, HermesTools]`, `create_full_app(config) -> FastAPI`, `main(argv=None) -> int`, `create_mock_app() -> FastAPI`.

- [ ] **Step 1: Write the failing tests**

Write `bridge/tests/test_mock.py`:

```python
from fastapi.testclient import TestClient

from htp_bridge.mock import create_mock_app

AUTH = {"Authorization": "Bearer any-token-works-in-mock"}


def client():
    return TestClient(create_mock_app())


def test_mock_accepts_any_token():
    response = client().get("/htp/v1/dashboard", headers={"Authorization": "Bearer whatever"})
    assert response.status_code == 200


def test_mock_upload_returns_received():
    response = client().post(
        "/htp/v1/captures", content=b"RIFF", headers={**AUTH, "X-Capture-Id": "c-1"}
    )
    assert response.json() == {"id": "c-1", "state": "received"}


def test_mock_status_reports_a_ready_reply():
    body = client().get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH).json()
    entry = body["captures"][0]
    assert entry["state"] == "reply_ready"
    assert entry["transcript"]
    assert "server_time" in body


def test_mock_reply_returns_wav_bytes():
    response = client().get("/htp/v1/captures/c-1/reply.wav", headers=AUTH)
    assert response.status_code == 200
    assert response.content.startswith(b"RIFF")


def test_mock_dashboard_has_items_and_stable_rev():
    first = client().get("/htp/v1/dashboard", headers=AUTH).json()
    assert first["items"]
    second = client().get("/htp/v1/dashboard", params={"rev": first["rev"]}, headers=AUTH).json()
    assert second["unchanged"] is True


def test_mock_notifications_and_ack():
    c = client()
    body = c.get("/htp/v1/notifications", headers=AUTH).json()
    assert body["notifications"][0]["priority"] in {"normal", "urgent"}
    assert c.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH).json()["ok"]


def test_mock_complete_returns_ok():
    assert client().post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH).json()["ok"]
```

Write `bridge/tests/test_main.py`:

```python
import pytest
from fastapi.testclient import TestClient

from htp_bridge.config import load_config
from htp_bridge.main import build_deps, create_full_app, main

CONFIG_TEMPLATE = """
[server]
host = "127.0.0.1"
port = 8787
max_upload_bytes = 4200000
sync_interval_seconds = 600

[[devices]]
id = "pocket-01"
token = "tok-aaaaaaaaaaaaaaaaaaaa"

[salutations]
prefixes = ["hey hermes"]

[agent]
base_url = "http://127.0.0.1:8080/v1"
model = "hermes-fast"
api_key = ""
timeout_seconds = 60

[speech]
provider = "openai"
api_key = "sk-test"
stt_model = "whisper-1"
tts_model = "tts-1"
tts_voice = "alloy"
timeout_seconds = 60

[storage]
db_path = "{db}"
audio_dir = "{audio}"
upload_retention_days = 365
reply_retention_days = 7

[dashboard]
max_items = 32
max_text_chars = 40
"""


@pytest.fixture
def config_path(tmp_path):
    path = tmp_path / "config.toml"
    path.write_text(
        CONFIG_TEMPLATE.format(db=tmp_path / "htp.db", audio=tmp_path / "audio")
    )
    return path


def test_build_deps_wires_every_component(config_path):
    deps, tools = build_deps(load_config(config_path))
    assert deps.devices.authenticate("tok-aaaaaaaaaaaaaaaaaaaa").id == "pocket-01"
    assert tools.dashboard is deps.dashboard
    assert tools.notifications is deps.notifications


def test_full_app_serves_healthz_and_requires_auth_on_htp(config_path):
    app = create_full_app(load_config(config_path))
    with TestClient(app) as client:  # the context manager runs the lifespan
        assert client.get("/healthz").status_code == 200
        assert client.get("/htp/v1/dashboard").status_code == 401


def test_full_app_mounts_the_mcp_endpoint(config_path):
    app = create_full_app(load_config(config_path))
    assert any(getattr(route, "path", "").startswith("/mcp") for route in app.routes)


def test_main_reports_missing_config_without_traceback(tmp_path, capsys):
    exit_code = main(["--config", str(tmp_path / "absent.toml")])
    assert exit_code == 1
    assert "not found" in capsys.readouterr().err
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd bridge && python -m pytest tests/test_main.py tests/test_mock.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.main'`

- [ ] **Step 3: Write the mock app**

Write `bridge/src/htp_bridge/mock.py`:

```python
from __future__ import annotations

import time
from typing import Any

from fastapi import FastAPI, Request
from fastapi.responses import Response

MOCK_WAV = b"RIFF" + b"\x00" * 128
MOCK_REV = "mockrev1"
MOCK_ITEMS = [
    {"id": "t-9f2", "text": "Buy milk", "done": False},
    {"id": "t-c41", "text": "Call dentist", "done": True, "style": "dim"},
]


def create_mock_app() -> FastAPI:
    """Canned HTP responses for firmware development.

    Accepts any token and touches no database, speech provider, or agent, so a
    device can be brought up against a laptop with nothing else running.
    """
    app = FastAPI(title="HTP Bridge (mock)", docs_url=None, redoc_url=None)
    now = lambda: int(time.time())

    @app.get("/healthz")
    async def healthz() -> dict[str, Any]:
        return {"status": "ok", "mock": True, "server_time": now()}

    @app.post("/htp/v1/captures")
    async def upload(request: Request) -> dict[str, str]:
        await request.body()
        return {"id": request.headers.get("x-capture-id", "c-mock"), "state": "received"}

    @app.get("/htp/v1/captures")
    async def status(ids: str = "") -> dict[str, Any]:
        requested = [part for part in ids.split(",") if part] or ["c-mock"]
        return {
            "server_time": now(),
            "captures": [
                {
                    "id": capture_id,
                    "state": "reply_ready",
                    "transcript": "Hey Hermes, what's on my calendar today?",
                    "conversation_id": "v-mock",
                }
                for capture_id in requested
            ],
        }

    @app.get("/htp/v1/captures/{capture_id}/reply.wav")
    async def reply(capture_id: str) -> Response:
        return Response(content=MOCK_WAV, media_type="audio/wav")

    @app.get("/htp/v1/dashboard")
    async def dashboard(rev: str = "") -> dict[str, Any]:
        if rev == MOCK_REV:
            return {"rev": MOCK_REV, "unchanged": True, "server_time": now()}
        return {"rev": MOCK_REV, "server_time": now(), "title": "Today", "items": MOCK_ITEMS}

    @app.post("/htp/v1/complete")
    async def complete(payload: dict[str, Any]) -> dict[str, Any]:
        return {"ok": True, "rev": "mockrev2"}

    @app.get("/htp/v1/notifications")
    async def notifications() -> dict[str, Any]:
        return {
            "server_time": now(),
            "notifications": [
                {
                    "id": "n-1",
                    "text": "Meeting with Alex at 10:00 AM",
                    "priority": "urgent",
                    "created": now(),
                }
            ],
        }

    @app.post("/htp/v1/notifications/ack")
    async def acknowledge(payload: dict[str, Any]) -> dict[str, Any]:
        return {"ok": True, "acked": len(payload.get("ids", []))}

    return app
```

- [ ] **Step 4: Write the entrypoint**

Write `bridge/src/htp_bridge/main.py`:

```python
from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import sys
from pathlib import Path

import uvicorn
from fastapi import FastAPI

from htp_bridge.agent import AgentClient
from htp_bridge.api import Deps, create_app
from htp_bridge.captures import CaptureStore
from htp_bridge.config import Config, ConfigError, load_config
from htp_bridge.dashboard import DashboardStore
from htp_bridge.db import Database
from htp_bridge.devices import DeviceRegistry
from htp_bridge.mcp_server import HermesTools, create_mcp_server
from htp_bridge.mock import create_mock_app
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import build_speech_provider
from htp_bridge.storage import AudioStorage

SWEEP_INTERVAL_SECONDS = 60
PRUNE_INTERVAL_SECONDS = 3600

log = logging.getLogger("htp_bridge")


def build_deps(config: Config) -> tuple[Deps, HermesTools]:
    db = Database(config.storage.db_path)
    captures = CaptureStore(db)
    storage = AudioStorage(config.storage)
    notifications = NotificationStore(db)
    dashboard = DashboardStore(db, config.dashboard)
    devices = DeviceRegistry(db, config.devices)
    agent = AgentClient(config.agent)
    pipeline = Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=build_speech_provider(config.speech),
        agent=agent,
        salutation_prefixes=config.salutations.prefixes,
    )
    deps = Deps(
        server=config.server,
        captures=captures,
        storage=storage,
        dashboard=dashboard,
        notifications=notifications,
        devices=devices,
        pipeline=pipeline,
        agent=agent,
    )
    tools = HermesTools(dashboard=dashboard, notifications=notifications, devices=devices)
    return deps, tools


async def _background_loop(deps: Deps) -> None:
    """Retry unfinished work and move unclaimed replies into the queue."""
    elapsed = 0
    while True:
        await asyncio.sleep(SWEEP_INTERVAL_SECONDS)
        elapsed += SWEEP_INTERVAL_SECONDS
        try:
            await deps.pipeline.sweep_redirects()
            await deps.pipeline.sweep_ingestion()
            if elapsed >= PRUNE_INTERVAL_SECONDS:
                elapsed = 0
                deps.storage.prune()
        except Exception:
            log.exception("background sweep failed")


def create_full_app(config: Config) -> FastAPI:
    deps, tools = build_deps(config)
    mcp_app = create_mcp_server(tools).streamable_http_app()

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        # Starlette does not run a mounted sub-app's lifespan, so the MCP session
        # manager is started explicitly here. Without this, /mcp returns errors.
        async with mcp_app.router.lifespan_context(mcp_app):
            resumed = await deps.pipeline.resume()
            if resumed:
                log.info("resumed %d unfinished capture(s)", resumed)
            sweeper = asyncio.create_task(_background_loop(deps))
            try:
                yield
            finally:
                sweeper.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await sweeper

    app = create_app(deps, lifespan=lifespan)
    app.mount("/mcp", mcp_app)
    return app


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="htp-bridge")
    parser.add_argument("--config", default="/etc/htp-bridge/config.toml")
    parser.add_argument("--mock", action="store_true", help="serve canned responses")
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s"
    )

    if args.mock:
        uvicorn.run(create_mock_app(), host=args.host or "127.0.0.1", port=args.port or 8787)
        return 0

    try:
        config = load_config(Path(args.config))
    except ConfigError as exc:
        print(f"configuration error: {exc}", file=sys.stderr)
        return 1

    uvicorn.run(
        create_full_app(config),
        host=args.host or config.server.host,
        port=args.port or config.server.port,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd bridge && python -m pytest tests/test_main.py tests/test_mock.py -v`
Expected: 4 passed in `test_main.py`, 7 passed in `test_mock.py`

- [ ] **Step 6: Write the deployment files**

Write `bridge/deploy/htp-bridge.service`:

```ini
[Unit]
Description=HTP Bridge
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=htp
Group=htp
ExecStart=/opt/htp-bridge/venv/bin/htp-bridge --config /etc/htp-bridge/config.toml
Restart=always
RestartSec=5
StateDirectory=htp-bridge
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/htp-bridge

[Install]
WantedBy=multi-user.target
```

Write `bridge/deploy/Caddyfile.example`:

```
# Only the device-facing protocol is exposed. Hermes Agent and the /mcp mount
# stay on localhost — note that the catch-all below is what keeps /mcp private.
#
# Rate limiting needs the caddy-ratelimit plugin, which is not in the standard
# build. Until it is installed, the bridge is protected by bearer auth and the
# body-size cap only; add the plugin before exposing this to the internet
# long-term.
terminal.example.com {
    encode gzip

    handle /htp/v1/* {
        request_body {
            max_size 5MB
        }
        reverse_proxy 127.0.0.1:8787
    }

    handle {
        respond "Not found" 404
    }
}
```

- [ ] **Step 7: Write the README**

Write `bridge/README.md` covering, in this order: what the bridge is and its place between device and Hermes Agent (three sentences, linking to the design doc); install (`python -m venv`, `pip install -e .`); configure (copy `htp-bridge.example.toml`, generate tokens with the `secrets.token_urlsafe(32)` one-liner); run (`htp-bridge --config ...`, and `htp-bridge --mock` for firmware development); connect Hermes Agent (point its MCP client at `http://127.0.0.1:8787/mcp`, and instruct it to call `publish_dashboard` whenever its task list changes); deploy (copy the systemd unit and Caddyfile, `systemctl enable --now htp-bridge`); and test (`python -m pytest`). Include a "Verify against your Hermes Agent" section listing the four open items from spec §11 as a checklist.

- [ ] **Step 8: Commit**

```bash
git add bridge/src/htp_bridge/main.py bridge/src/htp_bridge/mock.py bridge/deploy bridge/README.md bridge/tests/test_main.py bridge/tests/test_mock.py
git commit -m "feat(bridge): add entrypoint, mock mode, and deployment files"
```

---

### Task 15: Reliability suite and contract fixtures

The per-module tests prove each piece works. This task proves the §8 guarantees hold
across pieces, and produces the golden request/response pairs that firmware development
will be written against.

**Files:**
- Create: `bridge/tests/test_reliability.py`
- Create: `bridge/tests/test_contract.py`
- Create: `bridge/tests/fixtures/contract/*.json` (generated by the test on first run)

**Interfaces:**
- Consumes: everything.
- Produces: no production code. Fixture files become the firmware's reference.

- [ ] **Step 1: Write the reliability tests**

Write `bridge/tests/test_reliability.py`:

```python
"""Cross-cutting tests for the guarantees in design section 8."""

import itertools

import pytest

from htp_bridge.agent import AgentError, FakeAgentClient
from htp_bridge.captures import CaptureStore
from htp_bridge.config import StorageConfig
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import FakeSpeechProvider, SpeechError
from htp_bridge.storage import AudioStorage
from tests.conftest import AUTH

WAV = b"RIFF" + b"\x00" * 64


def build_pipeline(db, fake_clock, tmp_path, *, speech, agent):
    captures = CaptureStore(db, clock=fake_clock)
    storage = AudioStorage(
        StorageConfig(
            db_path=tmp_path / "htp.db",
            audio_dir=tmp_path / "audio",
            upload_retention_days=365,
            reply_retention_days=7,
        ),
        clock=fake_clock,
    )
    counter = itertools.count(1)
    notifications = NotificationStore(db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}")
    pipeline = Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=speech,
        agent=agent,
        salutation_prefixes=["hey hermes"],
        clock=fake_clock,
    )
    return captures, storage, notifications, pipeline


def test_repeated_upload_delivers_the_note_exactly_once(client, app_context):
    headers = {**AUTH, "X-Capture-Id": "c-1", "X-Recorded-At": "5", "Content-Type": "audio/wav"}

    for _ in range(5):
        assert client.post("/htp/v1/captures", content=WAV, headers=headers).status_code == 200

    assert len(app_context["agent"].ingested) == 1, "idempotency must survive aggressive retries"
    assert app_context["captures"].get("c-1").state == "done"


def test_repeat_upload_does_not_reset_an_in_flight_capture(client, app_context):
    headers = {**AUTH, "X-Capture-Id": "c-1", "Content-Type": "audio/wav"}
    client.post("/htp/v1/captures", content=WAV, headers=headers)
    app_context["captures"].set_state("c-1", "failed", error="transcription_failed")

    response = client.post("/htp/v1/captures", content=WAV, headers=headers)

    assert response.json()["state"] == "failed"


@pytest.mark.parametrize("crash_state", ["received", "transcribing", "processing"])
async def test_capture_resumes_from_any_mid_pipeline_state(db, fake_clock, tmp_path, crash_state):
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = FakeAgentClient()
    captures, storage, _, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=agent
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)
    captures.set_state("c-1", crash_state)

    # A fresh Pipeline stands in for a restarted process: only durable state survives.
    _, _, _, restarted = build_pipeline(db, fake_clock, tmp_path, speech=speech, agent=agent)
    resumed = await restarted.resume()

    assert resumed == 1
    assert captures.get("c-1").state == "done"


async def test_no_capture_is_lost_when_transcription_is_down(db, fake_clock, tmp_path):
    speech = FakeSpeechProvider(transcribe_error=SpeechError("stt down"))
    captures, storage, _, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=FakeAgentClient()
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)

    await pipeline.process("c-1")

    assert captures.get("c-1").state == "failed"
    assert storage.upload_path("c-1").exists(), "the recording must survive a provider outage"


async def test_note_survives_agent_outage_and_is_delivered_on_recovery(db, fake_clock, tmp_path):
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    captures, storage, _, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)
    await pipeline.process("c-1")

    healthy = FakeAgentClient()
    _, _, _, recovered = build_pipeline(db, fake_clock, tmp_path, speech=speech, agent=healthy)
    await recovered.sweep_ingestion()

    assert healthy.ingested == [("Add milk", 1)]
    assert captures.get("c-1").error is None


async def test_unclaimed_reply_is_never_silently_dropped(db, fake_clock, tmp_path):
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, what's for dinner?"})
    captures, storage, notifications, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=FakeAgentClient(reply_text="Lasagne at 7 PM.")
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)
    await pipeline.process("c-1")

    fake_clock.advance(3600)  # device slept long ago
    await pipeline.sweep_redirects()

    assert [n.text for n in notifications.pending()] == ["Lasagne at 7 PM."]


def test_notifications_redeliver_until_acknowledged(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM", "urgent")

    for _ in range(3):
        body = client.get("/htp/v1/notifications", headers=AUTH).json()
        assert len(body["notifications"]) == 1

    client.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH)
    assert client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"] == []


def test_device_learns_of_captures_the_bridge_never_received(client):
    body = client.get("/htp/v1/captures", params={"ids": "c-lost"}, headers=AUTH).json()
    assert body["captures"][0]["state"] == "unknown"


def test_every_device_response_carries_server_time(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    for path, params in (
        ("/htp/v1/captures", {"ids": ""}),
        ("/htp/v1/dashboard", {}),
        ("/htp/v1/notifications", {}),
    ):
        body = client.get(path, params=params, headers=AUTH).json()
        assert body["server_time"] == app_context["clock"].now, f"{path} must carry server_time"


def test_oversized_upload_is_rejected_without_storing(client, app_context):
    headers = {**AUTH, "X-Capture-Id": "c-big", "Content-Type": "audio/wav"}
    client.post("/htp/v1/captures", content=b"x" * 5000, headers=headers)

    assert app_context["captures"].get("c-big") is None
    assert not app_context["storage"].upload_path("c-big").exists()
```

- [ ] **Step 2: Run the reliability tests**

Run: `cd bridge && python -m pytest tests/test_reliability.py -v`
Expected: 12 passed (the parametrized resume test expands to 3 cases)

If `test_oversized_upload_is_rejected_without_storing` fails, the size check in
`upload_capture` is running after `captures.create` — move it above, as written in Task 12.

- [ ] **Step 3: Write the contract fixture test**

Write `bridge/tests/test_contract.py`:

```python
"""Golden HTP responses.

These files are the reference the device firmware is written against. A failure
here means the wire format changed: update the fixture deliberately and treat it
as a protocol change, not a test annoyance.
"""

import json
from pathlib import Path

import pytest

from tests.conftest import AUTH

FIXTURES = Path(__file__).parent / "fixtures" / "contract"
WAV = b"RIFF" + b"\x00" * 64


def assert_matches_fixture(name: str, payload: dict) -> None:
    FIXTURES.mkdir(parents=True, exist_ok=True)
    path = FIXTURES / f"{name}.json"
    serialized = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if not path.exists():
        path.write_text(serialized)
        pytest.skip(f"wrote new fixture {path.name}; re-run to verify")
    assert json.loads(path.read_text()) == payload, f"wire format changed for {name}"


def test_capture_upload_response(client):
    response = client.post(
        "/htp/v1/captures",
        content=WAV,
        headers={**AUTH, "X-Capture-Id": "c-20260804-101502-3fa9", "Content-Type": "audio/wav"},
    )
    assert_matches_fixture("captures_post", response.json())


def test_capture_status_response(client, app_context):
    client.post(
        "/htp/v1/captures",
        content=WAV,
        headers={**AUTH, "X-Capture-Id": "c-20260804-101502-3fa9", "Content-Type": "audio/wav"},
    )
    body = client.get(
        "/htp/v1/captures",
        params={"ids": "c-20260804-101502-3fa9,c-missing"},
        headers=AUTH,
    ).json()
    assert_matches_fixture("captures_get", body)


def test_dashboard_response(client, app_context):
    app_context["dashboard"].publish(
        "Today",
        [
            {"id": "t-9f2", "text": "Buy milk", "done": False},
            {"id": "t-c41", "text": "Call dentist", "done": True, "style": "dim"},
        ],
    )
    assert_matches_fixture("dashboard_get", client.get("/htp/v1/dashboard", headers=AUTH).json())


def test_dashboard_unchanged_response(client, app_context):
    rev = app_context["dashboard"].publish(
        "Today", [{"id": "t-9f2", "text": "Buy milk", "done": False}]
    )
    body = client.get("/htp/v1/dashboard", params={"rev": rev}, headers=AUTH).json()
    assert_matches_fixture("dashboard_unchanged", body)


def test_notifications_response(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM", "urgent")
    assert_matches_fixture(
        "notifications_get", client.get("/htp/v1/notifications", headers=AUTH).json()
    )


def test_complete_response(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-9f2", "text": "Buy milk", "done": False}])
    body = client.post("/htp/v1/complete", json={"item_id": "t-9f2"}, headers=AUTH).json()
    assert_matches_fixture("complete_post", body)


def test_ack_response(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM")
    body = client.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH).json()
    assert_matches_fixture("ack_post", body)


@pytest.mark.parametrize(
    "name,call",
    [
        ("error_unauthorized", lambda c: c.get("/htp/v1/dashboard")),
        (
            "error_invalid_capture_id",
            lambda c: c.post(
                "/htp/v1/captures", content=WAV, headers={**AUTH, "X-Capture-Id": "../nope"}
            ),
        ),
        ("error_reply_not_found", lambda c: c.get("/htp/v1/captures/c-nope/reply.wav", headers=AUTH)),
    ],
)
def test_error_responses(client, name, call):
    assert_matches_fixture(name, call(client).json())
```

- [ ] **Step 4: Generate and verify the fixtures**

Run twice — the first run writes the files and skips, the second asserts against them:

```bash
cd bridge && python -m pytest tests/test_contract.py -v && python -m pytest tests/test_contract.py -v
```

Expected: first run skips 10, second run passes 10.

The fake clock in `app_context` is fixed, so `server_time` is stable across runs and the
fixtures are reproducible.

- [ ] **Step 5: Run the whole suite**

Run: `cd bridge && python -m pytest -v`
Expected: all tests pass, no skips.

- [ ] **Step 6: Commit**

```bash
git add bridge/tests/test_reliability.py bridge/tests/test_contract.py bridge/tests/fixtures
git commit -m "test(bridge): add reliability suite and golden protocol fixtures"
```

---

## Verification against the live system

These are spec §11 open items. They are checked after the suite is green, against the
running Hermes Agent, and any mismatch is fixed in the single module named below.

- [ ] **Hermes Agent API shape** — confirm the API server's chat endpoint path, request
  body, and response shape. Fix `agent.py` if it differs; nothing else changes.
- [ ] **Model parameter** — confirm whether the agent accepts `model`. If not, drop it
  from the payload in `agent.py` and leave `model` unset in configuration.
- [ ] **MCP client wiring** — point Hermes Agent's MCP client at
  `http://127.0.0.1:8787/mcp` and confirm the three tools appear.
- [ ] **Dashboard republish instruction** — instruct the agent to call
  `publish_dashboard` whenever its task memory changes, and confirm a change in the
  agent produces a new revision on `GET /htp/v1/dashboard`.
- [ ] **Speech providers** — confirm the chosen vendor's transcription and synthesis
  endpoints against `speech.py`, and pick a voice.
- [ ] **Public hostname and TLS** — deploy Caddy, confirm `GET /healthz` over HTTPS from
  outside the network and that `/mcp` is *not* reachable from outside.

## Follow-on plan

Device firmware is a separate plan, written after this one is complete. It depends on
`htp-bridge --mock` and the contract fixtures produced in Task 15.
