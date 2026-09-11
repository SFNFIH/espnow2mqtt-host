# 架构总览

## 1. 为什么是四段而不是两段

最直接的做法是让每个 C3 自己连 Wi-Fi、自己连 MQTT。这套系统没这么做，
因为那样每个设备都要：跑完整 TCP/IP + TLS + MQTT 栈（几十 KB RAM）、
存 Wi-Fi 凭据、处理 AP 重连、被 DHCP 和 AP 的信道切换折腾。

改成 ESP-NOW 之后，C3 上**没有 IP 栈**：它只认一个固定信道和一个父节点的 MAC。
代价是需要一个网关把它接回 IP 世界——这就是本仓库。

结构上照的是 **Zigbee2MQTT**：一根 USB 协调器 + 一个用户态 bridge 进程 +
MQTT 上的 JSON。好处是每一段都能单独替换和单独调试。

```
┌──────────────┐  ESP-NOW（固定信道，无 IP）
│  C3 设备     │  en2m_pkt_t 二进制帧，221 字节
│  en2m 全栈   │  载荷 ≤ 160 字节的紧凑 JSON
└──────┬───────┘
       │ 上行：STATE / HELLO / HEARTBEAT / ACK
       │ 下行：CMD
┌──────▼───────┐  只用 en2m 传输层，不建 cluster
│  S3 协调器   │  维护 peer 表、配网窗口、ACK 重传
│  firmware/   │  把帧翻译成 NDJSON
└──────┬───────┘
       │ USB Serial/JTAG，一行一个 JSON
┌──────▼───────┐  合并状态、推导 caps、管理 slug
│   Bridge     │  串口 ↔ MQTT 双向翻译
│ espnow2mqtt/ │  设备名持久化到 devices.json
└──────┬───────┘
       │ MQTT，主题 espnow2mqtt/<slug>/...
┌──────▼───────┐
│  HA 集成     │  建实体（在 espnow2mqtt-ha 仓库）
└──────────────┘
```

## 2. 每一跳的分工

关键在于**每层只做它这层能做好的事**，不越界：

| 层 | 负责 | **不负责** |
|---|---|---|
| C3 设备（`en2m`） | 硬件、属性状态、上报时机、命令执行 | 不知道 MQTT、不知道 HA、不知道自己的 slug |
| S3 协调器 | mesh 树根、peer 表、配网、下行 ACK/重传、帧 ↔ NDJSON | **不解析设备载荷**，只做透传；不知道 MQTT |
| Bridge | 状态合并、caps 推导、slug 管理、MQTT 收发 | 不碰空口；不建 HA 实体（除非开了 `--ha-discovery`） |
| HA 集成 | 建实体、暴露服务 | 不碰串口 |

两个值得特别指出的"不负责"：

**协调器不解析设备的状态载荷。** `on_uplink` 拿到 `pkt->data` 之后，
只尝试 `cJSON_Parse` 一次，成功就整块塞进 `payload` 字段，失败就包成
`{"raw": "..."}`。它从不去看里面有什么键。

这让加新设备类型时**协调器固件一行都不用改**——加一个新 cluster，
设备那边多报一个键，Bridge 那边多认一个 caps，协调器全程无感。

**Bridge 默认不发 HA discovery。** `--ha-discovery` 默认关闭，实体由
[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha) 集成创建。
Bridge 里那一大段 discovery 代码是给"不想装集成、只想用原生 MQTT"的人留的后路。

## 3. 一条状态从设备到 HA 的完整路径

以一个继电器被按下为例。

```
① C3：按键 ISR → en2m_schedule_from_isr → en2m_attribute_write
      → 你的 write 回调拉高 GPIO → 提交属性 → 排一次上报
      （这一段全在 device 仓库，见那边的 docs/state-flow.md）

② C3：en2m 任务组报文
      {"node_role":"leaf","switch":"ON","caps":["switch"]}
      装进 EN2M_MSG_STATE 帧，发给父节点

③ mesh：如果中间有 ROUTER，逐跳转发，hop 每跳 +1

④ S3：en2m 任务收帧 → on_uplink(pkt, rssi, from_mac)
      - alloc_peer(pkt->origin) 更新 peer 表的 last_ms / rssi / hop / via / name / model
      - cJSON_Parse(pkt->data) 成功 → 整块当 payload
      - 补上协调器才知道的三个字段：ts、hop、via
      - usb_host_link_write 写一行 NDJSON

⑤ USB：
      {"type":"state","mac":"AA:..","ts":123456,"hop":1,"via":"BB:..",
       "payload":{"node_role":"leaf","switch":"ON","caps":["switch"]}}

⑥ Bridge：_serial_loop 按 '\n' 切行 → _handle_serial → _on_state
      - _parse_caps 取出 caps（没有就从键名/型号推）
      - merged = dict(dev.last_state); merged.update(payload)   ← 关键
      - switch 归一化成 "ON"/"OFF"
      - 补 hop / via / caps
      - 存回 dev.last_state

⑦ MQTT：
      espnow2mqtt/relay1/state  {"node_role":"leaf","switch":"ON","caps":["switch"],"hop":1,"via":"BB:.."}
      espnow2mqtt/relay1/switch ON        ← 逐个键也单独发一份（不 retain）

⑧ HA 集成：订阅 state 主题，更新 switch 实体
```

