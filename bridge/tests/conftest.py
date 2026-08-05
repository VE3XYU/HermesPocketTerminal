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
