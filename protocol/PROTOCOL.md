# ESP-NOW ↔ Serial Protocol (NDJSON) + Mesh — ESP-IDF

Coordinator talks to the host over **USB Serial/JTAG** (ESP-IDF `usb_serial_jtag`),
not over Wi-Fi. On-air protocol version **2** (mesh tree).

Hello includes `"stack":"esp-idf"`.

See `components/en2m/include/en2m_proto.h` for `en2m_pkt_t`.

Host ↔ USB commands unchanged: `ping`, `pair`, `list`, `unpair`, `cmd`.
Uplink JSON may include `hop`, `via`, `node_role`, `caps`, and flat HA fields.
`hop` and `via` are added by the coordinator, not by the device.

## State uplink (compact)

Limited to `EN2M_DATA_MAX` (160) bytes. Example light:

```json
{"node_role":"leaf","switch":"ON","brightness":200,"color_temp":370,"color_mode":"color_temp","caps":["light"]}
```

The device builds this from its attribute store. When the object does not fit
in one frame the optional parts are dropped in order — `node_role` first, then
`caps` — instead of truncating the JSON. The host merges successive reports for
a device, so a report that omits fields is not lossy.

## Set / command downlink

Flat (preferred for HA):

```json
{"switch":"ON","brightness":128,"color_temp":370}
{"cover":"CLOSE","position":100}
{"lock":"LOCK"}
{"fan_mode":"high","percentage":80}
{"hvac_mode":"cool","target_temperature":24}
{"identify":10}
```

`target_temperature` is applied to the cooling setpoint while the thermostat is
in `cool`, and to the heating setpoint otherwise.

Cluster-style, which also selects the endpoint explicitly:

```json
{"ep":1,"cluster":"on_off","command":"toggle"}
{"ep":1,"cluster":"level_control","command":"move_to_level","level":200}
{"ep":1,"cluster":"window_covering","command":"open"}
{"ep":1,"cluster":"door_lock","command":"unlock"}
{"ep":1,"cluster":"fan_control","command":"set_percent","percentage":50}
{"ep":1,"cluster":"thermostat","command":"set_mode","mode":"heat"}
{"ep":1,"cluster":"identify","command":"identify","seconds":10}
```

Omit `ep` to target the lowest endpoint that exposes the cluster.

## Acknowledgement and retries

A `cmd` line from the host carries an `id`. The coordinator retransmits that
frame (`EN2M_CMD_RETRIES` times, `EN2M_CMD_RETRY_MS` apart) until the device
acknowledges it, then reports the result:

```json
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":true}
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":false,"error":"timeout"}
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":false,"error":"send_fail"}
```

A device acknowledges every CMD frame that carries a non-zero `id` and then
sends a fresh state report, so the host sees both the confirmation and the new
state. An `id` of `0` means fire-and-forget with no retries.