**第 ⑥ 步的 `merged` 是整套设计的一个关键点。** 设备侧的上报有三级降级
（载荷超过 160 字节时先丢 `node_role`，再丢 `caps`），Bridge 这边把新载荷
**合并**进上一次的状态而不是替换，所以降级过的报文不会让 HA 丢字段。
设备侧的降级策略见 device 仓库的 `docs/reporting.md`。

## 4. 一条命令从 HA 到设备的完整路径

```
① HA：点开关 → 集成往 espnow2mqtt/relay1/set 发 {"switch":"ON"}

② Bridge：_on_mqtt_message
      - 从主题里截出 slug "relay1"
      - _find_by_slug 找到 Device，拿到它的 MAC
      - 载荷不是 JSON 时容错：裸 "ON" 会被包成 {"switch":"ON"}
      - 分配一个自增的 cmd_id
      - 往串口写 {"type":"cmd","mac":"AA:..","id":7,"payload":{"switch":"ON"}}

③ S3：handle_host_line → type == "cmd"
      - en2m_mac_from_str 解析 MAC
      - payload 重新序列化成紧凑字符串（截到 EN2M_DATA_MAX）
      - en2m_send_downlink(mac, id, payload, len)
        id 非 0 → 组件把这一帧存进重传表

④ mesh：查反向路由表找下一跳；查不到就直接发（可能设备就在一跳内）
      默认重传 4 次，间隔 400 ms，约 1.6 秒后判超时

⑤ C3：en2m 任务收 CMD → 解析 JSON → 派发命令
      → 你的 command / write 回调 → 硬件动作
      → 回一个 EN2M_MSG_ACK，然后发一份新的状态上报

⑥ S3：收到 ACK → 从重传表删除 → 转成 {"type":"ack",...,"ok":true}
      超时了 → EN2M_EVENT_ACK_TIMEOUT 事件 → {"type":"ack",...,"ok":false,"error":"timeout"}

⑦ Bridge：_on_ack
      从 pending 表取回原始命令内容和耗时
      ok:true  → LOG.debug
      ok:false → LOG.warning("command 7 to living_room failed: timeout")
      两种都发 <base>/<slug>/command_result（非 retained）

⑧ HA 集成：订阅 command_result
      ok:true  → LOG.debug
      ok:false → 在 HA 事件总线上 fire espnow2mqtt_command_failed

⑨ 与此同时第 ⑤ 步的新状态走完 3 节那条路，HA 里的开关落到 ON
```

注意第 ⑤ 步：**设备既回 ACK 又发一份新状态**。ACK 说"我收到了"，
状态说"我现在是这样"。HA 里那个开关的最终值来自后者——
这是为什么点了开关之后 HA 显示的是**设备真实到达的状态**，
而不是乐观更新。

