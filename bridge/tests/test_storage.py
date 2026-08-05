import os

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


def test_prune_removes_expired_orphaned_part_files(storage, fake_clock):
    """A crash between write_bytes and replace strands a .wav.part forever;
    prune must clean those up on the directory's retention window."""
    orphan = storage.reply_path("c-crash").with_suffix(".wav.part")
    orphan.write_bytes(b"partial")
    os.utime(orphan, (fake_clock.now, fake_clock.now))
    fake_clock.advance(8 * 86400)

    removed = storage.prune()

    assert not orphan.exists()
    assert removed == 1


def test_prune_keeps_part_files_inside_retention(storage, fake_clock):
    fresh = storage.reply_path("c-live").with_suffix(".wav.part")
    fresh.write_bytes(b"partial")
    os.utime(fresh, (fake_clock.now, fake_clock.now))

    removed = storage.prune()

    assert fresh.exists()
    assert removed == 0


def _streaming_wav(payload: bytes) -> bytes:
    """A WAV whose size fields are the 0xFFFFFFFF placeholders OpenAI TTS returns."""
    fmt = (
        b"fmt " + (16).to_bytes(4, "little")
        + (1).to_bytes(2, "little")      # PCM
        + (1).to_bytes(2, "little")      # mono
        + (24000).to_bytes(4, "little")  # sample rate
        + (48000).to_bytes(4, "little")  # byte rate
        + (2).to_bytes(2, "little")      # block align
        + (16).to_bytes(2, "little")     # bits per sample
    )
    return (
        b"RIFF" + b"\xff\xff\xff\xff" + b"WAVE"
        + fmt
        + b"data" + b"\xff\xff\xff\xff" + payload
    )


def test_save_reply_repairs_streaming_wav_size_fields(storage):
    """Providers stream TTS with placeholder sizes; the device plays by header.

    OpenAI's /audio/speech returns RIFF and data sizes of 0xFFFFFFFF regardless
    of the real length, so a device trusting the header would read far past the
    end of the file. The bridge owns the reply.wav contract and must hand the
    device a WAV whose header describes the bytes actually present.
    """
    payload = b"\x01\x02" * 1200
    path = storage.save_reply("c-1", _streaming_wav(payload))

    stored = path.read_bytes()
    assert int.from_bytes(stored[4:8], "little") == len(stored) - 8
    data_index = stored.index(b"data")
    assert int.from_bytes(stored[data_index + 4 : data_index + 8], "little") == len(payload)


def test_save_reply_leaves_a_well_formed_wav_untouched(storage):
    payload = b"\x03\x04" * 64
    good = bytearray(_streaming_wav(payload))
    good[4:8] = (len(good) - 8).to_bytes(4, "little")
    good[good.index(b"data") + 4 : good.index(b"data") + 8] = len(payload).to_bytes(4, "little")

    assert storage.save_reply("c-1", bytes(good)).read_bytes() == bytes(good)


def test_save_reply_stores_non_riff_bytes_verbatim(storage):
    """Only RIFF containers are repaired -- anything else is passed through."""
    assert storage.save_reply("c-1", b"ID3not-a-wav").read_bytes() == b"ID3not-a-wav"
