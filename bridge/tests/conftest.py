import itertools

import pytest
from fastapi.testclient import TestClient

from htp_bridge.agent import FakeAgentClient
from htp_bridge.api import Deps, create_app
from htp_bridge.captures import CaptureStore
from htp_bridge.config import DashboardConfig, DeviceConfig, ServerConfig, StorageConfig
from htp_bridge.dashboard import DashboardStore
from htp_bridge.db import Database
from htp_bridge.devices import DeviceRegistry
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import FakeSpeechProvider
from htp_bridge.storage import AudioStorage

TOKEN = "tok-aaaaaaaaaaaaaaaaaaaa"
AUTH = {"Authorization": f"Bearer {TOKEN}"}


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
