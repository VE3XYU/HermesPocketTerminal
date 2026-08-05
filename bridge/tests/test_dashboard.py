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


def test_publish_tolerates_non_string_style(db, fake_clock):
    s = store(db, fake_clock)
    s.publish(
        "Today",
        [
            {"id": "t-1", "text": "Normal", "done": False, "style": "bold"},
            {"id": "t-2", "text": "Malformed", "done": False, "style": ["oops"]},
        ],
    )
    items = s.current().items
    assert len(items) == 2
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
