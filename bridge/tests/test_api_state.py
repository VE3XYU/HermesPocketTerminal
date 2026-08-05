from tests.conftest import AUTH


def test_healthz_needs_no_token(client):
    response = client.get("/healthz")
    assert response.status_code == 200
    assert response.json()["status"] == "ok"


def test_dashboard_is_empty_before_first_publish(client):
    body = client.get("/htp/v1/dashboard", headers=AUTH).json()
    assert body["items"] == []
    assert body["rev"] == "0"


def test_dashboard_returns_published_items(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])

    body = client.get("/htp/v1/dashboard", headers=AUTH).json()

    assert body["title"] == "Today"
    assert body["items"] == [{"id": "t-1", "text": "Buy milk", "done": False}]
    assert body["server_time"] == app_context["clock"].now


def test_dashboard_includes_style_only_when_set(client, app_context):
    app_context["dashboard"].publish(
        "Today",
        [
            {"id": "t-1", "text": "Plain", "done": False},
            {"id": "t-2", "text": "Dim", "done": True, "style": "dim"},
        ],
    )
    items = client.get("/htp/v1/dashboard", headers=AUTH).json()["items"]
    assert "style" not in items[0]
    assert items[1]["style"] == "dim"


def test_dashboard_returns_unchanged_for_matching_revision(client, app_context):
    rev = app_context["dashboard"].publish(
        "Today", [{"id": "t-1", "text": "Buy milk", "done": False}]
    )

    body = client.get("/htp/v1/dashboard", params={"rev": rev}, headers=AUTH).json()

    assert body == {
        "rev": rev,
        "unchanged": True,
        "server_time": app_context["clock"].now,
        "sync_interval": 600,
    }


def test_dashboard_returns_full_body_for_stale_revision(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    body = client.get("/htp/v1/dashboard", params={"rev": "stale123"}, headers=AUTH).json()
    assert "items" in body


def test_dashboard_requires_a_token(client):
    assert client.get("/htp/v1/dashboard").status_code == 401


def test_dashboard_serves_sync_interval_on_full_body(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    body = client.get("/htp/v1/dashboard", headers=AUTH).json()
    assert body["sync_interval"] == 600


def test_dashboard_serves_sync_interval_when_empty(client):
    body = client.get("/htp/v1/dashboard", headers=AUTH).json()
    assert body["sync_interval"] == 600


def test_complete_marks_item_done_and_returns_new_rev(client, app_context):
    old = app_context["dashboard"].publish(
        "Today", [{"id": "t-1", "text": "Buy milk", "done": False}]
    )

    response = client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH)

    assert response.status_code == 200
    assert response.json()["ok"] is True
    assert response.json()["rev"] != old
    assert app_context["dashboard"].current().items[0].done is True


def test_complete_tells_the_agent(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH)

    message = app_context["agent"].ingested[-1][0]
    assert "t-1" in message
    assert "Buy milk" in message


def test_complete_is_idempotent(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    first = client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH).json()
    second = client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH).json()
    assert first["rev"] == second["rev"]


def test_complete_404s_for_unknown_item(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    response = client.post("/htp/v1/complete", json={"item_id": "t-nope"}, headers=AUTH)
    assert response.status_code == 404
    assert response.json() == {"error": "unknown_item"}


def test_complete_rejects_missing_item_id(client):
    response = client.post("/htp/v1/complete", json={}, headers=AUTH)
    assert response.status_code == 400
    assert response.json() == {"error": "missing_item_id"}


def test_notifications_returns_pending_queue(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM", "urgent")

    body = client.get("/htp/v1/notifications", headers=AUTH).json()

    assert body["notifications"] == [
        {
            "id": "n-1",
            "text": "Meeting with Alex at 10:00 AM",
            "priority": "urgent",
            "created": app_context["clock"].now,
        }
    ]


def test_notifications_redeliver_until_acknowledged(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM")
    assert len(client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"]) == 1
    assert len(client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"]) == 1


def test_ack_removes_notifications(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM")

    response = client.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH)

    assert response.json() == {"ok": True, "acked": 1}
    assert client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"] == []


def test_ack_with_unknown_ids_is_accepted(client):
    response = client.post("/htp/v1/notifications/ack", json={"ids": ["n-nope"]}, headers=AUTH)
    assert response.json() == {"ok": True, "acked": 0}


def test_ack_rejects_missing_ids_field(client):
    response = client.post("/htp/v1/notifications/ack", json={}, headers=AUTH)
    assert response.status_code == 400
    assert response.json() == {"error": "missing_ids"}


def test_ack_rejects_malformed_json_body(client):
    response = client.post(
        "/htp/v1/notifications/ack",
        content=b"{not valid json",
        headers={**AUTH, "Content-Type": "application/json"},
    )
    assert response.status_code == 422
    assert response.json() == {"error": "invalid_request"}


def test_unknown_path_under_htp_v1_returns_not_found(client):
    response = client.get("/htp/v1/does-not-exist", headers=AUTH)
    assert response.status_code == 404
    assert response.json() == {"error": "not_found"}


def test_wrong_method_on_known_path_returns_method_not_allowed(client):
    response = client.delete("/htp/v1/dashboard", headers=AUTH)
    assert response.status_code == 405
    assert response.json() == {"error": "method_not_allowed"}
