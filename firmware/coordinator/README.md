# `firmware/coordinator` — ESP32-S3 USB 协调器固件

**烧进插在 HA 主机上那块 ESP32-S3 里的固件。**
一头是 ESP-NOW 的 mesh，一头是 USB 上的 NDJSON（一行一个 JSON）。
Python 的 Bridge（[`espnow2mqtt/`](../../espnow2mqtt)）消费这些行并转成 MQTT。

这份 README 管"怎么烧起来、怎么确认它活着"。
想知道**每一行代码为什么这么写**，看
[docs/coordinator.md](../../docs/coordinator.md)。

| | |
|---|---|
| 目标芯片 | **ESP32-S3**（不是 C3） |
| 角色 | `EN2M_ROLE_COORDINATOR` |
| 启动函数 | `en2m_mesh_init()`（**不是** `en2m_start()`） |
| endpoint / cluster | 没有，只有传输层 |
| 主机接口 | USB Serial/JTAG 上的 NDJSON |
| 控制台 | **UART0**（故意和 USB 分开） |

---

## 硬件要求

**只接一根 USB-C 到 HA 主机，不需要任何其它接线。**

但板子有一条硬要求：**必须是带原生 USB / Serial-JTAG 的 S3**
（GPIO19/20 直连 USB），**不是**外挂 CP2102 / CH340 的那种。
协调器和主机之间走的就是原生 USB。

买板子的时候看两点：

- 板上有没有**两个** USB 口（一个 UART 一个原生 USB），或者只有一个但标着 "USB"
- 原理图里 GPIO19/20 是不是接到 USB D−/D+

S3 **不连家庭 Wi-Fi**，只锁定一个信道跑 ESP-NOW，所以不需要配网，插上就能用。

---

## 编译烧录

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
cd firmware/coordinator

echo 'CONFIG_EN2M_WIFI_CHANNEL=6' >> sdkconfig.defaults   # ← 和所有设备一致！

idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

**信道是最重要的一项配置。** 协调器、路由器和所有设备必须一致，
不一致时完全静默：没有错误、没有日志，设备就是找不到父节点。
改完要 `idf.py fullclean` 再 build。

