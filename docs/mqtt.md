# MQTT 主题与 payload 参考

Bridge 和 Home Assistant 之间只通过 MQTT 说话。这份文档是**每一个主题的完整参考**。

- 默认主题前缀 `<base>` = `espnow2mqtt`（`--base-topic` 可改）
- 默认 discovery 前缀 `<disc>` = `homeassistant`（`--discovery-prefix` 可改，
  **只在 `--ha-discovery` 开启时用到**）
- `<slug>` 的算法见 [bridge.md §3](bridge.md#3-slugmqtt-主题里的设备名)

> **`--base-topic` 必须和 HA 集成里配的一致。** 这是"HA 里什么都没有"最常见的原因。

目录：

1. [主题总表](#1-主题总表)
2. [`bridge/state`](#2-bridgestate)
3. [`bridge/info`](#3-bridgeinfo)
4. [`bridge/devices`](#4-bridgedevices)
5. [`bridge/request/permit_join`](#5-bridgerequestpermit_join)
6. [`<slug>/availability`](#6-slugavailability)
7. [`<slug>/state`](#7-slugstate)
8. [设备状态字段](#8-设备状态字段)
9. [`<slug>/<key>` 扁平主题](#9-slugkey-扁平主题)
10. [`<slug>/set`](#10-slugset)
11. [`<slug>/command_result`](#11-slugcommand_result)
12. [MQTT Discovery 主题](#12-mqtt-discovery-主题)
13. [retain 语义总结](#13-retain-语义总结)
14. [常用命令行](#14-常用命令行)

---

## 1. 主题总表

**Bridge 发布**

| 主题 | retain | QoS | 内容 |
|---|:-:|:-:|---|
| `<base>/bridge/state` | ✓ | 0 | `online` / `offline` |
| `<base>/bridge/info` | ✓ | 0 | S3 的 `hello` JSON 原样 |
| `<base>/bridge/devices` | ✓ | 0 | 设备列表 JSON 数组 |
| `<base>/<slug>/availability` | ✓ | 0 | `online` / `offline` |
| `<base>/<slug>/state` | ✓ | 0 | 合并后的状态 JSON |
| `<base>/<slug>/<key>` | ✗ | 0 | 单个字段的 `str()` 值 |
| `<base>/<slug>/command_result` | ✗ | 0 | 一条下行命令的结局 |
| `<disc>/…/config` | ✓ | 0 | MQTT Discovery，**仅 `--ha-discovery`** |

**Bridge 订阅**

| 主题 | 处理 |
|---|---|
| `<base>/+/set` | 设备命令 → USB `cmd` |
| `<base>/bridge/request/+` | **只有 `permit_join` 被处理**，其他静默丢弃 |

所有 publish 都用 paho 的默认 **QoS 0**，代码里没有任何 `qos=` 参数。
对本系统够用：状态是幂等的（retained + 周期重发），命令丢了 HA 侧会看到状态没变。

---

## 2. `bridge/state`

```
espnow2mqtt/bridge/state  →  "online" | "offline"       (retained)
```

Bridge 自身的存活标志。**HA 集成用它做整个 hub 的 availability。**

写入点有四个：

| 时机 | 值 | 谁写 |
|---|---|---|
| MQTT 连上（含自动重连） | `online` | `_on_mqtt_connect` |
| 收到 S3 的 `hello` | `online` | `_handle_serial` |
| `stop()`（SIGINT/SIGTERM） | `offline` | Bridge 主动 |
| Bridge 进程异常死亡 | `offline` | **MQTT LWT**，broker 代发 |

LWT 在 `__init__` 里注册：

```python
self.mqtt.will_set(f"{self.base}/bridge/state", "offline", retain=True)
```

> **⚠️ `online` 不代表 S3 在线**
>
> `_on_mqtt_connect` **无条件**发 `online`，此时它还没和 S3 说过一句话。
> 所以"`bridge/state = online` 但一个设备都没有"完全可能意味着**串口选错了**
> 或者**S3 没在跑**。
>
> 判断 S3 是否真的活着，看 `bridge/info` 有没有内容、以及 Bridge 日志里
> 有没有 `coord: coordinator ready`。`bridge/info` 是 retained 的，
> 但它**只在收到 `hello` 后才会有值**——所以：
>
> | `bridge/state` | `bridge/info` | 含义 |
> |---|---|---|
> | `online` | 有内容 | 一切正常 |
> | `online` | **空/不存在** | Bridge 活着但从没收到过 S3 的 hello → 串口错 / S3 没跑 |
> | `offline` | 有内容 | Bridge 挂了，`info` 是上次的残留 |
>
> 注意 `bridge/info` 是 retained 的，Bridge 重启也**不会清掉**它——所以
> "有内容"可能是很久以前留下的。要确认新鲜度，重启 Bridge 后看日志。

---

## 3. `bridge/info`

```
espnow2mqtt/bridge/info  →  {"type":"hello","version":2,...}    (retained)
```

S3 的 `hello` 行**原样转发**（连 `"type":"hello"` 都留着）。字段含义见
[usb-protocol.md §3](usb-protocol.md#3-s3--主机hello)。

```json
{
  "type": "hello",
  "version": 2,
  "role": "coordinator",
  "mac": "7C:DF:A1:00:11:22",
  "fw": "0.4.0-idf",
  "channel": 1,
  "mesh": true,
  "stack": "esp-idf"
}
```

最有用的两个字段：

- **`channel`** — S3 实际在用的 ESP-NOW 信道。设备侧固件的
  `CONFIG_EN2M_WIFI_CHANNEL` 必须是同一个值，否则设备永远上不了线。
- **`fw`** — 协调器固件版本，升级后确认真的刷进去了。

每 30 s 会被重写一次（S3 的周期 `hello`），但内容不变。

---

## 4. `bridge/devices`

```
espnow2mqtt/bridge/devices  →  [{...}, {...}]                   (retained)
```

Bridge 已知的**全部**设备（包括离线的）。JSON 数组：

```json
[
  {
    "mac": "AA:BB:CC:DD:EE:FF",
    "name": "living_room",
    "model": "c3-light",
    "online": true,
    "rssi": -58,
    "hop": 1,
    "via": "7C:DF:A1:00:11:22",
    "node_role": "leaf"
  },
  {
    "mac": "AA:BB:CC:DD:EE:01",
    "name": "aabbccddee01",
    "model": "c3-env",
    "online": false,
    "rssi": -81,
    "hop": 2,
    "via": "AA:BB:CC:DD:EE:FF",
    "node_role": "leaf"
  }
]
```

| 字段 | 可能为 null？ | 说明 |
|---|:-:|---|
| `mac` | ✗ | 大写带冒号 |
| `name` | ✗ | `d.name or self._slug(d)`——**没名字的设备显示紧凑 MAC，不是空串** |
| `model` | ✗ | 默认 `"c3-env"` |
| `online` | ✗ | bool |
| `rssi` | ✓ | Bridge 重启后、设备还没上报时是 `null` |
| `hop` | ✓ | 同上 |
| `via` | ✗ | 未知时是 `""` |
| `node_role` | ✗ | 未知时是 `""`（不是 `"unknown"`） |

**刷新时机**：每次 `_save_devices()`，也就是**每一条 `device` 行**。
每个设备每 30 s 一条心跳，所以这个主题的重写频率 ≈ 设备数 / 30 s。
32 个设备的话大约每秒一次。

**离线设备的 `rssi`/`hop`/`via` 是掉线前的最后已知值**，因为 S3 的 `offline`
事件不带这些字段（见 [usb-protocol.md §4](usb-protocol.md#4-s3--主机device)）。

`--ha-discovery` 开启时，RSSI 诊断实体的 `state_topic` 就是这个主题，
`value_template` 是个 Jinja 循环：

```jinja
{% for d in value_json %}{% if d.mac == 'AA:BB:CC:DD:EE:FF' %}{{ d.rssi }}{% endif %}{% endfor %}
```

每个设备的 RSSI 实体都要遍历整个数组。设备多了会有一点模板开销，
这也是推荐用 HA 集成而不是内置 discovery 的原因之一。

**这个主题没有"删除设备"的机制。** 设备一旦进过 `self.devices` 就会一直在列表里，
除了两种情况：

1. 手工发 `unpair` 到串口（S3 会发 `offline`，但 **Bridge 不会从 `self.devices` 里删**——
   只是 `online` 变 false）
2. 停掉 Bridge，手工编辑 `devices.json` 删掉那个 MAC，再启动

---

## 5. `bridge/request/permit_join`

```
espnow2mqtt/bridge/request/permit_join  ←  60 | {"value":60}
```

**这是 Bridge 唯一的输入控制主题。** 打开 S3 的配网窗口。

| 发什么 | `seconds` |
|---|---|
| `60` | 60 |
| `{"value":60}` | 60 |
| `{"value":"120"}` | 120 |
| `""` / `{}` / 任何解析不出来的东西 | **60**（兜底） |

Bridge 转成串口行 `{"type":"pair","seconds":<n>}`，S3 把它夹到 `[1, 300]`。
所以发 `99999` 实际得到 300 秒。

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60
```

**没有响应主题。** 成功与否只能在 Bridge 日志里看：

```
INFO espnow2mqtt: MQTT espnow2mqtt/bridge/request/permit_join => 60
INFO espnow2mqtt: coord: pairing_enabled
```

窗口关闭时：

```
INFO espnow2mqtt: coord: pairing_disabled
```

> **`bridge/request/unpair` 不存在。**
> Bridge 订阅了 `bridge/request/+`，但 `_on_mqtt_message` 里只匹配
> `permit_join`，其余**静默丢弃**。S3 支持 `unpair`，但只能手工往串口发
> （见 [usb-protocol.md §15](usb-protocol.md#15-手工调试)）。

**不要往这个主题发 retained 消息。** retained 的 `permit_join` 会在每次
Bridge 重连 MQTT 时重新投递，于是配网窗口每次重启都自动打开。
`mosquitto_pub -r` 在这里是个陷阱。清理：

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -r -n
```

---

## 6. `<slug>/availability`

```
espnow2mqtt/living_room/availability  →  "online" | "offline"    (retained)
```

单个设备的在线状态。

| 值 | 触发 |
|---|---|
| `online` | S3 的 `device`/`online` 事件；**或**收到该设备的任何状态而 Bridge 之前认为它离线 |
| `offline` | S3 的 `device`/`offline` 事件（`EN2M_OFFLINE_MS = 90000` ms 没收到任何帧） |

**Bridge 自己不做超时判定**，`offline` 只能来自 S3。所以：

> **⚠️ Bridge 挂掉时，设备的 `availability` 会停在 `online`**
>
> `bridge/state` 会变成 `offline`（LWT），但每个设备的
> `<slug>/availability` 还是 retained 的 `online`——因为没人去改它。
>
> HA 集成的做法是把 hub 的可用性和设备的可用性**串联**：
> `bridge/state == offline` 时所有实体都标为不可用。如果你在手搓自动化，
> 记得同时看这两个主题，别只看 `<slug>/availability`。

**Bridge 重启后**，`self.devices` 里所有设备的 `online` 都是 `False`
（`_load_devices` 硬编码 `online=False`），但 Bridge **不会主动发 `offline`**——
它只在状态变化时发。所以 broker 上的 retained `online` 保持不变，直到：

- 设备下一次上报 → `_on_state` 里 `if not dev.online` 命中 → 重发 `online`（值没变）
- 或者 S3 判它离线 → 发 `offline`

这个行为是合理的（重启不该让所有设备闪一下不可用），但意味着
**`<slug>/availability` 的新鲜度不如 `bridge/state`**。

---

## 7. `<slug>/state`

```
espnow2mqtt/living_room/state  →  {...}                          (retained)
```

**这是整个系统最重要的主题，也是 HA 集成唯一读的设备主题。**

内容是 Bridge 的 `merged` 字典——所有历史上报的**叠加**结果，不是单次上报。
合并逻辑见 [bridge.md §6](bridge.md#6-状态上行_on_state-的合并逻辑)。

一个调光灯：

```json
{
  "node_role": "leaf",
  "switch": "ON",
  "brightness": 200,
  "color_temp": 370,
  "color_mode": "color_temp",
  "caps": ["light"],
  "hop": 1,
  "via": "7C:DF:A1:00:11:22"
}
```

一个温湿度传感器：

```json
{
  "node_role": "leaf",
  "temperature": 23.45,
  "humidity": 51.2,
  "caps": ["temperature", "humidity"],
  "hop": 2,
  "via": "AA:BB:CC:DD:EE:FF"
}
```

三类字段：

| 来源 | 字段 |
|---|---|
| **设备上报** | 业务字段（见 [§8](#8-设备状态字段)）、`node_role`、`caps` |
| **S3 注入** | `hop`、`via`（每次发布时用 `dev.hop`/`dev.via` 覆盖） |
| **Bridge 注入** | `caps`（设备没报时用推断值）；`switch`/`contact` 的归一化 |

`hop`/`via`/`caps` 每次都重新注入，所以**即使某次上报把它们挤掉了，
MQTT 上也一定有**（前提是 Bridge 之前见过它们）。

---

## 8. 设备状态字段

下面是 `en2m` 组件能产生的**全部**业务字段。这张表是
device 仓库 `docs/reporting.md` 里 cluster→JSON 映射的主机侧视角。

| JSON 键 | 类型 | 取值 / 单位 | 来自哪个 cluster | Bridge 归一化 | 附带 `caps` |
|---|---|---|---|:-:|---|
| `switch` | string | `"ON"` / `"OFF"` | OnOff | **✓** | `switch` |
| `brightness` | number | 0–254 | LevelControl | | `light` |
| `color_temp` | number | mired | ColorControl | | `light` |
| `color_mode` | string | `"color_temp"` | ColorControl（固定值） | | |
| `contact` | string | `"ON"` / `"OFF"` | BooleanState | **✓** | `contact` |
| `occupancy` | string | `"ON"` / `"OFF"` | Occupancy | | `occupancy` |
| `illuminance` | number | lux | Illuminance | | `illuminance` |
| `temperature` | number | °C（组件已 ÷100） | TemperatureMeasurement | | `temperature` |
| `humidity` | number | %（已 ÷100） | RelativeHumidity | | `humidity` |
| `pressure` | number | hPa（已 ÷10） | PressureMeasurement | | `pressure` |
| `smoke` | string | `"ON"` / `"OFF"` | SmokeCO | | `smoke` |
| `carbon_monoxide` | string | `"ON"` / `"OFF"` | SmokeCO | | `carbon_monoxide` |
| `power` | number | W（已 ÷1000） | ElectricalPower | | `power` |
| `energy` | number | Wh（已 ÷1000） | ElectricalPower | | `energy` |
| `fan_mode` | string | `"off"`/`"low"`/`"medium"`/`"high"`/`"on"`/`"auto"` | FanControl | | `fan` |
| `percentage` | number | 0–100 | FanControl | | |
| `position` | number | 0–100 | WindowCovering | | `cover` |
| `cover` | string | `"OPEN"` / `"CLOSED"`（position ≥ 95 算 CLOSED） | WindowCovering（推导） | | |
| `lock` | string | `"LOCKED"` / `"UNLOCKED"` | DoorLock | | `lock` |
| `hvac_mode` | string | `"off"`/`"heat"`/`"cool"`/`"auto"`/… | Thermostat | | `climate` |
| `current_temperature` | number | °C（已 ÷100） | Thermostat | | |
| `target_temperature` | number | °C（已 ÷100） | Thermostat（按当前模式挑 setpoint） | | |
| `button` | string | 设备自定义 | 应用层自己塞 | | `button` |

拓扑/元数据字段：

| JSON 键 | 类型 | 说明 |
|---|---|---|
| `node_role` | string | `"leaf"` / `"router"`。**160 字节不够时第一个被丢** |
| `caps` | array 或 string | 能力列表。**第二个被丢**。可能是 `["light"]` 也可能是 `"light"`（省字节） |
| `hop` | number | 跳数，S3 注入 |
| `via` | string | 上一跳 MAC，S3 注入 |
| `raw` | string | **故障信号**：设备的 payload 不是合法 JSON，被 S3 包了一层。见 [usb-protocol.md §5](usb-protocol.md#5-s3--主机state) |
| `value` | any | **故障信号**：`payload` 不是 JSON 对象，Bridge 包成 `{"value": …}` |

### 只有两个字段被归一化

| 字段 | 认作 `"ON"` | 其他 |
|---|---|---|
| `switch` | `"ON"` / `"1"` / `"TRUE"` | `"OFF"` |
| `contact` | `"ON"` / `"1"` / `"TRUE"` / `"OPEN"` | `"OFF"` |

**`cover` 是 `"OPEN"`/`"CLOSED"`，`lock` 是 `"LOCKED"`/`"UNLOCKED"`，
`occupancy`/`smoke` 是 `"ON"`/`"OFF"`——这些都原样透传，没有归一化。**
写自动化时按这张表的字面值来。

### 三个换算已经在设备侧做完了

`temperature` / `humidity` / `pressure` / `power` / `energy` 在**设备侧**
序列化时就已经除过了（÷100、÷100、÷10、÷1000、÷1000）。
MQTT 上看到的是**人类单位**（°C / % / hPa / W / Wh），不是 Matter 的定点整数。
Bridge 和 HA 都不需要再换算。

---

## 9. `<slug>/<key>` 扁平主题

```
espnow2mqtt/living_room/switch      →  "ON"          (NOT retained)
espnow2mqtt/living_room/brightness  →  "200"         (NOT retained)
espnow2mqtt/living_room/hop         →  "1"           (NOT retained)
```

`merged` 里**除 `caps` 以外**的每个字段，各发一个主题，值是 Python 的 `str(val)`。

这是给手搓自动化 / Node-RED / `mosquitto_sub` 调试用的**方便主题**。
两个必须知道的限制：

### 9.1 值是 `str()` 不是 JSON

| Python 值 | 发出去的字符串 | JSON 会是 |
|---|---|---|
| `"ON"` | `ON` | `"ON"` |
| `200` | `200` | `200` |
| `23.45` | `23.45` | `23.45` |
| `True` | `True` | `true` ⚠️ |
| `None` | `None` | `null` ⚠️ |
| `["light"]` | `['light']` | `["light"]` ⚠️ |

所以**别在扁平主题上解析复杂值**。`caps` 被显式跳过就是因为这个。
权威数据源永远是 `<slug>/state`。

### 9.2 不 retain

新订阅者订上来**不会立刻收到值**，得等下一次上报——叶子节点默认
`EN2M_REPORT_INTERVAL_LEAF_MS = 30000` ms。

要"订上来就有值"，用 `<slug>/state`。

### 9.3 每次上报全量重发

因为发布的是 `merged` 而不是本次 `payload`，**每条上报都会把所有扁平主题重发一遍**，
即使值没变。一个 8 字段的设备每 30 s 产生 8 条 MQTT 消息。
这对 Mosquitto 完全无所谓，但如果你在 broker 上做了消息计数告警，别被吓到。

---

## 10. `<slug>/set`

```
espnow2mqtt/living_room/set  ←  {"switch":"ON","brightness":128}
```

**唯一的设备控制入口。** Bridge 转成串口 `{"type":"cmd","mac":…,"id":<自增>,"payload":<原样>}`。

### payload 的三种写法

**1. JSON 对象（正常路径，原样透传）**

```bash
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"switch":"ON"}'
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"switch":"ON","brightness":128,"color_temp":370}'
mosquitto_pub -t espnow2mqtt/bedroom_cover/set -m '{"cover":"CLOSE"}'
mosquitto_pub -t espnow2mqtt/front_door/set -m '{"lock":"LOCK"}'
mosquitto_pub -t espnow2mqtt/ceiling_fan/set -m '{"fan_mode":"high","percentage":80}'
mosquitto_pub -t espnow2mqtt/living_ac/set -m '{"hvac_mode":"cool","target_temperature":24}'
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"identify":10}'
```

**2. Cluster 风格（可指定端点）**

```bash
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"ep":1,"cluster":"on_off","command":"toggle"}'
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"ep":1,"cluster":"level_control","command":"move_to_level","level":200}'
```

**3. 裸值（只能开关）**

| 发什么 | Bridge 转成 |
|---|---|
| `ON` | `{"switch":"ON"}` |
| `on` | `{"switch":"ON"}`（转大写） |
| `OFF` | `{"switch":"OFF"}` |
| `1` | `{"switch":"1"}` ⚠️ 见下 |

```bash
mosquitto_pub -t espnow2mqtt/living_room/set -m ON
```

`1` 这一条走的是另一条代码路径：`json.loads("1")` **成功**（得到 int `1`），
不是 dict，于是 `{"switch": str(1)}` = `{"switch":"1"}`——**没经过大写转换**。
设备侧会把 `"1"` 当 ON 处理，所以结果是对的，只是绕了一圈。

### Bridge 对 payload 零校验

`{"brightness":"洗衣机"}` 会被原样送到设备，由**设备侧的写回调**拒绝
（返回非 `ESP_OK` → 属性不提交 → 状态不变）。Bridge 不知道有哪些属性、
取值范围是什么，那是数据模型层的职责。

后果：**发一个非法命令不会有任何 MQTT 上的错误反馈**。你会看到：

- Bridge 日志里 `MQTT espnow2mqtt/living_room/set => {"brightness":"洗衣机"}`
- 1.6 s 内一条 `ack ok:true`（设备确实收到了命令，只是拒绝了那个值）
- `<slug>/state` 里 `brightness` 没变

### `id` 和重传

Bridge 用自增的 `cmd_id`（从 1 开始），**非零**，所以 S3 会重传
4 次（0/400/800/1200 ms），1.6 s 后超时。

**MQTT 路径上没法发 fire-and-forget 命令**（`id:0`），那只能手工往串口写。

### 失败怎么看

命令的结局会回到 MQTT 上，见 [§11](#11-slugcommand_result)。

Bridge 日志里同时有一行：

```
WARNING espnow2mqtt: command 7 to living_room failed: timeout
WARNING espnow2mqtt: command 8 to living_room failed: send_fail
WARNING espnow2mqtt: unknown device slug old_name
```

| 日志 | 含义 |
|---|---|
| `failed: timeout` | 命令发出去了，设备 1.6 s 内没确认。设备断电/信号差/中继掉线 |
| `failed: send_fail` | S3 根本没发出去。路由表里没这个 MAC，或 pending 表满 |
| `unknown device slug X` | Bridge 的设备表里没有叫 `X` 的设备。最常见是 HA 里留着一个改过名/已移除的旧实体 |

最后一种**不会**产生 `command_result`——命令根本没被发出去，
也没有 `id` 可言。

### 别往 `<slug>/set` 发 retained 消息

retained 的命令会在 Bridge 每次重连 MQTT 时**重新投递**，于是重启就自动开灯。
清理：

```bash
mosquitto_pub -t espnow2mqtt/living_room/set -r -n
```

`<base>/+/set` 这个订阅也会匹配 `<base>/bridge/set`。往那里发东西会让
`_find_by_slug("bridge")` 失败，只产生一条 `unknown device slug bridge` 警告。

---

## 11. `<slug>/command_result`

```
espnow2mqtt/living_room/command_result
```

| | |
|---|---|
| 方向 | **Bridge 发布** |
| retain | **✗ 绝对不 retain** |
| QoS | 0 |
| 什么时候发 | 每收到一条能归属到设备的 `ack` USB 行 |
| 发布者 | `_on_ack()`（见 [bridge.md](bridge.md#5-串口读取_serial_loop-与-_handle_serial)） |

一条下行命令的结局。**成功和失败都发。**

```json
{"id": 7, "ok": true, "mac": "AA:BB:CC:DD:EE:FF",
 "payload": {"switch": "ON"}, "elapsed_ms": 142}
```

```json
{"id": 8, "ok": false, "mac": "AA:BB:CC:DD:EE:FF", "error": "timeout",
 "payload": {"brightness": 200}, "elapsed_ms": 1642}
```

| 字段 | 类型 | 总是有 | 含义 |
|---|---|:-:|---|
| `id` | number | ✓ | 命令序号，和 `<slug>/set` 触发的那条 USB `cmd` 行的 `id` 一致 |
| `ok` | bool | ✓ | 协调器是否收到了设备的链路层确认 |
| `mac` | string \| null | ✓ | 目标 MAC。`send_fail` 时协调器可能不填，Bridge 会用 `pending` 里的值补上；都没有则为 `null` |
| `error` | string | 失败时 | `timeout` / `send_fail` / … 原样转发协调器的字符串 |
| `payload` | object | 见下 | **原始命令内容**，即 `<slug>/set` 的 payload |
| `elapsed_ms` | number | 见下 | 从 publish 到 ack 的往返耗时 |

### 11.1 `payload` 和 `elapsed_ms` 可能缺失

这两个字段来自 Bridge 的 `pending` 表，那个表有 **30 秒 TTL**。
协调器在命令中途复位、ack 迟到超过 30 秒的话，条目已经被清掉了，
这两个字段就不会出现。`id` / `ok` / `mac` / `error` 始终在。

订阅方要按可选字段处理：

```jinja
{{ value_json.payload | default({}) }}
```

### 11.2 为什么不 retain

命令结果是**一次性事件**，不是状态。retain 了会有两个后果：

1. HA 每次重连都会重新收到最后一条结果，于是**重启就弹一次"命令失败"告警**
2. 多个订阅者看到的"最新结果"可能早就过期了

规则和 `<slug>/set` 一样：**状态 retain，命令和命令结果不 retain。**

### 11.3 `ok: true` 不等于"设备照办了"

`ok` 反映的是**链路层**：协调器把帧送到了设备并收到了 ESP-NOW 确认。
设备的应用层完全可以在那之后拒绝这个值——写回调返回非 `ESP_OK`，
属性不提交，状态也就不变。

| 现象 | `command_result` |
|---|---|
| 状态变了 | `ok: true` |
| 状态不变，因为命令没送到 | `ok: false` |
| 状态不变，因为设备**拒绝**了那个值 | **`ok: true`** |

第三行的常见原因：值超出范围、往只读属性写、设备上没有那个属性
（往不支持色温的灯发 `color_temp`）、枚举值固件没实现
（风扇的 `smart`）。这些只能在设备的 `idf.py monitor` 里看到。

所以关键操作要**同时**看 `command_result` 和 `<slug>/state`。

### 11.4 无法归属的 ack 不会发布

如果 ack 里的 `mac` 查不到设备、`pending` 里也没有对应的 `id`，
Bridge 只记日志、不发 MQTT。那种 ack 通常是**协调器自己的串口控制台
发出的命令**产生的，和 Bridge 无关，发出去只会让订阅者困惑。

### 11.5 订阅示例

```bash
# 看所有命令的结局
mosquitto_sub -t 'espnow2mqtt/+/command_result' -v

# 只看失败的
mosquitto_sub -t 'espnow2mqtt/+/command_result' -v \
  | grep '"ok": false'
```

HA 集成订阅这个主题，把失败的转成 `espnow2mqtt_command_failed`
事件，见
[ha 仓库 docs/usage.md §7](https://github.com/SFNFIH/espnow2mqtt-ha/blob/main/docs/usage.md#7-命令的成败反馈)。

> **0.3.x 的 Bridge 不发这个主题。** 协调器的 ack 到了 `_on_ack`
> 就停下了，只写一行日志。命令失败在这个进程之外完全不可见。
> 如果你的 HA 集成是 0.4.0 但 Bridge 是旧版，
> 集成会正常工作，只是永远收不到 `command_result`，
> 也就永远不会 fire 失败事件。

---

## 12. MQTT Discovery 主题

**只在 `--ha-discovery` 开启时发布。默认关闭。**
为什么默认关、覆盖面有多小，见 [bridge.md §9](bridge.md#9-ha_discovery为什么默认关)。

### Bridge 自身

```
<disc>/binary_sensor/espnow2mqtt_bridge/config                   (retained)
```

```json
{
  "name": "ESP-NOW Bridge",
  "unique_id": "espnow2mqtt_bridge_state",
  "state_topic": "espnow2mqtt/bridge/state",
  "payload_on": "online",
  "payload_off": "offline",
  "device_class": "connectivity",
  "device": {
    "identifiers": ["espnow2mqtt_bridge"],
    "name": "ESP-NOW 2 MQTT",
    "manufacturer": "espnow2mqtt",
    "model": "USB Coordinator Bridge",
    "sw_version": "0.1.0"
  }
}
```

设备实体都用 `"via_device": "espnow2mqtt_bridge"` 挂在它下面，
所以 HA 的设备页会显示正确的层级关系。

### 设备实体

`ident = f"espnow2mqtt_{slug}"`。**按 caps 条件发布：**

| cap | 主题 | 平台 | 关键字段 |
|---|---|---|---|
| `switch` | `<disc>/switch/<ident>/config` | switch | `command_topic` = `<base>/<slug>/set`，`payload_on` = `{"switch":"ON"}` |
| `temperature` | `<disc>/sensor/<ident>_temperature/config` | sensor | °C，`device_class: temperature`，`state_class: measurement` |
| `humidity` | `<disc>/sensor/<ident>_humidity/config` | sensor | %，`device_class: humidity` |
| `contact` | `<disc>/binary_sensor/<ident>_contact/config` | binary_sensor | `device_class: door` |
| `power` | `<disc>/sensor/<ident>_power/config` | sensor | W，`state_class: measurement` |
| `energy` | `<disc>/sensor/<ident>_energy/config` | sensor | Wh，`state_class: total_increasing` |
| `button` | `<disc>/sensor/<ident>_button/config` | sensor | 普通字符串 sensor，**不是** HA 的 `event` 实体 |

**无条件发布的三个诊断实体**（不看 caps）：

| 主题 | state_topic | value_template |
|---|---|---|
| `<disc>/sensor/<ident>_rssi/config` | `<base>/bridge/devices` | Jinja 循环找自己的 MAC |
| `<disc>/sensor/<ident>_hop/config` | `<base>/<slug>/state` | `value_json.hop \| default(0)` |
| `<disc>/sensor/<ident>_role/config` | `<base>/<slug>/state` | `value_json.node_role \| default('unknown')` |

三个都带 `"entity_category": "diagnostic"`，在 HA 里会被折叠到诊断区。
`_rssi` 这个**没有** availability 字段（代码里没 `**avail`），所以它永远显示为可用。

### 什么时候重发

```python
sig = ",".join(sorted(caps)) + "|" + dev.model
if dev.discovered and dev.discovery_sig == sig:
    return
```

只有 **caps 集合或 model 变了**才重发。`sorted()` 保证顺序变化不误触发。
另外 Bridge 每次启动 `discovered` 都是 `False`，所以**重启必然重发一轮**。

### 清理 discovery

关掉 `--ha-discovery` **不会**删掉已经发过的 retained config 主题——
HA 里的实体会一直留着。得手工清：

```bash
# 看看有哪些
mosquitto_sub -t 'homeassistant/+/espnow2mqtt_+/config' -v

# 逐个清空（空 payload + retain = 删除）
mosquitto_pub -t homeassistant/switch/espnow2mqtt_living_room/config -r -n
mosquitto_pub -t homeassistant/sensor/espnow2mqtt_living_room_rssi/config -r -n
# ...
```

> **⚠️ 别同时开 `--ha-discovery` 和 HA 集成。**
> 会出现两套实体（`sensor.living_room_temperature` 和
> `sensor.living_room_temperature_2`），自动化会引用错的那个。
> 选一个：简单设备用 discovery，完整支持用集成。

---

## 13. retain 语义总结

| 主题 | retain | 为什么 |
|---|:-:|---|
| `bridge/state` | ✓ | HA 重启后要立刻知道 hub 在不在 |
| `bridge/info` | ✓ | 协调器信息变化极少，订上来就该有 |
| `bridge/devices` | ✓ | 设备列表是状态，不是事件 |
| `<slug>/availability` | ✓ | 同 `bridge/state` |
| `<slug>/state` | ✓ | **最关键**：HA 重启后不用等 30 s 才有值 |
| `<slug>/<key>` | ✗ | 方便主题，不保证新鲜度；值是 `str()` 不适合当权威 |
| `<disc>/…/config` | ✓ | MQTT Discovery 的标准要求 |
| `bridge/request/permit_join` | **不该 retain** | retained 的命令会在每次重连时重放 |
| `<slug>/set` | **不该 retain** | 同上，会导致重启自动开灯 |

**记一条规则：状态 retain，命令不 retain。**

---

## 14. 常用命令行

### 看全局

```bash
# 所有主题
mosquitto_sub -t 'espnow2mqtt/#' -v

# 只看 bridge
mosquitto_sub -t 'espnow2mqtt/bridge/#' -v

# 只看设备状态（不看扁平主题）
mosquitto_sub -t 'espnow2mqtt/+/state' -v
```

### 一次性取值

```bash
# hub 在不在
mosquitto_sub -t espnow2mqtt/bridge/state -C 1

# 协调器信息（含 channel）
mosquitto_sub -t espnow2mqtt/bridge/info -C 1 | python3 -m json.tool

# 设备列表，格式化
mosquitto_sub -t espnow2mqtt/bridge/devices -C 1 | python3 -m json.tool

# 某个设备的状态
mosquitto_sub -t espnow2mqtt/living_room/state -C 1 | python3 -m json.tool
```

`-C 1` 是"收到 1 条就退出"，配合 retained 消息就是"读一次当前值"。

### 只列出在线设备

```bash
mosquitto_sub -t espnow2mqtt/bridge/devices -C 1 \
  | python3 -c 'import json,sys; [print(d["mac"], d["name"], d["rssi"], "hop="+str(d["hop"])) for d in json.load(sys.stdin) if d["online"]]'
```

### 控制

```bash
# 开网 60 秒
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60

# 开关
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"switch":"ON"}'
mosquitto_pub -t espnow2mqtt/living_room/set -m OFF

# 调光
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"switch":"ON","brightness":180,"color_temp":320}'

# 让设备闪灯确认身份
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"identify":10}'
```

### 带认证

```bash
mosquitto_sub -h homeassistant.local -u mqttuser -P mqttpass -t 'espnow2mqtt/#' -v
```

### 清 retained 垃圾

```bash
# 一个改过名的旧设备留下的孤儿主题
for t in state availability switch brightness; do
  mosquitto_pub -t "espnow2mqtt/old_name/$t" -r -n
done
```

---

## 相关文档

- [bridge.md](bridge.md) — 这些主题是怎么产生的
- [usb-protocol.md](usb-protocol.md) — 再往上一层，USB 上的 JSON 行
- [architecture.md](architecture.md) — 完整的上下行路径
- [troubleshooting.md](troubleshooting.md) — 按症状排查
- device 仓库 `docs/reporting.md` — 每个字段在设备侧是怎么算出来的
- device 仓库 `docs/data-model.md` — 完整的 cluster / command / 参数表
