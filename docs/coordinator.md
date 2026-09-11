# ESP32-S3 协调器固件

> **这篇讲"为什么这么写"。** 想知道"怎么烧起来、怎么手动测 USB 协议"，
> 看 [firmware/coordinator/README.md](../firmware/coordinator/README.md)。

源码：**`firmware/coordinator/`**，391 行 `main.c` + 84 行 `usb_host_link.c`。

它的工作只有一句话：**把 ESP-NOW 帧和 USB 上的 NDJSON 互相翻译，
并且记住谁在线。** 它不解析设备载荷，不认识任何 cluster，也不知道 MQTT 的存在。

---

## 1. 为什么协调器不用数据模型

协调器调的是 **`en2m_mesh_init`**，不是 `en2m_start`：

```c
en2m_config_t cfg = {                      /* en2m_config_t，不是 en2m_device_config_t */
    .role    = EN2M_ROLE_COORDINATOR,
    .model   = "s3-coord",
    .name    = "coordinator",
    .fw      = EN2M_FW_VERSION,
    .channel = EN2M_WIFI_CHANNEL,
    .on_uplink = on_uplink,                /* 上行帧交给我 */
    .on_log    = on_mesh_log,              /* mesh 的日志也交给我 */
};
ESP_ERROR_CHECK(en2m_mesh_init(&cfg));
```

`en2m_start` = "初始化数据模型 + 调 `en2m_mesh_init`"。协调器没有 endpoint、
没有 cluster、没有属性，所以直接用下层就行。好处：

| | 说明 |
|---|---|
| 少 5.6 KB RAM | 数据模型的静态数组整个不参与（默认 4 × 8 × 6 = 5664 字节） |
| 少一整套逻辑 | 不需要上报调度、不需要命令翻译、不需要 NVS 持久化 |
| 载荷透传 | 协调器不解析 `pkt->data`，加新设备类型时它一行都不用改 |

同一个模式也用在 device 仓库的 `firmware/router` 上——那是一个更纯粹的例子
（纯转发，连 peer 表都不要）。

> 协调器的 `sdkconfig.defaults` 里**没有**把三个容量宏压到最小。
> 想省 RAM 可以加
> `CONFIG_EN2M_MAX_ENDPOINTS=1`、`CONFIG_EN2M_MAX_CLUSTERS_PER_ENDPOINT=1`、
> `CONFIG_EN2M_MAX_ATTRIBUTES_PER_CLUSTER=1`，反正一个都不会用到。
> S3 的 RAM 很宽裕，所以默认没动。

---

## 2. 上行：`on_uplink`

这是整个固件最核心的 30 行。它跑在 **`en2m` 任务**上（不是 ESP-NOW 回调里，
组件已经把帧从回调里搬出来了）。

```c
static void on_uplink(const en2m_pkt_t *pkt, int8_t rssi, const uint8_t from_mac[6], void *user)
```

| 参数 | 意思 |
|---|---|
| `pkt` | 完整的空口帧。`pkt->origin` 是**原始发送者**，可能隔了几跳 |
| `rssi` | **最后一跳**的信号强度，不是端到端的 |
| `from_mac` | 最后一跳的 MAC，也就是 `via` |

### 第一步：记账

```c
int idx = alloc_peer(pkt->origin);         /* 找不到就分配一个槽位 */
if (idx < 0) { usb_log("peer table full"); return; }

host_peer_t *p = &s_peers[idx];
bool first = (p->last_ms == 0);            /* 这是它第一次说话吗 */
p->last_ms = millis();
p->rssi = rssi;
p->hop  = pkt->hop;
p->role = pkt->role;
en2m_mac_copy(p->via, from_mac);
if (pkt->model[0]) strncpy(p->model, pkt->model, sizeof(p->model) - 1);
if (pkt->name[0])  strncpy(p->name,  pkt->name,  sizeof(p->name) - 1);
```

注意 `first` 的判断用的是 `last_ms == 0`，而不是 `alloc_peer` 有没有新建槽位——
因为 `unpair` 会把 `used` 置 false 但不清零，所以用时间戳更稳。

