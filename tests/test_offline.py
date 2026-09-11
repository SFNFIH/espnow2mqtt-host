#!/usr/bin/env python3
"""Offline checks for serial/MQTT JSON shapes (no hardware)."""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from espnow2mqtt.__main__ import Bridge, Device, PendingCommand  # noqa: E402


class FakeMqtt:
    """Records publishes so tests can assert on the topics and payloads."""

    def __init__(self) -> None:
        self.published: list[tuple[str, str, bool]] = []

    def publish(self, topic: str, payload: str, retain: bool = False) -> None:
        self.published.append((topic, payload, retain))

    def topic(self, suffix: str) -> dict | None:
        for topic, payload, _retain in self.published:
            if topic.endswith(suffix):
                return json.loads(payload)
        return None


def _bridge() -> Bridge:
    b = Bridge.__new__(Bridge)
    b.base = "espnow2mqtt"
    b.devices = {}
    b.pending = {}
    b.cmd_id = 1
    b.mqtt = FakeMqtt()
    return b


def test_slug() -> None:
    b = Bridge.__new__(Bridge)
    b.base = "espnow2mqtt"
    d = Device(mac="AA:BB:CC:DD:EE:FF", name="Living Room")
    assert b._slug(d) == "living_room"
    d2 = Device(mac="AA:BB:CC:DD:EE:FF")
    assert b._slug(d2) == "aabbccddeeff"


def test_protocol_examples_parse() -> None:
    samples = [
        '{"type":"hello","version":1,"role":"coordinator","mac":"AA:BB:CC:DD:EE:FF","fw":"0.1.0"}',
        '{"type":"device","event":"online","mac":"11:22:33:44:55:66","model":"c3-env","name":"node1"}',
        '{"type":"state","mac":"11:22:33:44:55:66","ts":1,"payload":{"temperature":23.4,"humidity":50,"switch":"OFF"}}',
        '{"type":"cmd","mac":"11:22:33:44:55:66","id":1,"payload":{"switch":"ON"}}',
    ]
    for s in samples:
        obj = json.loads(s)
        assert "type" in obj


def test_find_by_slug() -> None:
    b = Bridge.__new__(Bridge)
    b.devices = {
        "11:22:33:44:55:66": Device(mac="11:22:33:44:55:66", name="node1"),
    }
    assert b._find_by_slug("node1") is not None
    assert b._find_by_slug("112233445566") is not None
    assert b._find_by_slug("missing") is None


def test_parse_caps() -> None:
    b = Bridge.__new__(Bridge)
    assert b._parse_caps({"caps": ["temperature", "humidity"]}) == [
        "temperature",
        "humidity",
    ]
    assert b._parse_caps({"caps": "switch,power"}) == ["switch", "power"]
    assert b._parse_caps({"temperature": 1.0, "humidity": 2.0}) == [
        "temperature",
        "humidity",
    ]
    assert b._parse_caps({"switch": "ON"}) == ["switch"]
    assert b._parse_caps({}, model="ex-contact") == ["contact"]
    assert "switch" in b._parse_caps({}, model="ex-plug")
    assert "power" in b._parse_caps({}, model="ex-plug")


def test_inferred_caps_do_not_replace_explicit_ones() -> None:
    """A report that had no room for `caps` must not downgrade a light."""
    b = _bridge()
    dev = Device(mac="11:22:33:44:55:66", model="ex-dimmer")

    b._update_caps(dev, {"caps": ["light"], "switch": "ON", "brightness": 200})
    assert dev.caps == ["light"]

    # Same device, next report, `caps` dropped to fit the 160-byte budget. The
    # only inferable key is `switch`, which must not overwrite `light`.
    b._update_caps(dev, {"switch": "ON", "brightness": 200})
    assert dev.caps == ["light", "switch"]

    # An explicit list is authoritative and may shrink what we stored.
    b._update_caps(dev, {"caps": ["light"]})
    assert dev.caps == ["light"]


def test_failed_command_is_published() -> None:
    b = _bridge()
    b.devices["11:22:33:44:55:66"] = Device(mac="11:22:33:44:55:66", name="node1")
    b.pending[7] = PendingCommand(
        mac="11:22:33:44:55:66",
        slug="node1",
        payload={"switch": "ON"},
        sent_at=time.time(),
    )

    b._on_ack({"type": "ack", "mac": "11:22:33:44:55:66", "id": 7, "ok": False, "error": "timeout"})

    result = b.mqtt.topic("/node1/command_result")
    assert result is not None
    assert result["ok"] is False
    assert result["error"] == "timeout"
    assert result["payload"] == {"switch": "ON"}
    assert 7 not in b.pending, "the ack should clear the pending entry"


def test_successful_command_is_published() -> None:
    b = _bridge()
    b.devices["11:22:33:44:55:66"] = Device(mac="11:22:33:44:55:66", name="node1")
    b.pending[9] = PendingCommand(
        mac="11:22:33:44:55:66", slug="node1", payload={"switch": "OFF"}, sent_at=time.time()
    )

    b._on_ack({"type": "ack", "mac": "11:22:33:44:55:66", "id": 9, "ok": True})

    result = b.mqtt.topic("/node1/command_result")
    assert result is not None and result["ok"] is True
    # Command results describe a moment, so they must never be retained.
    assert all(retain is False for _t, _p, retain in b.mqtt.published)


def test_unattributable_ack_is_dropped() -> None:
    """An ack for a device we never saw has no slug to publish under."""
    b = _bridge()
    b._on_ack({"type": "ack", "mac": "99:99:99:99:99:99", "id": 1, "ok": False, "error": "send_fail"})
    assert b.mqtt.published == []


def test_pending_commands_expire() -> None:
    b = _bridge()
    b.pending[1] = PendingCommand(mac="a", slug="a", payload={}, sent_at=time.time() - 3600)
    b.pending[2] = PendingCommand(mac="b", slug="b", payload={}, sent_at=time.time())
    b._expire_pending()
    assert list(b.pending) == [2]


if __name__ == "__main__":
    test_slug()
    test_protocol_examples_parse()
    test_find_by_slug()
    test_parse_caps()
    test_inferred_caps_do_not_replace_explicit_ones()
    test_failed_command_is_published()
    test_successful_command_is_published()
    test_unattributable_ack_is_dropped()
    test_pending_commands_expire()
    print("ok")
