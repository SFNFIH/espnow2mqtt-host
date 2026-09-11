# ESP-NOW ↔ Serial Protocol (NDJSON) + Mesh — ESP-IDF

Coordinator talks to the host over **USB Serial/JTAG** (ESP-IDF `usb_serial_jtag`),
not over Wi-Fi. On-air protocol version **2** (mesh tree).

Hello includes `"stack":"esp-idf"`.

See `components/en2m/include/en2m_proto.h` for `en2m_pkt_t`.

Host ↔ USB commands unchanged: `ping`, `pair`, `list`, `unpair`, `cmd`.
Uplink JSON may include `hop`, `via`, `node_role`, `caps`, and flat HA fields.

## State uplink (compact)

Limited to `EN2M_DATA_MAX` (160) bytes. Example light:

```json
{"caps":["light"],"clusters":["on_off","level","color"],"node_role":"leaf","switch":"ON","brightness":200,"color_temp":300,"color_mode":"color_temp"}
```

## Set / command downlink

Flat (preferred for HA):

```json
{"switch":"ON","brightness":128,"color_temp":370}
{"cover":"CLOSE","position":100}
{"lock":"LOCK"}
{"fan_mode":"high","percentage":80}
{"hvac_mode":"cool","target_temperature":24}
```

Cluster-style:

```json
{"ep":1,"cluster":"window_covering","command":"open"}
{"ep":1,"cluster":"door_lock","command":"unlock"}
{"ep":1,"cluster":"fan_control","command":"set_percent","percentage":50}
{"ep":1,"cluster":"thermostat","command":"set_mode","mode":"heat"}
```