### 第二步：HELLO / HEARTBEAT → `device` 事件

```c
if (pkt->msg_type == EN2M_MSG_HELLO || pkt->msg_type == EN2M_MSG_HEARTBEAT) {
    emit_device(p, first ? "online" : "info");
}
```

所以：

| 帧类型 | 主机看到 |
|---|---|
| 第一个 HELLO / HEARTBEAT | `{"type":"device","event":"online",...}` |
| 后续的 HELLO / HEARTBEAT | `{"type":"device","event":"info",...}` |

`online` 让 Bridge 发 `availability: online` 并触发 discovery；
`info` 只是刷新 RSSI / hop / via 这些诊断信息。

### 第三步：STATE / HELLO / ACK → `state` 或 `ack` 行

```c
if (pkt->msg_type == EN2M_MSG_STATE || pkt->msg_type == EN2M_MSG_HELLO ||
    pkt->msg_type == EN2M_MSG_ACK) {
    /* type = ack 还是 state，取决于 msg_type */
    /* 补上 mac / ts / hop / via */
    if (pkt->msg_type == EN2M_MSG_ACK) {
        /* 再补 id 和 ok:true */
    }
    if (pkt->data_len) {
        /* cJSON_Parse 成功 → 整块塞进 payload
           失败 → {"raw": "原始字符串"} */
    }
}
```

**HEARTBEAT 不在这个列表里**，因为它不带载荷——它只用来记账，不产生 `state` 行。

`ts`、`hop`、`via` 三个字段是**协调器加的**，设备并不知道自己隔了几跳。
这也是为什么 `hop` 在 Bridge 那边是从 `device` / `state` 行的顶层取，
而不是从 `payload` 里取。

### 载荷解析失败会怎样

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

**协调器绝不丢帧**。载荷不是合法 JSON 时它包成 `{"raw":"..."}` 照样发上去，
让 Bridge 和人去判断出了什么事。这在调设备侧上报代码时特别有用——
你能看到设备到底发了什么，而不是看到一片安静。

> 这种情况在正常运行中不该出现。出现了通常意味着设备侧的上报超过了
> `EN2M_DATA_MAX`（160 字节）被截断。设备侧有三级降级来避免这个，
> 见 device 仓库的 `docs/reporting.md`。

---

## 3. 下行：`handle_host_line`

跑在 **`usb_rx` 任务**上。支持五种 `type`：

| `type` | 作用 | 回什么 |
|---|---|---|
| `ping` | 探活 | `{"type":"pong","ms":...}` |
| `pair` | 开配网窗口 | `{"type":"log","msg":"pairing_enabled","seconds":N}` |
| `list` | 把整张 peer 表吐出来 | 每个在线设备一条 `device`/`info` |
| `unpair` | 忘掉一个设备 | 一条 `device`/`offline` |
| `cmd` | 下发命令 | 失败时 `{"type":"ack",...,"error":"send_fail"}` |

非法输入的处理一律是**回一条 `log` 行然后继续**，从不重启、从不静默：

```
{"type":"log","msg":"bad json"}        ← 这一行不是 JSON
{"type":"log","msg":"missing type"}    ← 是 JSON 但没有 type 字段
{"type":"log","msg":"bad mac"}         ← MAC 解析不出来
{"type":"log","msg":"unknown cmd"}     ← type 不认识
```

### `pair`

```c
en2m_set_pairing(true);
s_pair_until_ms = millis() + seconds * 1000LL;
```

`seconds` 被夹到 **1..300**。到点由 `housekeeping` 关掉：

```c
if (en2m_get_pairing() && now >= s_pair_until_ms) {
    en2m_set_pairing(false);
    usb_log("pairing_disabled");
}
```

配网窗口的作用是让协调器接受**陌生设备**的上行。不开窗口时组件会丢掉
不认识的上行并打 `drop uplink (not pairing / unknown)`。
机制细节见 device 仓库的 `docs/mesh.md`。

### `cmd`

