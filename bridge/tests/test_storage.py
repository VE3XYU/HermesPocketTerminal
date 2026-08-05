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
    ["", "../escape", "c-1/../../etc/passwd", "c 1", "c-1.wav", "a" * 129, "c-1\x00", "c-1\n"],
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
