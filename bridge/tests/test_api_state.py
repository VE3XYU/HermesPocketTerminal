import httpx

from htp_bridge.api import authenticate_token
from tests.conftest import AUTH, TOKEN


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
    assert body["rev"] != "stale123", "the device stores this rev; echoing the stale one wedges sync"


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


def test_complete_404s_when_item_vanishes_between_check_and_write(client, app_context):
    # An MCP publish can remove the item after the endpoint's snapshot check
    # but before DashboardStore.complete runs; complete() then returns None.
    # The device must get unknown_item, not {"ok": true, "rev": null}.
    dashboard = app_context["dashboard"]
    dashboard.publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])

    original = dashboard.complete

    def racing_complete(item_id):
        dashboard.publish("Today", [])
        return original(item_id)

    dashboard.complete = racing_complete

    response = client.post("/htp/v1/complete", json={"item_id": "t-1"}, headers=AUTH)

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


def test_ack_rejects_oversized_ids_list(client):
    ids = [f"n-{i}" for i in range(65)]
    response = client.post("/htp/v1/notifications/ack", json={"ids": ids}, headers=AUTH)
    assert response.status_code == 400
    assert response.json() == {"error": "too_many_ids"}


def test_ack_accepts_a_full_batch_at_the_cap(client):
    ids = [f"n-{i}" for i in range(64)]
    response = client.post("/htp/v1/notifications/ack", json={"ids": ids}, headers=AUTH)
    assert response.status_code == 200
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


def test_authenticate_token_rejects_non_ascii_token_without_raising(app_context):
    # httpx (and real HTTP clients) refuse to send non-ASCII header values, so
    # this can't be exercised through client.post(...). Starlette itself
    # decodes headers as latin-1, though, so a garbled Authorization header can
    # still carry non-ASCII bytes into the app. hmac.compare_digest raises
    # TypeError on non-ASCII str input rather than returning False; the fix
    # rejects such tokens before they reach DeviceRegistry.authenticate.
    result = authenticate_token(app_context["devices"], "tok-\xff")
    assert result is None


def test_authenticate_token_still_accepts_a_valid_token(app_context):
    result = authenticate_token(app_context["devices"], TOKEN)
    assert result is not None
    assert result.id == "pocket-01"


async def test_unhandled_exception_returns_generic_500(app_context):
    def boom():
        raise RuntimeError("boom")

    app_context["notifications"].pending = boom

    transport = httpx.ASGITransport(app=app_context["client"].app, raise_app_exceptions=False)
    async with httpx.AsyncClient(transport=transport, base_url="http://test") as raw_client:
        response = await raw_client.get("/htp/v1/notifications", headers=AUTH)

    assert response.status_code == 500
    assert response.json() == {"error": "internal_error"}