```c
uint16_t id = cJSON_IsNumber(idj) ? (uint16_t)idj->valueint : en2m_next_cmd_id();
char *ps = payload ? cJSON_PrintUnformatted(payload) : NULL;
esp_err_t err = en2m_send_downlink(mac, id, (const uint8_t *)(ps ? ps : "{}"),
                                   (uint8_t)strnlen(ps ? ps : "{}", EN2M_DATA_MAX));
if (err != ESP_OK) {
    usb_emit_ack(macj->valuestring, id, false, "send_fail");
}
```

三个细节：

1. **主机没给 `id` 时协调器自己分配**（`en2m_next_cmd_id()`，永不返回 0）。
   Bridge 总是会给，所以这是给手工调试留的方便。
2. **`id == 0` 意味着不重传、不等 ACK**。想 fire-and-forget 就显式传 0。
3. **载荷被 `strnlen(..., EN2M_DATA_MAX)` 截断**。160 字节对命令来说很宽裕
   （最长的 cluster 风格命令也就 70 来字节），但如果你塞了个超长的自定义载荷，
   它会被**静默截断**成非法 JSON，设备那边会打
   `command payload is not valid JSON` 并且**不回 ACK**，
   于是重传 4 次后超时。这是一个真实的失败模式，见
   [troubleshooting.md](troubleshooting.md#74-failed-timeout)。

---

## 4. ACK 与重传

重传本身**完全由 `en2m` 组件做**，协调器只负责报告结果。

```
Bridge 发 {"type":"cmd","id":7,...}
   │
   ▼
en2m_send_downlink(mac, 7, payload, len)
   │  id != 0 → 存进重传表（EN2M_MAX_PENDING 个槽位，默认 4）
   ▼
第 1 次发送        t = 0 ms
第 2 次（重传 1）  t = 400 ms
第 3 次（重传 2）  t = 800 ms
第 4 次（重传 3）  t = 1200 ms
   │
   ├─ 期间收到 ACK 帧 → on_uplink 里 msg_type == EN2M_MSG_ACK
   │                   → {"type":"ack","id":7,"ok":true}
   │                   → 组件把槽位释放
   │
   └─ t = 1600 ms 仍无 ACK
      → 组件发 EN2M_EVENT_ACK_TIMEOUT 事件
      → on_en2m_event 收到 → {"type":"ack","id":7,"ok":false,"error":"timeout"}
```

`(retries + 1) × retry_ms = 4 × 400 = 1600 ms`。三个参数都能调：

| Kconfig | 默认 | 影响 |
|---|---|---|
| `CONFIG_EN2M_CMD_RETRIES` | 3 | 重传次数 |
| `CONFIG_EN2M_CMD_RETRY_MS` | 400 | 重传间隔 |
| `CONFIG_EN2M_MAX_PENDING` | 4 | 同时在等 ACK 的命令数 |

`MAX_PENDING` 满了之后新命令**照常发出去，但不进重传表**，
组件打 `no free retry slot; command N is sent unacknowledged`。
HA 里"一键关闭所有灯"会瞬间产生十几条命令，很容易撞到这个——
部署的设备多就把它调到 8 或 16（每槽 248 字节）。

### 三种 `ok:false`

| `error` | 什么时候 | 谁发的 |
|---|---|---|
| `"timeout"` | 重传完还是没 ACK | `EN2M_EVENT_ACK_TIMEOUT` → `on_en2m_event` |
| `"send_fail"` | `en2m_send_downlink` 直接返回错误（加 peer 失败等） | `handle_host_line` 同步返回 |
| — | MAC 解析失败 | 不发 ack，只发 `{"type":"log","msg":"bad mac"}` |

---

## 5. `housekeeping`：1 秒周期的 `esp_timer`

固件里唯一的周期性逻辑，三件事：

```c
static void housekeeping(void *arg)
{
    static int64_t last_sweep;
    static int64_t last_hello;
    int64_t now = millis();

    /* ① 关配网窗口 */
    if (en2m_get_pairing() && now >= s_pair_until_ms) { ... }

    /* ② 每 2 秒扫一遍 peer 超时 */
    if (now - last_sweep > PEER_SWEEP_PERIOD_MS) {        /* 2000 */
        for (each used peer)
            if (now - p->last_ms > EN2M_OFFLINE_MS)       /* 90000 */
                emit_device(p, "offline"), p->used = false;
    }

    /* ③ 每 30 秒重发一次 hello */
    if (now - last_hello > HELLO_PERIOD_MS) { emit_hello(); }   /* 30000 */
}
```

### 离线判定

`EN2M_OFFLINE_MS`（默认 90 秒）是**协调器独有**的配置项——设备侧编译进去
完全不起作用。它和设备的心跳周期必须配套：

**`OFFLINE_MS ≥ 3 × 全网最大的 HEARTBEAT_MS`**

默认是 90 / 30，刚好 3 倍。只留 2 倍余量的话，一次心跳丢包就会让 HA 里的实体
闪一下"不可用"。如果你把某些电池设备的 `CONFIG_EN2M_HEARTBEAT_MS` 调到了
120 秒，**必须**同时把协调器的 `OFFLINE_MS` 调到 360 秒以上。

被判离线的 peer 会 `used = false`，槽位释放。设备下次说话时
`alloc_peer` 会重新分配，`last_ms` 已经是 0 了，所以会发 `online`——
Bridge 那边会重新走一次 discovery，是期望行为。

### 周期性 `hello`

`hello` 行不只是握手，它是**Bridge 判断"协调器还活着"的唯一依据**：

```python
elif mtype == "hello":
    self.mqtt.publish(f"{self.base}/bridge/state", "online", retain=True)
```

Bridge 每收到一次 `hello` 就把 `bridge/state` 刷成 `online`。所以拔掉 S3
之后 MQTT 上的 `bridge/state` 会停在 `online`，直到 Bridge 进程退出
（`will_set` 的遗嘱消息把它改成 `offline`）。想让"协调器掉了"更快被发现，
可以在 Bridge 里加一个 hello 超时——目前没有。

---

## 6. `usb_host_link.c`：一行一个 JSON

84 行，做的事非常窄：

```c
esp_err_t usb_host_link_start(usb_line_cb_t on_line, void *user);
void usb_host_link_write(const char *line);       /* 自动补 '\n' */
void usb_host_link_printf(const char *fmt, ...);
```

### 读

`usb_rx_task` **一个字节一个字节**读（`usb_serial_jtag_read_bytes(&ch, 1, ...)`），
遇到 `\n` 就把攒下来的行交给回调。行缓冲 **1024 字节**，
超长的行会被**整行丢弃**（`n = 0` 重新开始）而不是截断成半个 JSON——
这样至少不会喂给 `cJSON_Parse` 一个畸形对象。

`\r` 被忽略，所以 CRLF 和 LF 都能处理。

### 写

```c
usb_serial_jtag_write_bytes(line, len, pdMS_TO_TICKS(100));
const char nl = '\n';
usb_serial_jtag_write_bytes(&nl, 1, pdMS_TO_TICKS(20));
```

100 ms 超时意味着**主机不读的时候写会阻塞最多 100 ms 然后丢**。
这是有意的：宁可丢一条状态，也不能让 `en2m` 任务卡在 USB 上
（那会连带影响 mesh 维护和重传）。

### 驱动已装的容错

```c
esp_err_t err = usb_serial_jtag_driver_install(&cfg);
if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { return err; }
```

`ESP_ERR_INVALID_STATE` 表示驱动已经装过了，视为成功。

---

## 7. `emit_hello` 和 `emit_device` 的字段

### `hello`

```json
{"type":"hello","version":2,"role":"coordinator","mac":"AA:BB:CC:DD:EE:FF",
 "fw":"0.4.0-idf","channel":1,"mesh":true,"stack":"esp-idf"}
```

| 字段 | 来源 | 用途 |
|---|---|---|
| `version` | `EN2M_VERSION` | 空中协议版本。Bridge 目前不校验它 |
| `mac` | `en2m_get_self_mac()` | 协调器自己的 MAC |
| `fw` | `EN2M_FW_VERSION` | 固件版本 |
| `channel` | `EN2M_WIFI_CHANNEL` | **排错时最有用的一个字段**：直接看协调器在几信道 |
| `stack` | 常量 `"esp-idf"` | 区别于早期的 Arduino 实现 |

### `device`

```json
{"type":"device","event":"online","mac":"11:22:33:44:55:66",
 "model":"ex-switch","name":"relay1","rssi":-42,"hop":1,
 "via":"AA:BB:CC:DD:EE:FF","node_role":"leaf"}
```

`event == "offline"` 时**只带 `mac`**，其余字段全部省略——因为那些信息已经过期了。
Bridge 那边靠 `event` 分支处理，不会去读不存在的字段。

`node_role` 由 `pkt->role` 映射：`EN2M_ROLE_ROUTER` → `"router"`，
`EN2M_ROLE_LEAF` → `"leaf"`，其它 → `"unknown"`。

---

## 8. `s_peers` 没有加锁

前面架构文档里提过，这里说清楚具体影响。

`s_peers[]` 被三个上下文访问：

| 上下文 | 操作 |
|---|---|
| `en2m` 任务（`on_uplink`） | `alloc_peer` 找空槽并写入；更新 `last_ms` / `rssi` / `hop` / `name` / `model` |
| `usb_rx` 任务（`list` / `unpair`） | 遍历读；`unpair` 把 `used` 置 false |
| `esp_timer` 任务（`housekeeping`） | 遍历读 `last_ms`；超时把 `used` 置 false |

理论上可能出现的问题：

| 竞态 | 后果 | 严重程度 |
|---|---|---|
| `alloc_peer` 和 `housekeeping` 同时挑同一个空槽 | 两个设备抢一个槽位，一个被覆盖 | 低（下次心跳会重建） |
| `strncpy(p->name, ...)` 和 `emit_device` 读 `p->name` 交错 | `list` 输出一个半新半旧的名字 | 很低（下次刷新就对了） |
| `int64_t last_ms` 在 32 位读写上撕裂 | S3 是 32 位，理论上可能读到一个荒谬的时间戳 | 低但**真实存在**，可能导致一次误判离线 |

实践中没观察到问题，因为这些操作都短到几十个指令，而三个任务的触发频率很低
（心跳 30 秒一次、扫描 2 秒一次、`list` 只在 Bridge 启动时）。

**要做产品的话应该加一把 `SemaphoreHandle_t`**，把 `find_peer` /
`alloc_peer` / 遍历都包起来。之所以现在没加，是因为这会让 `on_uplink`
（跑在 `en2m` 任务上）多一个阻塞点，而那个任务还要负责 mesh 维护和重传——
需要先确认锁持有时间足够短。

---

## 9. 编译与烧录

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd firmware/coordinator

echo 'CONFIG_EN2M_WIFI_CHANNEL=6' >> sdkconfig.defaults   # 和所有设备一致！

idf.py set-target esp32s3
idf.py build flash monitor
```

**确认成功**：UART0 上出现

```
I (xxx) coord:    espnow2mqtt coordinator (ESP-IDF)
I (xxx) usb_link: USB Serial/JTAG host link ready
```

以及 USB 上（用 `cat /dev/ttyACM0` 看）

```json
{"type":"hello","version":2,"role":"coordinator",...}
{"type":"log","msg":"coordinator ready (esp-idf mesh)"}
{"type":"log","msg":"mesh init"}
```

**注意**：`idf.py monitor` 连的是 UART0（因为 `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`），
而 NDJSON 走的是 USB。两者不冲突，可以同时看。

板子要求：**必须有原生 USB / Serial-JTAG**（GPIO19/20），
不是外挂 CP2102/CH340 的那种。

---

## 10. 相关文档

- 整条链路怎么串起来 → [architecture.md](architecture.md)
- USB 上每一种 JSON 行的完整字段 → [usb-protocol.md](usb-protocol.md)
- Bridge 怎么消费这些行 → [bridge.md](bridge.md)
- 空中协议 → [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md)
- `en2m` 组件的传输层 API →
  [../components/en2m/README.md](../components/en2m/README.md) 和
  device 仓库的 `docs/mesh.md`
