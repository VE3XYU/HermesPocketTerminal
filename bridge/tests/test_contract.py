"""Golden HTP responses.

These files are the reference the device firmware is written against. A failure
here means the wire format changed: update the fixture deliberately and treat it
as a protocol change, not a test annoyance.
"""

import json
from pathlib import Path

import pytest

from tests.conftest import AUTH

FIXTURES = Path(__file__).parent / "fixtures" / "contract"
WAV = b"RIFF" + b"\x00" * 64


def assert_matches_fixture(name: str, payload: dict) -> None:
    FIXTURES.mkdir(parents=True, exist_ok=True)
    path = FIXTURES / f"{name}.json"
    serialized = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if not path.exists():
        path.write_text(serialized)
        pytest.skip(f"wrote new fixture {path.name}; re-run to verify")
    assert json.loads(path.read_text()) == payload, f"wire format changed for {name}"


def test_capture_upload_response(client):
    response = client.post(
        "/htp/v1/captures",
        content=WAV,
        headers={**AUTH, "X-Capture-Id": "c-20260804-101502-3fa9", "Content-Type": "audio/wav"},
    )
    assert_matches_fixture("captures_post", response.json())


def test_capture_status_response(client, app_context):
    client.post(
        "/htp/v1/captures",
        content=WAV,
        headers={**AUTH, "X-Capture-Id": "c-20260804-101502-3fa9", "Content-Type": "audio/wav"},
    )
    body = client.get(
        "/htp/v1/captures",
        params={"ids": "c-20260804-101502-3fa9,c-missing"},
        headers=AUTH,
    ).json()
    assert_matches_fixture("captures_get", body)


def test_dashboard_response(client, app_context):
    app_context["dashboard"].publish(
        "Today",
        [
            {"id": "t-9f2", "text": "Buy milk", "done": False},
            {"id": "t-c41", "text": "Call dentist", "done": True, "style": "dim"},
        ],
    )
    assert_matches_fixture("dashboard_get", client.get("/htp/v1/dashboard", headers=AUTH).json())


def test_dashboard_unchanged_response(client, app_context):
    rev = app_context["dashboard"].publish(
        "Today", [{"id": "t-9f2", "text": "Buy milk", "done": False}]
    )
    body = client.get("/htp/v1/dashboard", params={"rev": rev}, headers=AUTH).json()
    assert_matches_fixture("dashboard_unchanged", body)


def test_notifications_response(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM", "urgent")
    assert_matches_fixture(
        "notifications_get", client.get("/htp/v1/notifications", headers=AUTH).json()
    )


def test_complete_response(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-9f2", "text": "Buy milk", "done": False}])
    body = client.post("/htp/v1/complete", json={"item_id": "t-9f2"}, headers=AUTH).json()
    assert_matches_fixture("complete_post", body)


def test_ack_response(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM")
    body = client.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH).json()
    assert_matches_fixture("ack_post", body)


@pytest.mark.parametrize(
    "name,call",
    [
        ("error_unauthorized", lambda c: c.get("/htp/v1/dashboard")),
        (
            "error_invalid_capture_id",
            lambda c: c.post(
                "/htp/v1/captures", content=WAV, headers={**AUTH, "X-Capture-Id": "../nope"}
            ),
        ),
        ("error_reply_not_found", lambda c: c.get("/htp/v1/captures/c-nope/reply.wav", headers=AUTH)),
    ],
)
def test_error_responses(client, name, call):
    assert_matches_fixture(name, call(client).json())
