#!/usr/bin/env python3
"""Offline checks for serial/MQTT JSON shapes (no hardware)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from espnow2mqtt.__main__ import Bridge, Device  # noqa: E402


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


if __name__ == "__main__":
    test_slug()
    test_protocol_examples_parse()
    test_find_by_slug()
    test_parse_caps()
    print("ok")
