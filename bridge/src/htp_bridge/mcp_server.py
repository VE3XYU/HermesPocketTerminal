from __future__ import annotations

from typing import Any

from mcp.server.fastmcp import FastMCP

from htp_bridge.dashboard import DashboardStore
from htp_bridge.devices import DeviceRegistry
from htp_bridge.notifications import NotificationStore


class HermesTools:
    """The bridge's writable surface for Hermes Agent.

    Hermes Agent's memory is the source of truth; these tools publish a
    materialized copy that terminal devices can poll without waking the agent.
    """

    def __init__(
        self,
        dashboard: DashboardStore,
        notifications: NotificationStore,
        devices: DeviceRegistry,
    ) -> None:
        self.dashboard = dashboard
        self.notifications = notifications
        self.devices = devices

    def publish_dashboard(self, title: str, items: list[dict[str, Any]]) -> dict[str, Any]:
        """Replace the list shown on pocket terminals.

        Call this whenever the task list changes. The whole list is replaced, so
        send the complete current state, not a delta.

        Args:
            title: Short heading, e.g. "Today".
            items: Objects with "id" (stable identifier used when the user marks
                the item complete), "text" (short, truncated for a small display),
                "done" (boolean), and optional "style" of "bold" or "dim".
        """
        rev = self.dashboard.publish(title, items)
        return {"rev": rev, "item_count": len(self.dashboard.current().items)}

    def queue_notification(self, text: str, priority: str = "normal") -> dict[str, Any]:
        """Queue a message for delivery to pocket terminals.

        Devices poll on an interval and sleep in between, so delivery may be
        delayed by several minutes. Write the text with absolute time references
        ("Meeting with Alex at 10:00 AM"), never relative ones ("in 15 minutes"),
        which would be wrong by the time the user reads them.

        Args:
            text: One short sentence suitable for a small monochrome display.
            priority: "urgent" to chime on arrival, "normal" to display silently.
        """
        if not text or not text.strip():
            raise ValueError("notification text must not be empty")
        return {"id": self.notifications.enqueue(text.strip(), priority)}

    def get_device_status(self) -> list[dict[str, Any]]:
        """Report each terminal's last known battery level and contact time.

        `last_seen` is Unix epoch seconds. A device that has not been seen for
        many hours is probably out of range or discharged.
        """
        return [
            {"device_id": s.device_id, "battery": s.battery, "last_seen": s.last_seen}
            for s in self.devices.statuses()
        ]


def create_mcp_server(tools: HermesTools, name: str = "htp-bridge") -> FastMCP:
    # FastMCP's streamable_http_app() serves at streamable_http_path, which
    # defaults to "/mcp". The app is mounted under "/mcp" (main.create_full_app),
    # so leaving the default would put the endpoint at "/mcp/mcp" and 404 the
    # documented URL. Pin it to the mount root and let the mount own the prefix.
    server = FastMCP(name, streamable_http_path="/")
    server.add_tool(tools.publish_dashboard)
    server.add_tool(tools.queue_notification)
    server.add_tool(tools.get_device_status)
    return server