选信道的策略（和家里 AP 错开）见
[device 仓库的 docs/kconfig.md](https://github.com/SFNFIH/espnow2mqtt-device/blob/main/docs/kconfig.md)。

---

## 确认成功

### UART0 上（`idf.py monitor`）

```
I (xxx) coord:    espnow2mqtt coordinator (ESP-IDF)
I (xxx) usb_link: USB Serial/JTAG host link ready
```

### USB 上（NDJSON）

```bash
cat /dev/ttyACM0
```

```json
{"type":"hello","version":2,"role":"coordinator","mac":"...","fw":"0.4.0-idf","channel":6,...}
{"type":"log","msg":"coordinator ready (esp-idf mesh)"}
{"type":"log","msg":"mesh init"}
```

`hello` 每 30 秒重发一次，所以你随时接上都能在半分钟内看到它。

> **`idf.py monitor` 连的是 UART0，NDJSON 走的是 USB，两者不冲突**，
> 可以同时开两个终端看。这是 `sdkconfig.defaults` 里
> `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` 的用意——
> 把日志和数据分到两条物理链路上，日志就不会污染协议。

### 有设备上线时

```json
{"type":"device","event":"online","mac":"aa:bb:...","name":"relay1","model":"ex-switch",
 "rssi":-42,"hop":1,"via":"...","node_role":"leaf"}
{"type":"state","mac":"aa:bb:...","ts":123456,"hop":1,"via":"...",
 "payload":{"switch":"OFF","caps":["switch"]}}
```

看不到设备的话先开配网窗口（下一节）。

---

## 手动测协调器（不用 Bridge）

协调器的 USB 协议是纯文本的，可以直接对话。**排错时非常有用**——
能立刻分清问题在固件还是在 Bridge。

```bash
# 开一个终端看输出
cat /dev/ttyACM0

# 另一个终端写命令（每条一行 JSON）
echo '{"type":"ping"}' > /dev/ttyACM0                    # → {"type":"pong","ms":...}
echo '{"type":"pair","seconds":60}' > /dev/ttyACM0       # 开 60 秒配网窗口
echo '{"type":"list"}' > /dev/ttyACM0                    # 列出已知设备
echo '{"type":"cmd","mac":"aa:bb:cc:dd:ee:ff","id":1,"payload":{"switch":"ON"}}' > /dev/ttyACM0
echo '{"type":"unpair","mac":"aa:bb:cc:dd:ee:ff"}' > /dev/ttyACM0
```

**`pair` 是新设备第一次上线的必要步骤。** 不开窗口的话协调器会静默丢掉
陌生设备的上行。窗口最长 300 秒，到期自动关并发一条
`{"type":"log","msg":"pairing_disabled"}`。

`cmd` 里的 `id` 非零时组件会**重传直到收到 ACK**，
超时后发一条 `{"type":"ack","ok":false,"error":"timeout"}`。

全部消息类型和字段见
[docs/usb-protocol.md](../../docs/usb-protocol.md)。

---

## 它和设备固件的区别

```c
en2m_config_t cfg = {                      /* 不是 en2m_device_config_t */
    .role      = EN2M_ROLE_COORDINATOR,
    .name      = "coordinator",
    .model     = "s3-coord",
    .fw        = EN2M_FW_VERSION,
    .channel   = EN2M_WIFI_CHANNEL,
    .on_uplink = on_uplink,                /* ← 协调器独有 */
    .on_log    = on_mesh_log,
};

en2m_mesh_init(&cfg);                      /* 不是 en2m_start */
```

协调器**没有自己的 cluster**——它不是一个设备，是一个网关。
所以它只用传输层，和 device 仓库的 `firmware/router` 是同一个模式，
只是多了 `on_uplink`（把收到的帧交给 USB）。

`app_main` 里除了初始化只剩一个 `esp_timer`：

| 任务 | 谁在跑 |
|---|---|
| 收 mesh 帧 → 发 USB | `on_uplink`，跑在 en2m 任务上 |
| 收 USB 行 → 发 mesh | `handle_host_line`，跑在 usb_link 任务上 |
| 配网窗口到期、设备离线判定、周期 `hello` | `housekeeping`，1 秒一次的 `esp_timer` |
| 下行重传和 ACK 超时 | 组件自己，不用管 |

**没有 `while (1)`。**

---

## 常见坑

| 现象 | 原因 |
|---|---|
| `/dev/ttyACM0` 不存在 | 板子没有原生 USB，或者烧完没重新插拔 |
| UART0 有日志但 USB 上什么都没有 | 用错了口：NDJSON 在原生 USB 上，不在 UART 上 |
| USB 上只有 `hello`，没有任何设备 | 没开 `pair` 窗口；或者设备信道不一致 |
| 设备上线又马上 `offline` | 设备侧供电抖动导致重启，或者 RSSI 太差心跳丢了 |
| Bridge 说串口被占用 | `idf.py monitor` 抢了 USB。monitor 应该连 UART0，检查是不是指定了 `-p /dev/ttyACM0` |
| `peer table full` | 设备数超过 `CONFIG_EN2M_MAX_ROUTES`，调大它 |
| 日志里刷 `rx_dropped=` | 队列扛不住，调大 `CONFIG_EN2M_QUEUE_LEN` |

排错流程见 [docs/troubleshooting.md](../../docs/troubleshooting.md)。

---

## 延伸阅读

- [docs/coordinator.md](../../docs/coordinator.md) — 这份固件的逐节精讲
- [docs/usb-protocol.md](../../docs/usb-protocol.md) — USB 上每种 JSON 行的完整字段
- [docs/bridge.md](../../docs/bridge.md) — Python Bridge 怎么消费这些行
- [docs/architecture.md](../../docs/architecture.md) — 整条链路怎么串起来
- [protocol/PROTOCOL.md](../../protocol/PROTOCOL.md) — 空中协议
- [components/en2m/README.md](../../components/en2m/README.md) — 传输层 API 速查
