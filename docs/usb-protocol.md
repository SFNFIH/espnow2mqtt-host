# USB NDJSON 协议参考（S3 ↔ 主机）

S3 协调器和主机之间只有一条物理链路：**USB Serial/JTAG**（ESP32-S3 的原生 USB，
ESP-IDF 里的 `usb_serial_jtag` 驱动）。链路上跑的是 **NDJSON**：

> 一行一个 JSON 对象，`\n` 结尾，每个对象都有一个 `"type"` 字段。

这份文档是**逐行类型的完整参考**。想知道每一行是怎么产生的、怎么被消费的，看
[coordinator.md](coordinator.md)（S3 侧）和 [bridge.md](bridge.md)（主机侧）。
空口那一段的协议在 [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md)。

目录：

1. [链路层规则](#1-链路层规则)
2. [方向与总览](#2-方向与总览)
3. [S3 → 主机：`hello`](#3-s3--主机hello)
4. [S3 → 主机：`device`](#4-s3--主机device)
5. [S3 → 主机：`state`](#5-s3--主机state)
6. [S3 → 主机：`ack`](#6-s3--主机ack)
7. [S3 → 主机：`log`](#7-s3--主机log)
8. [S3 → 主机：`pong`](#8-s3--主机pong)
9. [主机 → S3：`ping`](#9-主机--s3ping)
10. [主机 → S3：`list`](#10-主机--s3list)
11. [主机 → S3：`pair`](#11-主机--s3pair)
12. [主机 → S3：`unpair`](#12-主机--s3unpair)
13. [主机 → S3：`cmd`](#13-主机--s3cmd)
14. [错误行为总表](#14-错误行为总表)
15. [手工调试](#15-手工调试)

---

## 1. 链路层规则

| 规则 | 值 | 在哪实现 |
|---|---|---|
| 编码 | UTF-8（实际上只用 ASCII） | — |
| 行分隔 | `\n`。S3 读取时会 trim 掉 `\r`，也会丢弃流中所有 `\r` | `usb_host_link.c:28-43` |
| S3 收行缓冲 | **1024 字节**。超长的行会被**整行丢弃**（`n = 0`），不会截断后交给解析器 | `usb_host_link.c:19,41-42` |
| S3 写超时 | 正文 100 ms，换行符 20 ms。超时就**丢掉**，不重试 | `usb_host_link.c:71-73` |
| 主机收行缓冲 | 无上限（Python `str` 累积），`read(256)` 分块 | `__main__.py:176-185` |
| 主机写 | 整行加锁，`json.dumps(separators=(",",":"))` 紧凑输出 | `__main__.py:164-170` |
| 波特率 | **无意义**。USB Serial/JTAG 是 USB CDC，不是真 UART。`--baud` 只为兼容外置 USB-UART 芯片的板子 | — |
| 非 JSON 行 | 双方都静默忽略（主机走 `LOG.debug`，S3 回一条 `{"type":"log","msg":"bad json"}`） | — |
| 未知 `type` | 主机 `LOG.debug("ignored")`；S3 回 `{"type":"log","msg":"unknown cmd"}` | — |

> **两个关键含义**
>
> 1. **1024 字节的行上限不是瓶颈。** 上行最长的行是带满 160 字节 payload 的
>    `state`，算上包头也就 250 字节左右。下行 `cmd` 的 payload 会被 S3 截到
>    `EN2M_DATA_MAX = 160`（`strnlen(ps, EN2M_DATA_MAX)`），所以你发个 2 KB 的
>    payload 过去，**它会在 S3 侧被无声截断成前 160 字节**——很可能就变成非法 JSON，
>    设备侧解析失败、命令无效。别发超长 payload。
> 2. **写超时会丢行。** 如果主机侧不读（Bridge 挂了、或者你用 `idf.py monitor`
>    占着串口但不消费），S3 的 USB 发送缓冲会满，`usb_serial_jtag_write_bytes`
>    在 100 ms 后超时返回，那一行就**永久丢失**。没有重传、没有排队。
>    所以"Bridge 重启期间设备的状态上报会丢"是设计内的——靠设备的周期性上报补齐。

**日志不走这条链路。** 协调器的 `sdkconfig.defaults` 里：

```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n
```

`ESP_LOGx` 走 UART0（板子上的 TX/RX 引脚），**不会污染 USB 上的 NDJSON 流**。
要看 S3 的 ESP-IDF 日志，得接 UART0。这是和 C3 设备固件相反的配置
（设备那边把 console 放在 USB 上，因为设备的 USB 只用来调试）。

---

## 2. 方向与总览

```
                         USB Serial/JTAG
  ┌──────────────┐   ◄── hello / device / state / ack / log / pong ──   ┌──────────┐
  │  S3 协调器   │                                                       │  Bridge  │
  └──────────────┘   ── ping / list / pair / unpair / cmd ──►            └──────────┘
```

**S3 → 主机（6 种）**

| `type` | 触发 | 频率 | Bridge 怎么处理 |
|---|---|---|---|
| [`hello`](#3-s3--主机hello) | 启动时 + 每 30 s | 30 s | 发 `bridge/state=online` + `bridge/info` |
| [`device`](#4-s3--主机device) | 设备 HELLO/HEARTBEAT、离线扫描、`list`、`unpair` | 每设备 30 s | 更新设备表、`availability`、写 `devices.json` |
| [`state`](#5-s3--主机state) | 设备 STATE 或 HELLO 帧带 payload | 取决于设备上报间隔 | 合并 → `<slug>/state` |
| [`ack`](#6-s3--主机ack) | 设备 ACK 帧，或组件的 ACK 超时事件，或 `en2m_send_downlink` 立即失败 | 每条命令 1 次 | 只记日志 |
| [`log`](#7-s3--主机log) | mesh 日志、配网状态、各种错误 | 稀疏 | `LOG.info("coord: …")` |
| [`pong`](#8-s3--主机pong) | 收到 `ping` | 按需 | `LOG.debug` |

**主机 → S3（5 种）**

| `type` | 谁发 | 何时 |
|---|---|---|
| [`ping`](#9-主机--s3ping) | Bridge `start()` | 启动时一次 |
| [`list`](#10-主机--s3list) | Bridge `start()` | 启动时一次 |
| [`pair`](#11-主机--s3pair) | `bridge/request/permit_join` | 用户开网 |
| [`unpair`](#12-主机--s3unpair) | **Bridge 没有暴露入口**，只能手工发 | — |
| [`cmd`](#13-主机--s3cmd) | `<slug>/set` | 每次 HA 控制 |

---

## 3. S3 → 主机：`hello`

协调器自报身份。**这是主机判断"S3 在线"的唯一依据。**

```json
{"type":"hello","version":2,"role":"coordinator","mac":"7C:DF:A1:00:11:22","fw":"0.4.0-idf","channel":1,"mesh":true,"stack":"esp-idf"}
```

| 字段 | 类型 | 来源 | 说明 |
|---|---|---|---|
| `type` | string | — | `"hello"` |
| `version` | number | `EN2M_VERSION` | **空口协议版本**，当前 `2`。不是固件版本 |
| `role` | string | 常量 | 恒为 `"coordinator"` |
| `mac` | string | `en2m_get_self_mac()` | S3 自己的 STA MAC，大写带冒号 |
| `fw` | string | `EN2M_FW_VERSION` | 当前 `"0.4.0-idf"` |
| `channel` | number | `EN2M_WIFI_CHANNEL` | ESP-NOW 固定信道，默认 `1`。**设备侧必须配成同一个值** |
| `mesh` | bool | 常量 | 恒为 `true`，保留字段（早期有非 mesh 的单跳模式） |
| `stack` | string | 常量 | 恒为 `"esp-idf"`，区别于早期的 Arduino 实现 |

**发送时机**（`main.c`）：

1. `app_main()` 里 `en2m_mesh_init()` 之后立刻发一次
2. `housekeeping` 定时器里每 `HELLO_PERIOD_MS = 30000` ms 发一次

Bridge 收到时：

```python
self.mqtt.publish(f"{self.base}/bridge/state", "online", retain=True)
self.mqtt.publish(f"{self.base}/bridge/info", json.dumps(msg), retain=True)
self._publish_bridge_discovery()
```

整条 `hello` 原样转发到 `bridge/info`，所以你可以在 MQTT 上直接看到 S3 的
版本和信道：

```bash
mosquitto_sub -t espnow2mqtt/bridge/info -C 1
```

> **`channel` 不匹配是最常见的"设备完全不上线"原因。**
> S3 的 `channel` 在 `bridge/info` 里，设备的信道得看它的固件 Kconfig。
> 两边不一致的话，设备收不到 beacon，永远选不上父节点。见
> [troubleshooting.md](troubleshooting.md)。

---

## 4. S3 → 主机：`device`

设备的**拓扑与生命周期**事件。**不含设备的业务状态**（那是 `state`）。

```json
{"type":"device","event":"online","mac":"AA:BB:CC:DD:EE:FF","model":"c3-light","name":"living_room","rssi":-58,"hop":1,"via":"7C:DF:A1:00:11:22","node_role":"leaf"}
{"type":"device","event":"info","mac":"AA:BB:CC:DD:EE:FF","model":"c3-light","name":"living_room","rssi":-61,"hop":1,"via":"7C:DF:A1:00:11:22","node_role":"leaf"}
{"type":"device","event":"offline","mac":"AA:BB:CC:DD:EE:FF"}
```

| 字段 | 类型 | offline 时有？ | 说明 |
|---|---|---|---|
| `event` | string | ✓ | `"online"` / `"info"` / `"offline"` |
| `mac` | string | ✓ | 设备的 **origin MAC**（不是中继的 MAC） |
| `model` | string | ✗ | 设备 `en2m_config_t.model`，**最多 11 字符**（`char model[12]`）。空字符串时该字段**整个不出现** |
| `name` | string | ✗ | 设备 `en2m_config_t.name`，**最多 15 字符**（`char name[16]`）。空时不出现 |
| `rssi` | number | ✗ | S3 收到这一帧时的 RSSI，dBm，负数 |
| `hop` | number | ✗ | 包头里的跳数。直连叶子是 `1` |
| `via` | string | ✗ | **上一跳**的 MAC。直连时等于 S3 自己的 MAC |
| `node_role` | string | ✗ | `"leaf"` / `"router"` / `"unknown"` |

`offline` 事件**只带 `type`/`event`/`mac`**（`emit_device()` 里
`if (strcmp(event, "offline") != 0)` 把其余全部跳过）。因此 Bridge 里
一个已离线设备的 `rssi`/`hop`/`via` 是**它掉线前的最后已知值**，不会被清零。

### 三种 `event` 的触发条件

| `event` | S3 侧触发 | 代码位置 |
|---|---|---|
| `online` | 收到设备的 `HELLO` 或 `HEARTBEAT` 帧，**且这是 peer 表里首次见到它**（`p->last_ms == 0`） | `on_uplink`，`emit_device(p, first ? "online" : "info")` |
| `info` | 同上，但 peer 表里已经有它了 | 同上 |
| `info` | 主机发来 `list`，S3 遍历 peer 表全部重发一遍 | `handle_host_line` 的 `list` 分支 |
| `offline` | `housekeeping` 每 `PEER_SWEEP_PERIOD_MS = 2000` ms 扫一遍，`now - last_ms > EN2M_OFFLINE_MS`（默认 **90000** ms） | `housekeeping` |
| `offline` | 主机发来 `unpair`，命中 peer 表 | `handle_host_line` 的 `unpair` 分支 |

> **离线判定的时间窗**
>
> 设备心跳 `EN2M_HEARTBEAT_MS = 30000` ms，离线阈值 `EN2M_OFFLINE_MS = 90000` ms，
> 扫描周期 2 s。所以设备真正断电后，`offline` 事件会在 **90～92 s** 后发出。
> 允许连丢 2 次心跳，这是有意留的余量（ESP-NOW 单播偶发丢包很正常）。
>
> 注意 `last_ms` 被**任何**上行帧刷新（`on_uplink` 开头就更新），不只是心跳。
> 所以一个每 30 s 上报状态的设备，即使心跳全丢了也不会被判离线。

`info` 出现得非常频繁（每个设备每 30 s 一条）。Bridge 对它的处理刻意很轻：
更新字段、必要时补发 discovery、写 `devices.json`，**不动 `online` 标志**。

### peer 表容量

S3 侧的 `s_peers[]` 长度是 `EN2M_MAX_ROUTES`（默认 **32**）。满了之后
`alloc_peer()` 返回 `-1`，`on_uplink` 发一条 `{"type":"log","msg":"peer table full"}`
**然后直接丢弃这一帧**（状态也不会转发）。要支持更多设备，改设备端和协调器端的
`CONFIG_EN2M_MAX_ROUTES` 并重新编译。

---

## 5. S3 → 主机：`state`

设备的业务状态。**S3 对 `payload` 的内容完全不理解**，只是把设备 JSON 原样嵌进去。

```json
{"type":"state","mac":"AA:BB:CC:DD:EE:FF","ts":123456,"hop":1,"via":"7C:DF:A1:00:11:22","payload":{"node_role":"leaf","switch":"ON","brightness":200,"color_temp":370,"caps":["light"]}}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `mac` | string | 设备 origin MAC |
| `ts` | number | **S3 的开机毫秒数**（`esp_timer_get_time()/1000`）。不是墙上时间，S3 复位后归零。Bridge **不用**这个字段 |
| `hop` | number | 跳数，S3 加的 |
| `via` | string | 上一跳 MAC，S3 加的 |
| `payload` | object | 设备发的 JSON，**原样解析后嵌入**。`data_len == 0` 时该字段**整个不出现** |

`payload` 里可能有什么，取决于设备的属性集——完整的字段表在
[mqtt.md](mqtt.md#8-设备状态字段) 和 device 仓库的 `docs/reporting.md`。

### 触发条件

`on_uplink` 里：

```c
if (pkt->msg_type == EN2M_MSG_STATE || pkt->msg_type == EN2M_MSG_HELLO ||
    pkt->msg_type == EN2M_MSG_ACK) {
```

所以 `state` 行由三种帧产生：

| 帧类型 | 产生的行 | 说明 |
|---|---|---|
| `EN2M_MSG_STATE`（3） | `type:"state"` | 正常的周期/变化上报 |
| `EN2M_MSG_HELLO`（2） | `type:"state"` **和** `type:"device"` | 设备入网的第一帧同时带身份和状态，S3 会发两行 |
| `EN2M_MSG_ACK`（5） | `type:"ack"` | 见 [§6](#6-s3--主机ack) |

`EN2M_MSG_HEARTBEAT`（6）**只**产生 `device` 行，不产生 `state`——心跳帧不带 payload。

### `{"raw": ...}` 兜底

设备的 payload 不是合法 JSON 时，S3 不丢弃，而是包一层：

```c
cJSON *payload = cJSON_Parse(tmp);
if (payload) {
    cJSON_AddItemToObject(o, "payload", payload);
} else {
    cJSON *wrap = cJSON_CreateObject();
    cJSON_AddStringToObject(wrap, "raw", tmp);
    cJSON_AddItemToObject(o, "payload", wrap);
}
```

结果：

```json
{"type":"state","mac":"AA:BB:CC:DD:EE:FF","ts":123456,"hop":1,"via":"7C:DF:A1:00:11:22","payload":{"raw":"{\"switch\":\"ON\",\"brig"}}
```

**看到 `raw` 就说明设备侧的 JSON 构造有 bug**——最典型的是设备自己拼字符串
超过了 160 字节被截断（用 `en2m` 组件的内置上报不会有这个问题，
它有降级机制；手写 `en2m_send_state()` 才会）。

Bridge 会把它当普通 payload 合并进去，于是 MQTT 上出现一个 `raw` 字段。
这是一个**故意可见的故障信号**：比静默丢包好排查得多。

---

## 6. S3 → 主机：`ack`

命令确认。有**三个来源**，字段略有不同。

### 6.1 设备确认（成功）

设备收到带非零 `id` 的 CMD 帧后回一个 ACK 帧，S3 转成：

```json
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":true,"ts":123456,"hop":1,"via":"7C:DF:A1:00:11:22","payload":{...}}
```

注意它走的是 `state` 那段代码（`on_uplink` 里 `EN2M_MSG_ACK` 和 `STATE` 共用分支），
所以**带 `ts`/`hop`/`via`，而且如果 ACK 帧带了 data 还会带 `payload`**。
当前的设备固件 ACK 不带 data，所以实际上看不到 `payload`。

### 6.2 ACK 超时（失败）

组件重传 4 次仍无响应，抛 `EN2M_EVENT_ACK_TIMEOUT`，`on_en2m_event` 转成：

```json
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":false,"error":"timeout"}
```

这条走的是 `usb_emit_ack()`，**只有 4 个字段 + `error`**，没有 `ts`/`hop`/`via`。

时间线（`EN2M_CMD_RETRIES = 3`，`EN2M_CMD_RETRY_MS = 400`）：

```
t=0     ms   第 1 次发送
t=400   ms   第 2 次
t=800   ms   第 3 次
t=1200  ms   第 4 次
t=1600  ms   放弃 → EN2M_EVENT_ACK_TIMEOUT → {"ok":false,"error":"timeout"}
```

所以从 Bridge 发出 `cmd` 到收到失败 ACK，最坏 **约 1.6 s**。

### 6.3 立即发送失败

`en2m_send_downlink()` 同步返回非 `ESP_OK`（比如没有到该 MAC 的路由、
pending 表满了）：

```json
{"type":"ack","mac":"AA:BB:CC:DD:EE:FF","id":7,"ok":false,"error":"send_fail"}
```

这条是**立刻**返回的，不用等 1.6 s。

| `error` | 含义 | 常见原因 |
|---|---|---|
| `"timeout"` | 发出去了，设备没回 | 设备断电 / 信号太差 / 中继节点掉了 / 设备侧崩了 |
| `"send_fail"` | 根本没发出去 | S3 路由表里没有这个 MAC（设备从没上线过，或 `EN2M_ROUTE_STALE_MS = 120000` ms 后路由过期）；`EN2M_MAX_PENDING = 4` 条未确认命令已占满 |

Bridge 对 `ok:true` 只 `LOG.debug`，对 `ok:false` 发 `LOG.warning`：

```
WARNING espnow2mqtt: command 7 to AA:BB:CC:DD:EE:FF failed: timeout
```

**失败不进 MQTT。** 见 [bridge.md](bridge.md#5-串口读取_serial_loop-与-_handle_serial)。

---

## 7. S3 → 主机：`log`

S3 的带内日志通道。**因为 ESP-IDF 的 `ESP_LOGx` 走 UART0 而不是 USB，
这是主机唯一能看到的 S3 内部信息。**

```json
{"type":"log","msg":"coordinator ready (esp-idf mesh)"}
{"type":"log","msg":"pairing_enabled","seconds":60}
```

| 字段 | 说明 |
|---|---|
| `msg` | 日志文本 |
| `seconds` | **只有 `pairing_enabled` 这一条**额外带这个字段 |

### 完整的 `msg` 列表

S3 固件里 `usb_log()` / `type:"log"` 的所有调用点：

| `msg` | 来自 | 含义 |
|---|---|---|
| `"coordinator ready (esp-idf mesh)"` | `app_main()` | 初始化完成。**看到这条说明 S3 起来了** |
| `"mesh init"` | `en2m` 组件经 `on_log` 回调 | 组件的 mesh 层初始化完成 |
| `"pairing_enabled"`（带 `seconds`） | `pair` 命令 | 配网窗口打开 |
| `"pairing_disabled"` | `housekeeping` | 配网窗口到时自动关闭 |
| `"peer table full"` | `on_uplink` → `alloc_peer()` 失败 | 已有 32 个设备，**这一帧被丢弃** |
| `"bad json"` | `handle_host_line` | 主机发来的行不是合法 JSON |
| `"missing type"` | `handle_host_line` | JSON 合法但没有字符串型 `type` |
| `"unknown cmd"` | `handle_host_line` | `type` 不是 5 种已知命令之一 |
| `"bad mac"` | `unpair` / `cmd` 分支 | `mac` 字段缺失，或 `en2m_mac_from_str()` 解析失败 |
| `"rx_dropped=<n>"` | `on_en2m_event` 收到 `EN2M_EVENT_RX_DROPPED` | **组件的接收队列满了，累计丢了 n 帧** |

> **`rx_dropped` 是个重要信号。**
> 它表示 ESP-NOW 收包速率超过了 `en2m` 任务的处理速度
> （队列深度 `EN2M_QUEUE_LEN = 8`）。`n` 是**累计值**，所以它只会涨。
> 偶尔涨 1～2 无所谓；持续快速增长说明设备太多或上报太频繁，
> 该调大 `CONFIG_EN2M_QUEUE_LEN` 或者调长设备的上报间隔。

Bridge 侧：

```python
elif mtype == "log":
    LOG.info("coord: %s", msg.get("msg") or msg)
```

`log` 行**不进 MQTT**，只出现在 Bridge 的 stdout。所以：

```
INFO espnow2mqtt: coord: coordinator ready (esp-idf mesh)
INFO espnow2mqtt: coord: pairing_enabled
```

注意 `msg.get("msg") or msg`——如果某条 `log` 行没有 `msg` 字段，
会把整个 dict 打出来。`pairing_enabled` 的 `seconds` 字段因此**不会**出现在
Bridge 日志里（因为 `msg` 字段存在，`or` 短路了）。

---

## 8. S3 → 主机：`pong`

```json
{"type":"pong","ms":123456}
```

| 字段 | 说明 |
|---|---|
| `ms` | S3 的开机毫秒数 |

对 `ping` 的响应，就这一个用途。Bridge 只 `LOG.debug("pong %s", msg)`，
**所以不开 `-v` 你看不到它**。

`ms` 的实际用途是判断 S3 有没有偷偷复位过：如果两次 `pong` 的 `ms` 不是递增的，
S3 中间重启了。Bridge 没有实现这个检查（`hello` 每 30 s 一次已经够用了），
但手工调试时很有用。

---

## 9. 主机 → S3：`ping`

```json
{"type":"ping"}
```

无参数。S3 立刻回 `pong`。

Bridge 在 `start()` 里发一次（串口打开后 500 ms），之后**再也不发**——
没有周期性心跳。判断 S3 是否存活靠 S3 自己每 30 s 的 `hello`。

---

## 10. 主机 → S3：`list`

```json
{"type":"list"}
```

无参数。S3 遍历整个 `s_peers[]`，对每个 `used` 的槽位发一条
`{"type":"device","event":"info",...}`。

```c
} else if (strcmp(type->valuestring, "list") == 0) {
    for (int i = 0; i < EN2M_MAX_ROUTES; i++) {
        if (s_peers[i].used) {
            emit_device(&s_peers[i], "info");
        }
    }
}
```

**没有响应边界**——不会有 `{"type":"list_end"}` 之类的标记。设备表为空时
`list` 什么都不返回，主机分不清"没设备"和"S3 没收到 list"。

> **`list` 不重放设备状态。**
> 它只重放拓扑信息（MAC/name/model/rssi/hop/via/role）。S3 **不缓存设备的
> payload**，所以 Bridge 重启后拿不回业务状态，只能等设备下一次上报。
> 详见 [bridge.md](bridge.md#10-devicesjson持久化了什么没持久化什么) 的警告。

Bridge 只在 `start()` 里发一次。想随时重新同步，手工发（见 [§15](#15-手工调试)）。

---

## 11. 主机 → S3：`pair`

打开配网窗口。**这是新设备唯一的入网途径。**

```json
{"type":"pair","seconds":60}
```

| 字段 | 类型 | 必需 | 默认 | 约束 |
|---|---|---|---|---|
| `seconds` | number | 否 | `60` | 夹到 `[1, 300]` |

```c
int seconds = 60;
const cJSON *sec = cJSON_GetObjectItem(doc, "seconds");
if (cJSON_IsNumber(sec)) {
    seconds = sec->valueint;
}
if (seconds < 1)   { seconds = 1; }
if (seconds > 300) { seconds = 300; }
en2m_set_pairing(true);
s_pair_until_ms = millis() + seconds * 1000LL;
```

响应：

```json
{"type":"log","msg":"pairing_enabled","seconds":60}
```

窗口到时后 `housekeeping`（1 s 周期）自动关闭并发：

```json
{"type":"log","msg":"pairing_disabled"}
```

**重复发 `pair` 会重置窗口**（覆盖 `s_pair_until_ms`），不会叠加。
发 `{"type":"pair","seconds":1}` 可以用来提前关闭配网（1 s 后自动关）。
没有显式的"立刻关闭"命令。

MQTT 入口是 `<base>/bridge/request/permit_join`，见
[mqtt.md](mqtt.md#5-bridgerequestpermit_join)。

---

## 12. 主机 → S3：`unpair`

```json
{"type":"unpair","mac":"AA:BB:CC:DD:EE:FF"}
```

| 字段 | 类型 | 必需 |
|---|---|---|
| `mac` | string | **是**。缺失或格式错误 → `{"type":"log","msg":"bad mac"}` |

S3 做两件事：

```c
int idx = find_peer(mac);
if (idx >= 0) {
    emit_device(&s_peers[idx], "offline");
    s_peers[idx].used = false;
}
en2m_forget_route(mac);
```

1. 从 peer 表里删掉（先发一条 `offline` 让主机同步）
2. 调 `en2m_forget_route()` 删掉 mesh 路由

**注意 `en2m_forget_route(mac)` 是无条件调用的**，即使 peer 表里没这个 MAC。
所以对一个未知 MAC 发 `unpair` 不会报错，只是什么都没发生（`find_peer` 返回 -1，
`forget_route` 也找不到）。

> **`unpair` 不会让设备忘记协调器。**
> 它只清理 S3 侧的表。设备那边还认着它的父节点，会继续发心跳，
> 于是几秒后它又会作为 `online` 事件重新出现——**除非配网窗口关着**。
>
> 配网关着的时候，S3 的 `en2m` 组件会拒绝未知设备的入网请求。
> 所以正确的"踢掉设备"流程是：
>
> 1. 确认配网窗口已关（没发过 `pair`，或者等它超时）
> 2. 发 `unpair`
> 3. 设备侧断电或重新烧固件
>
> 如果只做第 2 步而设备还在跑，它能不能回来取决于组件的入网策略——
> 见 device 仓库 `docs/mesh.md` 的"配网"一节。

**Bridge 没有暴露 `unpair` 的 MQTT 入口。** `bridge/request/+` 订阅了，
但 `_on_mqtt_message` 里只匹配 `permit_join`，其他的静默丢弃。要用 `unpair`
只能手工往串口写（[§15](#15-手工调试)），而且得先停掉 Bridge（串口独占）。

---

## 13. 主机 → S3：`cmd`

下行命令。**这是唯一会产生空口发送的主机命令。**

```json
{"type":"cmd","mac":"AA:BB:CC:DD:EE:FF","id":7,"payload":{"switch":"ON","brightness":128}}
```

| 字段 | 类型 | 必需 | 说明 |
|---|---|---|---|
| `mac` | string | **是** | 目标设备。解析失败 → `{"type":"log","msg":"bad mac"}` |
| `id` | number | 否 | `uint16_t`。**非零 = 启用 ACK/重传**；`0` = fire-and-forget。缺失时 S3 用 `en2m_next_cmd_id()` 自己生成（所以缺省是**带**重传的） |
| `payload` | object | 否 | 原样序列化后发给设备。缺失时发 `"{}"` |

S3 侧：

```c
uint16_t id = cJSON_IsNumber(idj) ? (uint16_t)idj->valueint : en2m_next_cmd_id();
char *ps = payload ? cJSON_PrintUnformatted(payload) : NULL;
esp_err_t err = en2m_send_downlink(
    mac, id, (const uint8_t *)(ps ? ps : "{}"),
    (uint8_t)strnlen(ps ? ps : "{}", EN2M_DATA_MAX));
```

三个要注意的点：

1. **`payload` 被重新序列化。** S3 用 `cJSON_PrintUnformatted()` 重新打印，
   所以空格、key 顺序、数字格式可能和你发的不完全一样。语义等价，但别指望字节级一致。
2. **超过 160 字节会被无声截断。** `strnlen(ps, EN2M_DATA_MAX)` 只是限制长度，
   **不做 JSON 合法性检查**。截断后设备侧 `cJSON_Parse` 失败，命令被丢弃，
   而且**不会有 ACK**，所以你会在 1.6 s 后收到 `{"ok":false,"error":"timeout"}`。
   实际的 HA 命令都远小于 160 字节，除非你手工发很大的 payload。
3. **`id` 会被截成 `uint16_t`。** `{"id":70000}` 变成 `4464`。

### `payload` 的两种写法

S3 **完全不看 payload 内容**，两种风格都只是透传。解析在设备侧的数据模型层。

**扁平风格（HA 友好，推荐）**

```json
{"switch":"ON"}
{"switch":"ON","brightness":128,"color_temp":370}
{"cover":"CLOSE","position":100}
{"lock":"LOCK"}
{"fan_mode":"high","percentage":80}
{"hvac_mode":"cool","target_temperature":24}
{"identify":10}
```

**Cluster 风格（可以显式指定端点）**

```json
{"ep":1,"cluster":"on_off","command":"toggle"}
{"ep":1,"cluster":"level_control","command":"move_to_level","level":200}
{"ep":1,"cluster":"window_covering","command":"open"}
{"ep":1,"cluster":"door_lock","command":"unlock"}
{"ep":1,"cluster":"fan_control","command":"set_percent","percentage":50}
{"ep":1,"cluster":"thermostat","command":"set_mode","mode":"heat"}
{"ep":1,"cluster":"identify","command":"identify","seconds":10}
```

省略 `ep` 时，设备会选**暴露该 cluster 的最小端点号**。
完整的 cluster / command / 参数表在 device 仓库的 `docs/data-model.md`。

### 命令的完整生命周期

```
Bridge: {"type":"cmd","mac":"AA:..","id":7,"payload":{"switch":"ON"}}
  │
  ├─ 解析失败/bad mac ──► {"type":"log","msg":"bad mac"}                    [终止]
  │
  ├─ en2m_send_downlink != ESP_OK ──► {"ok":false,"error":"send_fail"}      [终止]
  │
  └─ 排进组件的 pending 表，空口发送
       │
       ├─ t=0/400/800/1200 ms 重传（id != 0 时）
       │
       ├─ 设备 ACK ──► {"type":"ack","id":7,"ok":true,...}
       │                └─ 设备随后发一条新 STATE ──► {"type":"state",...}  [成功]
       │
       └─ t=1600 ms 无 ACK ──► {"ok":false,"error":"timeout"}               [失败]
```

**成功的命令会产生两行**：一行 `ack`，一行 `state`。Bridge 把 `ack` 记进日志、
把 `state` 发到 MQTT，所以 HA 看到的是状态变化而不是"命令成功"。

---

## 14. 错误行为总表

### S3 对畸形输入的反应

| 主机发的 | S3 的反应 |
|---|---|
| 非 JSON | `{"type":"log","msg":"bad json"}` |
| JSON 但顶层不是对象（`[1,2]`、`"x"`、`5`） | `{"type":"log","msg":"missing type"}`（`cJSON_GetObjectItem` 在非对象上返回 NULL） |
| 缺 `type`，或 `type` 不是字符串 | `{"type":"log","msg":"missing type"}` |
| `type` 是未知字符串 | `{"type":"log","msg":"unknown cmd"}` |
| `cmd`/`unpair` 缺 `mac` 或 MAC 格式错 | `{"type":"log","msg":"bad mac"}` |
| 行超过 1023 字节 | **整行丢弃，无任何响应**（`usb_host_link.c` 里 `n = 0`） |
| `cmd` 的 `payload` 超 160 字节 | 无声截断 → 设备解析失败 → 1.6 s 后 `{"ok":false,"error":"timeout"}` |
| `pair` 的 `seconds` 超范围 | 静默夹到 `[1,300]`，响应里的 `seconds` 是**夹过之后**的值 |
| `pair` 的 `seconds` 不是数字（`"60"`） | 静默用默认 `60`（`cJSON_IsNumber` 为假） |

### Bridge 对畸形输入的反应

| S3 发的 | Bridge 的反应 |
|---|---|
| 非 JSON（比如 S3 复位时的 ROM 启动信息） | `LOG.debug("non-json: %s")`，**只在 `-v` 下可见** |
| 未知 `type` | `LOG.debug("ignored: %s")` |
| `device` 行没有 `mac` | 直接 `return`，什么都不做 |
| `state` 行没有 `mac` | 直接 `return` |
| `state` 的 `payload` 不是对象 | 包成 `{"value": <原值>}` 继续处理 |
| `hop`/`rssi` 不是整数 | `try/except (TypeError, ValueError): pass`，保留旧值 |
| `payload` 里有 `raw` 字段 | 当普通字段合并，会出现在 MQTT 的 `<slug>/state` 里 |

**两边都遵循"未知的就忽略"**，所以协议可以单向升级：
新版 S3 加一种新行类型，老 Bridge 不会崩；新 Bridge 加一个新命令，
老 S3 只回一条 `unknown cmd`。

---

## 15. 手工调试

**串口是独占的**，手工调试前必须先停掉 Bridge。

### 看 S3 在说什么

```bash
# 最简单：直接 cat（只读）
cat /dev/ttyACM0

# 想同时看和发，用 socat 或 miniterm
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

或者不停 Bridge，直接开 `-v` 看它转述：

```bash
python -m espnow2mqtt --port /dev/ttyACM0 --mqtt-host localhost -v
```

`-v` 会显示 `pong`、成功的 `ack`、以及所有非 JSON 行——**这些在默认的 INFO 级别下全都看不到**。

### 手工发命令

```bash
# 探活
printf '{"type":"ping"}\n' > /dev/ttyACM0

# 要一遍设备表
printf '{"type":"list"}\n' > /dev/ttyACM0

# 开网 120 秒
printf '{"type":"pair","seconds":120}\n' > /dev/ttyACM0

# 开灯（带 ACK）
printf '{"type":"cmd","mac":"AA:BB:CC:DD:EE:FF","id":1,"payload":{"switch":"ON"}}\n' > /dev/ttyACM0

# 开灯（fire-and-forget，不重传）
printf '{"type":"cmd","mac":"AA:BB:CC:DD:EE:FF","id":0,"payload":{"switch":"ON"}}\n' > /dev/ttyACM0

# 踢掉设备
printf '{"type":"unpair","mac":"AA:BB:CC:DD:EE:FF"}\n' > /dev/ttyACM0
```

**用 `printf` 不要用 `echo`**：某些 shell 的 `echo` 会对反斜杠做额外处理。
`printf` 的 `\n` 是必需的——没有换行 S3 永远不会处理这一行。

一边发一边看，开两个终端：

```bash
# 终端 1
cat /dev/ttyACM0
# 终端 2
printf '{"type":"ping"}\n' > /dev/ttyACM0
```

### 看 S3 的 ESP-IDF 日志

NDJSON 流里**没有** `ESP_LOGx` 的输出。要看崩溃回溯、Wi-Fi 驱动日志之类的，
必须接 UART0：

```bash
cd firmware/coordinator
idf.py -p /dev/ttyUSB0 monitor   # 注意：UART0 对应的设备，不是 USB 的 ttyACM0
```

如果你的板子没引出 UART0，临时改 `sdkconfig.defaults` 把 console 切回 USB
（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）重新烧——但这样 NDJSON 流会被日志污染，
Bridge 会把日志行当非 JSON 丢掉（功能上还能跑，只是日志刷屏）。

---

## 相关文档

- [coordinator.md](coordinator.md) — 这些行在 S3 侧是怎么产生的
- [bridge.md](bridge.md) — 这些行在主机侧是怎么被消费的
- [mqtt.md](mqtt.md) — 再往下一层，MQTT 上的主题
- [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md) — 空口协议摘要
- [troubleshooting.md](troubleshooting.md) — 按症状排查
