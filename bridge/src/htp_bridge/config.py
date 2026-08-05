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
