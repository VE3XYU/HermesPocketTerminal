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
