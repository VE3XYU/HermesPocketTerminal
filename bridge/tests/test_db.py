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


def test_read_enforces_read_only(tmp_path):
    path = tmp_path / "htp.db"
    db = Database(path)
    with pytest.raises(sqlite3.OperationalError):
        with db.read() as conn:
            conn.execute(
                "INSERT INTO notifications (id, text, priority, created_at) VALUES (?,?,?,?)",
                ("n-1", "hello", "normal", 100),
            )
    with db.write() as conn:
        assert conn.execute("SELECT COUNT(*) FROM notifications").fetchone()[0] == 0
    db.close()
