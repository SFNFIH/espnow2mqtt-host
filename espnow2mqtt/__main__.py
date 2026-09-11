"""ESP-NOW ↔ MQTT bridge for Home Assistant (Zigbee2MQTT-style)."""

from __future__ import annotations

import argparse
import json
import logging
import signal
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import paho.mqtt.client as mqtt
import serial
from serial.tools import list_ports

LOG = logging.getLogger("espnow2mqtt")


@dataclass
class Device:
    mac: str
    name: str = ""
    model: str = "c3-env"
    online: bool = False
    rssi: Optional[int] = None
    hop: Optional[int] = None
    via: str = ""
    node_role: str = ""  # leaf | router
    caps: list[str] = field(default_factory=list)
    last_state: dict[str, Any] = field(default_factory=dict)
    discovered: bool = False
    discovery_sig: str = ""  # rebuild discovery when caps change



class Bridge:
    def __init__(
        self,
        port: str,
        mqtt_host: str,
        mqtt_port: int,
        mqtt_user: str | None,
        mqtt_pass: str | None,
        base_topic: str,
        discovery_prefix: str,
        baud: int = 115200,
        devices_path: Path | None = None,
        ha_discovery: bool = False,
    ) -> None:
        self.port = port
        self.baud = baud
        self.base = base_topic.rstrip("/")
        self.discovery_prefix = discovery_prefix.rstrip("/")
        self.ha_discovery = ha_discovery
        self.devices_path = devices_path or Path("data/devices.json")
        self.devices: dict[str, Device] = {}
        self.ser: Optional[serial.Serial] = None
        self.cmd_id = 1
        self._stop = threading.Event()
        self._lock = threading.Lock()

        self.mqtt = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"espnow2mqtt-{int(time.time())}",
            protocol=mqtt.MQTTv311,
        )
        if mqtt_user:
            self.mqtt.username_pw_set(mqtt_user, mqtt_pass or "")
        self.mqtt.will_set(f"{self.base}/bridge/state", "offline", retain=True)
        self.mqtt.on_connect = self._on_mqtt_connect
        self.mqtt.on_message = self._on_mqtt_message
        self.mqtt_host = mqtt_host
        self.mqtt_port = mqtt_port

        self._load_devices()

    def _slug(self, dev: Device) -> str:
        if dev.name:
            return dev.name.replace(" ", "_").lower()
        return dev.mac.replace(":", "").lower()

    def _load_devices(self) -> None:
        if not self.devices_path.exists():
            return
        try:
            raw = json.loads(self.devices_path.read_text(encoding="utf-8"))
            for mac, info in raw.items():
                self.devices[mac] = Device(
                    mac=mac,
                    name=info.get("name", ""),
                    model=info.get("model", "c3-env"),
                    node_role=info.get("node_role", ""),
                    online=False,
                )
            LOG.info("loaded %d devices from %s", len(self.devices), self.devices_path)
        except Exception as exc:  # noqa: BLE001
            LOG.warning("devices load failed: %s", exc)

    def _save_devices(self) -> None:
        self.devices_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            mac: {
                "name": d.name,
                "model": d.model,
                "node_role": d.node_role,
            }
            for mac, d in self.devices.items()
        }
        self.devices_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        self._publish_device_list()

    def _publish_device_list(self) -> None:
        lst = [
            {
                "mac": d.mac,
                "name": d.name or self._slug(d),
                "model": d.model,
                "online": d.online,
                "rssi": d.rssi,
                "hop": d.hop,
                "via": d.via,
                "node_role": d.node_role,
            }
            for d in self.devices.values()
        ]
        self.mqtt.publish(
            f"{self.base}/bridge/devices",
            json.dumps(lst),
            retain=True,
        )

    def start(self) -> None:
        LOG.info("connecting MQTT %s:%s", self.mqtt_host, self.mqtt_port)
        self.mqtt.connect(self.mqtt_host, self.mqtt_port, keepalive=60)
        self.mqtt.loop_start()

        LOG.info("opening serial %s @ %s", self.port, self.baud)
        self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
        time.sleep(0.5)
        self._serial_write({"type": "ping"})
        self._serial_write({"type": "list"})

        t = threading.Thread(target=self._serial_loop, name="serial", daemon=True)
        t.start()

        while not self._stop.is_set():
            time.sleep(0.5)

    def stop(self) -> None:
        self._stop.set()
        try:
            self.mqtt.publish(f"{self.base}/bridge/state", "offline", retain=True)
        except Exception:  # noqa: BLE001
            pass
        self.mqtt.loop_stop()
        self.mqtt.disconnect()
        if self.ser and self.ser.is_open:
            self.ser.close()

    def _serial_write(self, obj: dict[str, Any]) -> None:
        if not self.ser:
            return
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        with self._lock:
            self.ser.write(line.encode("utf-8"))
            self.ser.flush()

    def _serial_loop(self) -> None:
        assert self.ser is not None
        buf = ""
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(256)
                if not chunk:
                    continue
                buf += chunk.decode("utf-8", errors="ignore")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if line:
                        self._handle_serial(line)
            except Exception as exc:  # noqa: BLE001
                LOG.error("serial error: %s", exc)
                time.sleep(1)

    def _handle_serial(self, line: str) -> None:
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            LOG.debug("non-json: %s", line)
            return

        mtype = msg.get("type")
        if mtype == "hello":
            LOG.info("coordinator hello: %s", msg)
            self.mqtt.publish(f"{self.base}/bridge/state", "online", retain=True)
            self.mqtt.publish(
                f"{self.base}/bridge/info",
                json.dumps(msg),
                retain=True,
            )
            self._publish_bridge_discovery()
        elif mtype == "pong":
            LOG.debug("pong %s", msg)
        elif mtype == "log":
            LOG.info("coord: %s", msg.get("msg") or msg)
        elif mtype == "device":
            self._on_device_event(msg)
        elif mtype == "state":
            self._on_state(msg)
        elif mtype == "ack":
            LOG.info("ack: %s", msg)
        else:
            LOG.debug("ignored: %s", msg)

    def _ensure_device(self, mac: str) -> Device:
        if mac not in self.devices:
            self.devices[mac] = Device(mac=mac)
        return self.devices[mac]

    def _on_device_event(self, msg: dict[str, Any]) -> None:
        mac = msg.get("mac", "")
        if not mac:
            return
        dev = self._ensure_device(mac)
        event = msg.get("event")
        if msg.get("name"):
            dev.name = str(msg["name"])
        if msg.get("model"):
            dev.model = str(msg["model"])
        if msg.get("node_role"):
            dev.node_role = str(msg["node_role"])
        if msg.get("via"):
            dev.via = str(msg["via"])
        if "hop" in msg:
            try:
                dev.hop = int(msg["hop"])
            except (TypeError, ValueError):
                pass
        if "rssi" in msg:
            try:
                dev.rssi = int(msg["rssi"])
            except (TypeError, ValueError):
                pass
        if event == "online":
            dev.online = True
            LOG.info(
                "device online %s (%s) role=%s hop=%s via=%s",
                mac,
                dev.name or "-",
                dev.node_role or "-",
                dev.hop,
                dev.via or "-",
            )
            self._publish_discovery(dev)
            self.mqtt.publish(
                f"{self.base}/{self._slug(dev)}/availability",
                "online",
                retain=True,
            )
        elif event == "offline":
            dev.online = False
            LOG.info("device offline %s", mac)
            self.mqtt.publish(
                f"{self.base}/{self._slug(dev)}/availability",
                "offline",
                retain=True,
            )
        elif event == "info":
            if not dev.discovered:
                self._publish_discovery(dev)
        self._save_devices()

    def _parse_caps(self, payload: dict[str, Any], model: str = "") -> list[str]:
        caps_raw = payload.get("caps")
        caps: list[str] = []
        if isinstance(caps_raw, list):
            caps = [str(c).strip().lower() for c in caps_raw if str(c).strip()]
        elif isinstance(caps_raw, str) and caps_raw.strip():
            caps = [c.strip().lower() for c in caps_raw.split(",") if c.strip()]
        if caps:
            return caps
        # Infer from payload keys / model
        inferred: list[str] = []
        for key in ("temperature", "humidity", "switch", "contact", "button", "power", "energy"):
            if key in payload:
                inferred.append(key)
        model_l = (model or "").lower()
        if not inferred:
            if "th" in model_l or "temp" in model_l:
                inferred = ["temperature", "humidity"]
            elif "contact" in model_l or "door" in model_l:
                inferred = ["contact"]
            elif "plug" in model_l:
                inferred = ["switch", "power", "energy"]
            elif "switch" in model_l or "relay" in model_l:
                inferred = ["switch"]
        return inferred

    def _on_state(self, msg: dict[str, Any]) -> None:
        mac = msg.get("mac", "")
        if not mac:
            return
        dev = self._ensure_device(mac)
        if msg.get("via"):
            dev.via = str(msg["via"])
        if "hop" in msg:
            try:
                dev.hop = int(msg["hop"])
            except (TypeError, ValueError):
                pass
        payload = msg.get("payload") or {}
        if not isinstance(payload, dict):
            payload = {"value": payload}
        if payload.get("node_role"):
            dev.node_role = str(payload["node_role"])
        new_caps = self._parse_caps(payload, dev.model)
        if new_caps and new_caps != dev.caps:
            dev.caps = new_caps
            dev.discovered = False
        if not dev.online:
            dev.online = True
            self._publish_discovery(dev)
            self.mqtt.publish(
                f"{self.base}/{self._slug(dev)}/availability",
                "online",
                retain=True,
            )
        merged = dict(dev.last_state)
        merged.update(payload)
        if "switch" in merged:
            sw = str(merged["switch"]).upper()
            merged["switch"] = "ON" if sw in ("ON", "1", "TRUE") else "OFF"
        if "contact" in merged:
            c = str(merged["contact"]).upper()
            merged["contact"] = "ON" if c in ("ON", "1", "TRUE", "OPEN") else "OFF"
        if dev.hop is not None:
            merged["hop"] = dev.hop
        if dev.via:
            merged["via"] = dev.via
        if dev.caps:
            merged["caps"] = dev.caps
        dev.last_state = merged
        if not dev.discovered:
            self._publish_discovery(dev)
        topic = f"{self.base}/{self._slug(dev)}"
        self.mqtt.publish(f"{topic}/state", json.dumps(merged), retain=True)
        for key, val in merged.items():
            if key == "caps":
                continue
            self.mqtt.publish(f"{topic}/{key}", str(val), retain=False)

    def _on_mqtt_connect(
        self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any = None
    ) -> None:
        LOG.info("MQTT connected rc=%s", reason_code)
        client.subscribe(f"{self.base}/+/set")
        client.subscribe(f"{self.base}/bridge/request/+")
        client.publish(f"{self.base}/bridge/state", "online", retain=True)
        self._publish_bridge_discovery()
        for dev in self.devices.values():
            self._publish_discovery(dev)
        self._publish_device_list()

    def _on_mqtt_message(self, client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
        topic = msg.topic
        raw = msg.payload.decode("utf-8", errors="ignore")
        LOG.info("MQTT %s => %s", topic, raw)

        if topic == f"{self.base}/bridge/request/permit_join":
            seconds = 60
            try:
                body = json.loads(raw) if raw.startswith("{") else {}
                seconds = int(body.get("value", raw) or 60)
            except Exception:  # noqa: BLE001
                try:
                    seconds = int(raw)
                except Exception:  # noqa: BLE001
                    seconds = 60
            self._serial_write({"type": "pair", "seconds": seconds})
            return

        if topic.endswith("/set"):
            slug = topic[len(self.base) + 1 : -4]
            dev = self._find_by_slug(slug)
            if not dev:
                LOG.warning("unknown device slug %s", slug)
                return
            payload: dict[str, Any]
            try:
                payload = json.loads(raw)
                if not isinstance(payload, dict):
                    payload = {"switch": str(payload)}
            except json.JSONDecodeError:
                # accept bare ON/OFF
                payload = {"switch": raw.strip().upper()}
            cid = self.cmd_id
            self.cmd_id += 1
            self._serial_write(
                {"type": "cmd", "mac": dev.mac, "id": cid, "payload": payload}
            )

    def _find_by_slug(self, slug: str) -> Optional[Device]:
        for d in self.devices.values():
            if self._slug(d) == slug:
                return d
        # also accept raw mac without colons
        compact = slug.replace(":", "").upper()
        for d in self.devices.values():
            if d.mac.replace(":", "").upper() == compact:
                return d
        return None

    def _publish_bridge_discovery(self) -> None:
        if not self.ha_discovery:
            return
        # binary sensor for bridge availability
        cfg = {
            "name": "ESP-NOW Bridge",
            "unique_id": "espnow2mqtt_bridge_state",
            "state_topic": f"{self.base}/bridge/state",
            "payload_on": "online",
            "payload_off": "offline",
            "device_class": "connectivity",
            "device": {
                "identifiers": ["espnow2mqtt_bridge"],
                "name": "ESP-NOW 2 MQTT",
                "manufacturer": "espnow2mqtt",
                "model": "USB Coordinator Bridge",
                "sw_version": "0.1.0",
            },
        }
        self.mqtt.publish(
            f"{self.discovery_prefix}/binary_sensor/espnow2mqtt_bridge/config",
            json.dumps(cfg),
            retain=True,
        )

    def _publish_discovery(self, dev: Device) -> None:
        if not self.ha_discovery:
            # Entity creation is handled by the Home Assistant custom integration.
            dev.discovered = True
            return
        slug = self._slug(dev)
        ident = f"espnow2mqtt_{slug}"
        caps = list(dev.caps)
        if not caps:
            caps = self._parse_caps(dev.last_state, dev.model)
            dev.caps = caps
        sig = ",".join(sorted(caps)) + "|" + dev.model
        if dev.discovered and dev.discovery_sig == sig:
            return
        device = {
            "identifiers": [ident],
            "name": dev.name or slug,
            "manufacturer": "espnow2mqtt",
            "model": dev.model,
            "connections": [["mac", dev.mac.lower()]],
            "via_device": "espnow2mqtt_bridge",
        }
        avail = {
            "availability_topic": f"{self.base}/{slug}/availability",
            "payload_available": "online",
            "payload_not_available": "offline",
        }
        state_topic = f"{self.base}/{slug}/state"
        has = set(caps)

        # If still unknown, publish diagnostics only and wait for first state
        if not has:
            has = set()

        if "switch" in has:
            sw = {
                "name": "Switch",
                "unique_id": f"{ident}_switch",
                "command_topic": f"{self.base}/{slug}/set",
                "state_topic": state_topic,
                "value_template": "{{ value_json.switch }}",
                "payload_on": '{"switch":"ON"}',
                "payload_off": '{"switch":"OFF"}',
                "state_on": "ON",
                "state_off": "OFF",
                "device": device,
                **avail,
            }
            self.mqtt.publish(
                f"{self.discovery_prefix}/switch/{ident}/config",
                json.dumps(sw),
                retain=True,
            )

        if "temperature" in has:
            temp = {
                "name": "Temperature",
                "unique_id": f"{ident}_temperature",
                "state_topic": state_topic,
                "value_template": "{{ value_json.temperature }}",
                "unit_of_measurement": "°C",
                "device_class": "temperature",
                "state_class": "measurement",
                "device": device,
                **avail,
            }
            self.mqtt.publish(
                f"{self.discovery_prefix}/sensor/{ident}_temperature/config",
                json.dumps(temp),
                retain=True,
            )

        if "humidity" in has:
            hum = {
                "name": "Humidity",
                "unique_id": f"{ident}_humidity",
                "state_topic": state_topic,
                "value_template": "{{ value_json.humidity }}",
                "unit_of_measurement": "%",
                "device_class": "humidity",
                "state_class": "measurement",
                "device": device,
                **avail,
            }
            self.mqtt.publish(
                f"{self.discovery_prefix}/sensor/{ident}_humidity/config",
                json.dumps(hum),
                retain=True,
            )

        if "contact" in has:
            contact = {
                "name": "Contact",
                "unique_id": f"{ident}_contact",
                "state_topic": state_topic,
                "value_template": "{{ value_json.contact }}",
                "payload_on": "ON",
                "payload_off": "OFF",
                "device_class": "door",
                "device": device,
                **avail,
            }
            self.mqtt.publish(
                f"{self.discovery_prefix}/binary_sensor/{ident}_contact/config",
                json.dumps(contact),
                retain=True,
            )

        if "power" in has:
            power = {
                "name": "Power",
                "unique_id": f"{ident}_power",
                "state_topic": state_topic,
                "value_template": "{{ value_json.power }}",
                "unit_of_measurement": "W",
                "device_class": "power",
                "state_class": "measurement",
                "device": device,
                **avail,
            }
            self.mqtt.publish(
                f"{self.discovery_prefix}/sensor/{ident}_power/config",
                json.dumps(power),
                retain=True,
            )

        if "energy" in has:
            energy = {
                "name": "Energy",
                "unique_id": f"{ident}_energy",
                "state_topic": state_topic,
                "value_template": "{{ value_json.energy }}",
                "unit_of_measurement": "Wh",
                "device_class": "energy",
                "state_class": "total_increasing",
                "device": device,
                **avail,
            }
            self.mqtt.publish(
                f"{self.discovery_prefix}/sensor/{ident}_energy/config",
                json.dumps(energy),
                retain=True,
            )

        if "button" in has:
            btn_sensor = {
                "name": "Button",
                "unique_id": f"{ident}_button",
                "state_topic": state_topic,
                "value_template": "{{ value_json.button | default('none') }}",
                "device": device,
                **avail,
            }
            self.mqtt.publish(
                f"{self.discovery_prefix}/sensor/{ident}_button/config",
                json.dumps(btn_sensor),
                retain=True,
            )

        # Always publish light diagnostics
        rssi = {
            "name": "RSSI",
            "unique_id": f"{ident}_rssi",
            "state_topic": f"{self.base}/bridge/devices",
            "value_template": (
                "{% for d in value_json %}"
                f"{{% if d.mac == '{dev.mac}' %}}{{{{ d.rssi }}}}{{% endif %}}"
                "{% endfor %}"
            ),
            "unit_of_measurement": "dBm",
            "device_class": "signal_strength",
            "entity_category": "diagnostic",
            "device": device,
        }
        self.mqtt.publish(
            f"{self.discovery_prefix}/sensor/{ident}_rssi/config",
            json.dumps(rssi),
            retain=True,
        )
        hop = {
            "name": "Mesh Hop",
            "unique_id": f"{ident}_hop",
            "state_topic": state_topic,
            "value_template": "{{ value_json.hop | default(0) }}",
            "entity_category": "diagnostic",
            "device": device,
            **avail,
        }
        self.mqtt.publish(
            f"{self.discovery_prefix}/sensor/{ident}_hop/config",
            json.dumps(hop),
            retain=True,
        )
        role = {
            "name": "Node Role",
            "unique_id": f"{ident}_role",
            "state_topic": state_topic,
            "value_template": "{{ value_json.node_role | default('unknown') }}",
            "entity_category": "diagnostic",
            "device": device,
            **avail,
        }
        self.mqtt.publish(
            f"{self.discovery_prefix}/sensor/{ident}_role/config",
            json.dumps(role),
            retain=True,
        )
        dev.discovered = True
        dev.discovery_sig = sig


def autodetect_port() -> Optional[str]:
    keywords = ("esp32", "espressif", "cp210", "ch340", "usb jtag", "usb serial")
    for p in list_ports.comports():
        blob = f"{p.description} {p.manufacturer} {p.product}".lower()
        if any(k in blob for k in keywords):
            return p.device
    # common defaults
    for candidate in ("/dev/ttyACM0", "/dev/ttyUSB0", "/dev/serial/by-id"):
        path = Path(candidate)
        if path.is_dir():
            kids = sorted(path.iterdir())
            if kids:
                return str(kids[0])
        if path.exists():
            return str(path)
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="ESP-NOW to MQTT bridge")
    parser.add_argument("--port", default="", help="Serial device, empty = autodetect")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mqtt-host", default="localhost")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-user", default="")
    parser.add_argument("--mqtt-pass", default="")
    parser.add_argument("--base-topic", default="espnow2mqtt")
    parser.add_argument("--discovery-prefix", default="homeassistant")
    parser.add_argument(
        "--ha-discovery",
        action="store_true",
        help="Publish Home Assistant MQTT Discovery (default off; use HA integration instead)",
    )
    parser.add_argument(
        "--no-ha-discovery",
        action="store_true",
        help=argparse.SUPPRESS,  # legacy alias
    )
    parser.add_argument("--devices", default="data/devices.json")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    port = args.port or autodetect_port()
    if not port:
        LOG.error("no serial port found; pass --port /dev/ttyACM0")
        return 2

    bridge = Bridge(
        port=port,
        mqtt_host=args.mqtt_host,
        mqtt_port=args.mqtt_port,
        mqtt_user=args.mqtt_user or None,
        mqtt_pass=args.mqtt_pass or None,
        base_topic=args.base_topic,
        discovery_prefix=args.discovery_prefix,
        baud=args.baud,
        devices_path=Path(args.devices),
        ha_discovery=bool(args.ha_discovery) and not bool(args.no_ha_discovery),
    )

    def _sig(_signum: int, _frame: Any) -> None:
        LOG.info("stopping...")
        bridge.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    bridge.start()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
