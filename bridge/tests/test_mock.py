from fastapi.testclient import TestClient

from htp_bridge.mock import create_mock_app

AUTH = {"Authorization": "Bearer any-token-works-in-mock"}


def client():
    return TestClient(create_mock_app())


def test_mock_accepts_any_token():
    response = client().get("/htp/v1/dashboard", headers={"Authorization": "Bearer whatever"})
    assert response.status_code == 200


def test_mock_upload_returns_received():
    response = client().post(
        "/htp/v1/captures", content=b"RIFF", headers={**AUTH, "X-Capture-Id": "c-1"}
    )
    assert response.json() == {"id": "c-1", "state": "received"}


def test_mock_status_reports_a_ready_reply():
    body = client().get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH).json()
    entry = body["captures"][0]
    assert entry["state"] == "reply_ready"
    assert entry["transcript"]
    assert "server_time" in body


def test_mock_reply_returns_wav_bytes():
    response = client().get("/htp/v1/captures/c-1/reply.wav", headers=AUTH)
    assert response.status_code == 200
    assert response.content.startswith(b"RIFF")


def test_mock_dashboard_has_items_and_stable_rev():
    first = client().get("/htp/v1/dashboard", headers=AUTH).json()
    assert first["items"]
    second = client().get("/htp/v1/dashboard", params={"rev": first["rev"]}, headers=AUTH).json()
    assert second["unchanged"] is True


def test_mock_notifications_and_ack():
    c = client()
    body = c.get("/htp/v1/notifications", headers=AUTH).json()
    assert body["notifications"][0]["priority"] in {"normal", "urgent"}
    assert c.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH).json()["ok"]


def test_mock_complete_returns_ok():
    assert client().post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH).json()["ok"]


def test_mock_dashboard_serves_sync_interval_on_full_body():
    body = client().get("/htp/v1/dashboard", headers=AUTH).json()
    assert body["sync_interval"] == 600


def test_mock_dashboard_serves_sync_interval_when_unchanged():
    c = client()
    rev = c.get("/htp/v1/dashboard", headers=AUTH).json()["rev"]
    body = c.get("/htp/v1/dashboard", params={"rev": rev}, headers=AUTH).json()
    assert body["unchanged"] is True
    assert body["sync_interval"] == 600


def test_mock_unknown_path_returns_not_found_slug():
    response = client().get("/htp/v1/nope", headers=AUTH)
    assert response.status_code == 404
    assert response.json() == {"error": "not_found"}


def test_mock_wrong_method_returns_method_not_allowed_slug():
    response = client().delete("/htp/v1/dashboard", headers=AUTH)
    assert response.status_code == 405
    assert response.json() == {"error": "method_not_allowed"}


def test_mock_invalid_body_returns_invalid_request_slug():
    response = client().post(
        "/htp/v1/complete", content=b"not json", headers={**AUTH, "Content-Type": "application/json"}
    )
    assert response.status_code == 422
    assert response.json() == {"error": "invalid_request"}
