import itertools

import pytest

from htp_bridge.config import DashboardConfig
from htp_bridge.dashboard import DashboardStore
from htp_bridge.devices import DeviceRegistry
from htp_bridge.mcp_server import HermesTools, create_mcp_server
from htp_bridge.notifications import NotificationStore


@pytest.fixture
def tools(db, fake_clock, device_config):
    counter = itertools.count(1)
    return HermesTools(
        dashboard=DashboardStore(
            db, DashboardConfig(max_items=32, max_text_chars=40), clock=fake_clock
        ),
        notifications=NotificationStore(db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}"),
        devices=DeviceRegistry(db, [device_config], clock=fake_clock),
    )


def test_publish_dashboard_returns_revision_and_count(tools):
    result = tools.publish_dashboard("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    assert result["item_count"] == 1
    assert len(result["rev"]) == 8


def test_publish_dashboard_replaces_previous_list(tools):
    tools.publish_dashboard("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    tools.publish_dashboard("Today", [{"id": "t-2", "text": "Call dentist", "done": False}])
    assert tools.dashboard.current().items[0].id == "t-2"


def test_publish_dashboard_accepts_an_empty_list(tools):
    result = tools.publish_dashboard("Today", [])
    assert result["item_count"] == 0


def test_queue_notification_returns_id(tools):
    result = tools.queue_notification("Meeting with Alex at 10:00 AM", "urgent")
    assert result["id"] == "n-1"
    assert tools.notifications.pending()[0].priority == "urgent"


def test_queue_notification_defaults_to_normal_priority(tools):
    tools.queue_notification("A reminder")
    assert tools.notifications.pending()[0].priority == "normal"


def test_queue_notification_rejects_empty_text(tools):
    with pytest.raises(ValueError, match="empty"):
        tools.queue_notification("   ")


def test_get_device_status_reports_battery_and_last_seen(tools, fake_clock):
    tools.devices.record_telemetry("pocket-01", 78)
    status = tools.get_device_status()[0]
    assert status == {"device_id": "pocket-01", "battery": 78, "last_seen": fake_clock.now}


def test_get_device_status_is_empty_before_any_contact(tools):
    assert tools.get_device_status() == []


def test_queue_notification_docstring_states_the_absolute_time_rule(tools):
    assert "absolute" in HermesTools.queue_notification.__doc__.lower()


async def test_mcp_server_registers_the_three_tools(tools):
    server = create_mcp_server(tools)
    names = {tool.name for tool in await server.list_tools()}
    assert names == {"publish_dashboard", "queue_notification", "get_device_status"}
