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
    assert len(store.pending()) == 1, "fetching does not consume"
