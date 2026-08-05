from __future__ import annotations

import hmac
import time
from dataclasses import dataclass
from typing import Callable

from htp_bridge.config import DeviceConfig
from htp_bridge.db import Database


@dataclass(frozen=True)
class DeviceStatus:
    device_id: str
    battery: int | None
    last_seen: int | None


class DeviceRegistry:
    def __init__(
        self,
        db: Database,
        devices: list[DeviceConfig],
        clock: Callable[[], int] = lambda: int(time.time()),
    ) -> None:
        self._db = db
        self._devices = list(devices)
        self._clock = clock

    def authenticate(self, token: str) -> DeviceConfig | None:
        """Return the device owning this token, comparing in constant time."""
        if not token:
            return None
        for device in self._devices:
            if hmac.compare_digest(device.token, token):
                return device
        return None

    def record_telemetry(self, device_id: str, battery: int | None) -> None:
        if battery is not None and not 0 <= battery <= 100:
            battery = None
        now = self._clock()
        with self._db.write() as conn:
            conn.execute(
                """
                INSERT INTO device_status (device_id, battery, last_seen) VALUES (?,?,?)
                ON CONFLICT(device_id) DO UPDATE SET
                    battery = COALESCE(excluded.battery, device_status.battery),
                    last_seen = excluded.last_seen
                """,
                (device_id, battery, now),
            )

    def statuses(self) -> list[DeviceStatus]:
        with self._db.read() as conn:
            rows = conn.execute(
                "SELECT device_id, battery, last_seen FROM device_status ORDER BY device_id"
            ).fetchall()
        return [DeviceStatus(r["device_id"], r["battery"], r["last_seen"]) for r in rows]