ACK 这条路（⑥⑦⑧）存在的意义是**失败的时候**。
命令失败不会产生任何新的状态上报，所以没有 ⑦⑧ 的话，
"命令丢了"和"设备本来就是这个值"在 MQTT 上完全一样。
主题格式见 [mqtt.md §11](mqtt.md#11-slugcommand_result)。

## 5. 为什么协调器需要一张 peer 表

`en2m` 组件自己有邻居表和路由表，但那是**传输层**的，记的是"下一跳是谁"。
协调器还需要一张**应用层**的表，记的是主机关心的东西：

```c
typedef struct {
    bool used;
    uint8_t mac[6];      /* 设备的 origin MAC，不是下一跳 */
    char model[12];      /* 从帧里抄的 */
    char name[16];       /* 同上，Bridge 拿它当 slug */
    uint8_t role;        /* leaf / router */
    uint8_t hop;         /* 离协调器几跳 */
    int8_t rssi;         /* 最后一跳的 RSSI */
    uint8_t via[6];      /* 最后一跳的 MAC */
    int64_t last_ms;     /* 最后一次听到它 */
} host_peer_t;

static host_peer_t s_peers[EN2M_MAX_ROUTES];   /* 默认 32 个 */
```

它撑起三件事：

1. **`online` / `offline` 事件。** `last_ms` 超过 `EN2M_OFFLINE_MS`（默认 90 秒）
   就发一条 `offline`，HA 里的实体变成不可用。
2. **`list` 命令。** Bridge 启动时会发一个 `{"type":"list"}`，协调器把整张表
   逐条 `emit_device(..., "info")` 吐出来，这样 Bridge 重启不用等设备心跳。
3. **`name` / `model` 的来源。** 这两个字段在**每一个** `en2m_pkt_t` 里都带着
   （`char model[12]` + `char name[16]`），协调器抄下来，Bridge 拿 `name`
   生成 slug。所以设备改个名字重启一下，MQTT 主题就跟着变了。

表满了会打 `peer table full` 并丢掉这一帧的记账（帧本身仍然转发给主机）。
32 个不够就调 `CONFIG_EN2M_MAX_ROUTES`。

## 6. 协调器的三个执行上下文

协调器固件本身**没有主循环**，`app_main` 装配完就返回。三件事并行跑：

| 上下文 | 谁创建 | 做什么 |
|---|---|---|
| `en2m` 任务 | `en2m_mesh_init`（组件内部） | 收帧、mesh 维护、重传。`on_uplink` / `on_log` 在这上面跑 |
| `usb_rx` 任务 | `usb_host_link_start` | 从 USB Serial/JTAG 一个字节一个字节读，按 `\n` 切行，回调 `handle_host_line` |
| `coord_housekeeping` | `esp_timer`（1 秒周期） | 关配网窗口、扫 peer 超时、每 30 秒发一次 `hello` |
| 事件循环任务 | ESP-IDF 默认 | `EN2M_EVENT_ACK_TIMEOUT` / `RX_DROPPED` 在这上面变成 NDJSON |

这里有一个**真实的并发注意点**：`s_peers[]` 被 `en2m` 任务（`on_uplink`）、
`usb_rx` 任务（`list` / `unpair`）和 `esp_timer` 任务（超时扫描）三者访问，
**没有加锁**。实践中没炸，是因为写入都是小的标量赋值、而且 ESP32-S3 虽然双核但
这几个任务的操作短到极难重叠。要做产品的话这里应该加一把 mutex——
细节和影响见 [coordinator.md](coordinator.md#8-s_peers-没有加锁)。

`usb_host_link_write` 也同理：`en2m` 任务、`usb_rx` 任务、`esp_timer` 都会调它。
`usb_serial_jtag_write_bytes` 本身是线程安全的（驱动内部有环形缓冲），
但**两行 NDJSON 可能交错**——实际没观察到，因为每次写都是一次完整的
`write_bytes` 调用加一个换行符。

## 7. 日志走 UART0，协议走 USB

这是协调器固件一个容易踩的配置细节：

```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n
```

| 通道 | 内容 |
|---|---|
| **USB Serial/JTAG** | 只走 NDJSON 协议，`usb_host_link.c` 独占 |
| **UART0** | `ESP_LOGI` 等调试日志 |

如果把控制台也放到 USB 上，`ESP_LOGI` 的输出会和 NDJSON 混在一条流里，
Bridge 那边就会不停地打 `non-json: ...`。所以 C3 设备用
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`（方便看日志），
S3 协调器**必须**用 UART0。

想在主机上看协调器的日志，有两个办法：

1. 接 UART0 的 TX（多数 S3 板子上有独立的 USB-UART 芯片或者排针）
2. 看 Bridge 打的 `coord: ...` ——协调器的 `usb_log()` 会把 mesh 层的日志
   （`mesh init`、`parent stale`、`rx_dropped=N`）当 NDJSON 发过来，
   Bridge 收到 `{"type":"log"}` 就 `LOG.info("coord: %s", ...)`

第 2 条是设计好的：`cfg.on_log = on_mesh_log` 把组件的日志接管过来了，
所以 mesh 层说的话不需要 UART 也能看到。

## 8. Bridge 的三个线程

```
主线程       连 MQTT → 开串口 → 发 ping + list → 然后每 0.5 秒空转等退出信号
serial 线程  阻塞读串口，按 '\n' 切行，_handle_serial（daemon）
paho 线程    mqtt.loop_start() 起的，跑 _on_mqtt_message
```

`_serial_write` 用 `self._lock` 保护（串口写可能从主线程和 paho 线程同时来），
读侧是单线程所以不用锁。`self.devices` 字典在两个线程里都会被改，
**没有加锁**——Python 的 dict 操作在 GIL 下是原子的，够用。

## 9. 设备身份与持久化

| 东西 | 存在哪 | 掉电/重启会丢吗 |
|---|---|---|
| 设备的属性值（开关状态、亮度…） | **C3 自己的 NVS** | 不丢，见 device 仓库的 `docs/persistence.md` |
| peer 表（谁在线、RSSI、hop） | S3 的 RAM | **S3 重启就丢**，靠设备心跳重建 |
| 设备名 / 型号 / node_role | Bridge 的 `devices.json` | 不丢 |
| MQTT 上的 `state` / `availability` | broker 的 retain | 不丢（retain=true） |
| HA 实体 | HA 的配置 | 不丢 |

所以整条链路里**唯一会丢的是 S3 的 peer 表**，而它丢了的后果只是
"HA 里的设备短暂变成不可用，直到下一次心跳（默认 30 秒内）"。
Bridge 重启则完全无感，因为它启动时会发 `list` 把表要回来，
而 `devices.json` 里的名字也还在。

## 10. 相关文档

- S3 固件细节 → [coordinator.md](coordinator.md)
- Bridge 细节 → [bridge.md](bridge.md)
- USB 上每一种 JSON 行 → [usb-protocol.md](usb-protocol.md)
- MQTT 主题与 payload → [mqtt.md](mqtt.md)
- 空中协议 → [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md)
- **设备侧的库架构、状态流转、回调契约** →
  [espnow2mqtt-device 的 docs/](https://github.com/SFNFIH/espnow2mqtt-device/tree/main/docs)
