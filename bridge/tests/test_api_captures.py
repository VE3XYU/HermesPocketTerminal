from tests.conftest import AUTH

WAV = b"RIFF" + b"\x00" * 64


def upload(client, capture_id, *, body=WAV, headers=None, mode="auto"):
    request_headers = {
        **AUTH,
        "Content-Type": "audio/wav",
        "X-Capture-Id": capture_id,
        "X-Capture-Mode": mode,
        "X-Recorded-At": "1754300102",
        "X-Battery": "78",
    }
    request_headers.update(headers or {})
    return client.post("/htp/v1/captures", content=body, headers=request_headers)


def test_upload_accepts_recording_and_returns_received(client):
    response = upload(client, "c-1")
    assert response.status_code == 200
    assert response.json()["id"] == "c-1"
    assert response.json()["state"] in {"received", "done"}


def test_upload_stores_the_wav(client, app_context):
    upload(client, "c-1")
    assert app_context["storage"].upload_path("c-1").read_bytes() == WAV


def test_upload_records_battery_telemetry(client, app_context):
    upload(client, "c-1")
    assert app_context["devices"].statuses()[0].battery == 78


def test_upload_runs_the_pipeline(client, app_context):
    upload(client, "c-1")
    assert app_context["captures"].get("c-1").state == "done"
    assert app_context["agent"].ingested == [("Add milk to the shopping list", 1754300102)]


def test_repeat_upload_does_not_reprocess(client, app_context):
    upload(client, "c-1")
    upload(client, "c-1")
    assert len(app_context["agent"].ingested) == 1


def test_upload_without_token_is_unauthorized(client):
    response = client.post(
        "/htp/v1/captures", content=WAV, headers={"X-Capture-Id": "c-1", "Content-Type": "audio/wav"}
    )
    assert response.status_code == 401
    assert response.json() == {"error": "unauthorized"}


def test_upload_with_wrong_token_is_unauthorized(client):
    response = upload(client, "c-1", headers={"Authorization": "Bearer nope"})
    assert response.status_code == 401


def test_upload_rejects_missing_capture_id(client):
    response = client.post(
        "/htp/v1/captures", content=WAV, headers={**AUTH, "Content-Type": "audio/wav"}
    )
    assert response.status_code == 400
    assert response.json() == {"error": "missing_capture_id"}


def test_upload_rejects_unsafe_capture_id(client):
    response = upload(client, "../escape")
    assert response.status_code == 400
    assert response.json() == {"error": "invalid_capture_id"}


def test_upload_rejects_empty_body(client):
    response = upload(client, "c-1", body=b"")
    assert response.status_code == 400
    assert response.json() == {"error": "empty_capture"}


def test_upload_rejects_oversized_body(client):
    response = upload(client, "c-1", body=b"x" * 1001)
    assert response.status_code == 413
    assert response.json() == {"error": "capture_too_large"}


def test_oversized_upload_leaves_no_orphan_wav_or_capture_row(client, app_context):
    # Protects the D1 ordering: the size cap is enforced before storage.save_upload
    # is ever called, so a rejected upload must leave neither an orphan WAV file
    # nor a capture row behind.
    upload(client, "c-1", body=b"x" * 1001)
    assert not app_context["storage"].upload_path("c-1").exists()
    assert app_context["captures"].get("c-1") is None


def test_background_pipeline_failure_marks_capture_pipeline_error(client, app_context):
    # An unexpected exception from deep inside the pipeline (here, from the
    # speech provider, which process() does not catch beyond SpeechError/OSError)
    # must not vanish silently after the 200 has already been returned -- it
    # must mark the capture failed so it isn't wedged non-terminal forever.
    async def explode(_wav_path):
        raise RuntimeError("boom")

    app_context["speech"].transcribe = explode

    upload(client, "c-1")

    capture = app_context["captures"].get("c-1")
    assert capture.state == "failed"
    assert capture.error == "pipeline_error"


def test_status_poll_returns_requested_captures(client, app_context):
    upload(client, "c-1")
    response = client.get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH)

    body = response.json()
    assert body["captures"][0]["id"] == "c-1"
    assert body["captures"][0]["state"] == "done"
    assert body["captures"][0]["transcript"] == "Add milk to the shopping list"
    assert body["server_time"] == app_context["clock"].now


def test_status_poll_reports_unknown_ids(client):
    response = client.get("/htp/v1/captures", params={"ids": "c-missing"}, headers=AUTH)
    assert response.json()["captures"] == [{"id": "c-missing", "state": "unknown"}]


def test_status_poll_preserves_request_order(client):
    upload(client, "c-1")
    upload(client, "c-2")
    response = client.get("/htp/v1/captures", params={"ids": "c-2,c-missing,c-1"}, headers=AUTH)
    assert [c["id"] for c in response.json()["captures"]] == ["c-2", "c-missing", "c-1"]


def test_status_poll_includes_error_for_failed_captures(client, app_context):
    upload(client, "c-1")
    app_context["captures"].set_state("c-1", "failed", error="transcription_failed")
    response = client.get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH)
    assert response.json()["captures"][0]["error"] == "transcription_failed"


def test_status_poll_rejects_too_many_ids(client):
    response = client.get(
        "/htp/v1/captures", params={"ids": ",".join(f"c-{n}" for n in range(65))}, headers=AUTH
    )
    assert response.status_code == 400
    assert response.json() == {"error": "too_many_ids"}


def test_status_poll_with_no_ids_returns_empty_list(client):
    response = client.get("/htp/v1/captures", params={"ids": ""}, headers=AUTH)
    assert response.json()["captures"] == []


def test_reply_download_returns_audio_and_marks_downloaded(client, app_context):
    app_context["captures"].create(
        capture_id="c-9", device_id="pocket-01", recorded_at=1, conversation_id=None
    )
    app_context["storage"].save_reply("c-9", b"RIFFreply")
    app_context["captures"].set_reply("c-9", "Lasagne at 7 PM.")

    response = client.get("/htp/v1/captures/c-9/reply.wav", headers=AUTH)

    assert response.status_code == 200
    assert response.content == b"RIFFreply"
    assert response.headers["content-type"] == "audio/wav"
    assert app_context["captures"].get("c-9").reply_downloaded_at is not None


def test_reply_download_404s_when_absent(client):
    response = client.get("/htp/v1/captures/c-nope/reply.wav", headers=AUTH)
    assert response.status_code == 404
    assert response.json() == {"error": "reply_not_found"}


def test_conversation_id_is_returned_and_accepted(client, app_context):
    app_context["speech"].set_default_transcript("Hey Hermes, what's for dinner?")
    upload(client, "c-1")

    status = client.get("/htp/v1/captures", params={"ids": "c-1"}, headers=AUTH).json()
    conversation_id = status["captures"][0]["conversation_id"]
    assert conversation_id == "v-1"

    upload(client, "c-2", headers={"X-Conversation-Id": conversation_id})
    assert app_context["captures"].get("c-2").conversation_id == "v-1"
