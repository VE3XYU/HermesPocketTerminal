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
