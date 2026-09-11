# 排查手册（按症状）

先做**三步分诊**，它能把 90% 的问题定位到某一段链路上。然后跳到对应的小节。

```
[ C3 设备 ] --ESP-NOW--> [ S3 协调器 ] --USB--> [ Bridge ] --MQTT--> [ HA 集成 ]
      ③                        ②                    ①                  ①/④
```

## 三步分诊

### 第 1 步：Bridge 和 S3 通不通？

```bash
mosquitto_sub -t 'espnow2mqtt/bridge/#' -v
```

| `bridge/state` | `bridge/info` | 结论 | 跳到 |
|---|---|---|---|
| 没有任何输出 | — | **Bridge 没跑，或 broker/base topic 不对** | [§1](#1-bridge-没连上-mqtt) |
| `online` | **空** | Bridge 活着但从没收到 S3 的 hello | [§2](#2-bridge-收不到-s3串口问题) |
| `online` | 有内容 | 上游正常，问题在设备或 HA 侧 | [§3](#3-设备不上线) / [§6](#6-ha-里没有实体) |
| `offline` | 有内容 | Bridge 挂了（`info` 是残留） | [§1](#1-bridge-没连上-mqtt) |

### 第 2 步：设备上来了吗？

```bash
mosquitto_sub -t espnow2mqtt/bridge/devices -C 1 | python3 -m json.tool
```

| 结果 | 跳到 |
|---|---|
| `[]` 空数组 | [§3](#3-设备不上线) |
| 有设备但 `"online": false` | [§4](#4-设备时上时下) |
| 有设备且在线，但 `<slug>/state` 没内容 | [§5](#5-设备在线但没有状态) |
| 一切正常但 HA 里看不到 | [§6](#6-ha-里没有实体) |

### 第 3 步：日志里有什么？

**Bridge 的日志是最重要的信息源。** 三种跑法的看法：

```bash
journalctl -u espnow2mqtt -f            # systemd
docker compose logs -f                  # docker
# HA Add-on：Add-on 页面的 Log 标签
```

调协议问题时**必须开 `-v`**，否则看不到非 JSON 行、`pong`、成功的 `ack`。

全部日志消息的含义见 [§10](#10-日志消息全表)。

---

## 1. Bridge 没连上 MQTT

### 症状

MQTT 上 `espnow2mqtt/#` 完全没东西；或者 `bridge/state` 是 `offline`。

### 排查

**1.1 进程在跑吗**

```bash
systemctl status espnow2mqtt
docker compose ps
```

**1.2 日志里有没有 `MQTT connected`**

```
INFO espnow2mqtt: connecting MQTT 192.168.1.10:1883
INFO espnow2mqtt: MQTT connected rc=Success
```

没有第二行就是连不上。

| 日志/现象 | 原因 | 解决 |
|---|---|---|
| 卡在 `connecting MQTT ...` 不动 | 地址/端口不通 | `telnet <host> 1883` 试连通性 |
| `ConnectionRefusedError` | broker 没跑，或端口错 | 检查 broker |
| `MQTT connected rc=Not authorized` | 用户名/密码错，或 broker 禁了匿名 | 加 `--mqtt-user` / `--mqtt-pass` |
| `MQTT connected rc=Success` 但你订不到东西 | **base topic 不一致** | 见下 |

**1.3 base topic 对不对**

这是最阴的一个。用通配符看 broker 上到底有什么：

```bash
mosquitto_sub -t '#' -v | head -50
```

如果你看到 `myprefix/bridge/state online`，说明 Bridge 用的是
`--base-topic myprefix`，而你在订 `espnow2mqtt/...`。

**Bridge、HA 集成、你的 `mosquitto_sub` 命令三处必须一致。**

**1.4 Add-on 的 MQTT 自动发现失败**

Add-on 日志里如果看到：

```
MQTT service not discovered; falling back to core-mosquitto
```

说明 supervisor 没提供 MQTT 服务信息。这时**用户名密码是空的**，
如果你的 Mosquitto 禁了匿名访问就会连不上。解决：

- 确认 Mosquitto add-on 已安装并运行
- 确认 HA 里配置了 MQTT 集成（**这是 supervisor 暴露 mqtt 服务的前提**）
- 或者在 add-on 配置里手填 `mqtt_host` / `mqtt_user` / `mqtt_password`

### 顺手确认 broker 本身是好的

```bash
mosquitto_sub -t test/x &
mosquitto_pub -t test/x -m hello
# 应该看到 hello
```

---

## 2. Bridge 收不到 S3（串口问题）

### 症状

`bridge/state` 是 `online`，但 `bridge/info` **空**，`bridge/devices` 是 `[]`，
日志里**没有** `coordinator hello`。

> `bridge/state = online` **不代表 S3 在线**。`_on_mqtt_connect` 无条件发
> `online`，此时它还没和 S3 说过一句话。判断 S3 的唯一依据是
> `bridge/info` 有内容 + 日志里有 `coordinator hello`。

### 2.1 串口路径对不对（最常见）

```bash
ls -l /dev/serial/by-id/
```

期望看到：

```
usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00 -> ../../ttyACM0
```

| 现象 | 原因 |
|---|---|
| 目录不存在 / 空 | **S3 没插，或 USB 线只有供电没有数据** |
| 有条目，但 `ID_VENDOR_ID` 是 `10c4`/`1a86` | 这块板子是外置 USB-UART 芯片，**不能当协调器**（见 [§2.4](#24-板子不对)） |
| 有正确的条目但 Bridge 连的是别的 | 自动探测选错了，见下 |

**别用自动探测。** `autodetect_port()` 遍历串口、第一个匹配关键词的就返回。
同时插着 S3 和 C3 开发板时它可能选中 C3——C3 不会回 `hello`，
于是就是这一节的症状。

```bash
# 永远显式指定，永远用 by-id
--port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00
```

日志的第二行会告诉你它实际打开了什么：

```
INFO espnow2mqtt: opening serial /dev/ttyACM0 @ 115200
```

### 2.2 串口被占用

**串口是独占的。**

```bash
sudo fuser -v /dev/ttyACM0
```

| 占用者 | 解决 |
|---|---|
| 另一个 Bridge | `systemctl stop espnow2mqtt` / `docker compose down` |
| `idf.py monitor` | Ctrl+] |
| `cat /dev/ttyACM0` 忘了关 | Ctrl+C |
| `ModemManager` | 见 [deployment.md §5.4](deployment.md#54-串口被占用) |

被占用时 Bridge 会抛 `serial.serialutil.SerialException: could not open port`
并退出（`start()` 里没有 try），所以进程会反复重启。

### 2.3 权限

```
PermissionError: [Errno 13] Permission denied: '/dev/ttyACM0'
```

```bash
sudo usermod -aG dialout $USER    # 重新登录才生效
groups                            # 确认
```

有些发行版用 `uucp` 组，用 `ls -l /dev/ttyACM0` 看第四列。

### 2.4 板子不对

协调器固件用 `usb_serial_jtag` 驱动，**需要芯片原生 USB**。

```bash
udevadm info -n /dev/ttyACM0 | grep -E 'ID_VENDOR_ID|ID_MODEL_ID'
```

| VID:PID | 是什么 | 能当协调器？ |
|---|---|---|
| `303a:1001` | Espressif USB JTAG/serial debug unit | **✓** |
| `10c4:ea60` | Silabs CP210x | ✗ |
| `1a86:7523` | WCH CH340 | ✗ |
| `0403:6001` | FTDI | ✗ |

后三种是外置 USB-UART 芯片，`usb_serial_jtag_driver_install()` 根本不会有数据。
**必须用 USB 口直接接在 S3 芯片上的板子**（比如 ESP32-S3-DevKitC-1 的
"USB" 口，不是 "UART" 口）。很多 DevKit 有两个 Type-C，插错了就是这个症状。

### 2.5 S3 固件没跑

USB 上的 NDJSON 流里**没有** `ESP_LOGx` 输出（console 在 UART0），
所以看不到崩溃回溯。判断办法：

**手工探活**（先停 Bridge）：

```bash
cat /dev/ttyACM0 &
printf '{"type":"ping"}\n' > /dev/ttyACM0
# 期望：{"type":"pong","ms":12345}
```

没有 `pong` 就是 S3 没在跑或者固件不对。

**看真正的日志**（接 UART0）：

```bash
cd firmware/coordinator
idf.py -p /dev/ttyUSB0 monitor    # UART0 对应的设备，不是 ttyACM0
```

如果板子没引出 UART0，临时把 console 切回 USB 重新烧：

```
# firmware/coordinator/sdkconfig.defaults
CONFIG_ESP_CONSOLE_UART_DEFAULT=n
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

这样 NDJSON 流会被日志污染（Bridge 会把日志行当非 JSON 丢掉，功能还能跑），
但你能看到崩溃信息。**调完记得改回去。**

### 2.6 只看到非 JSON 行

开 `-v` 后日志里一直刷：

```
DEBUG espnow2mqtt: non-json: I (312) cpu_start: Pro cpu start user code
DEBUG espnow2mqtt: non-json: ets Jun  8 2016 00:22:57
```

这说明**S3 的 console 配在了 USB 上**，日志正在污染 NDJSON 流。
改 `sdkconfig.defaults`（见 [§2.5](#25-s3-固件没跑)）重新烧。

偶尔出现几行非 JSON 是正常的（S3 复位时 ROM bootloader 的输出）。
持续刷才是问题。

---

## 3. 设备不上线

### 症状

S3 正常（`bridge/info` 有内容），但 `bridge/devices` 是 `[]`，
日志里没有 `device online`。

### 3.1 信道不匹配（最常见）

ESP-NOW 跑在**固定信道**上。协调器和设备必须配同一个。

```bash
mosquitto_sub -t espnow2mqtt/bridge/info -C 1 \
  | python3 -c 'import json,sys; print("coordinator channel =", json.load(sys.stdin)["channel"])'
```

设备侧：

```bash
cd <device-repo>/examples/relay_switch
idf.py menuconfig     # Component config → en2m → Wi-Fi channel used by ESP-NOW
# 或者直接看 sdkconfig
grep EN2M_WIFI_CHANNEL sdkconfig
```

**两边不一致 ⇒ 设备收不到 beacon ⇒ 永远选不上父节点 ⇒ 一个字节都不会上来。**

默认都是 `1`。如果你改过一边，改另一边。

### 3.2 配网窗口没开

新设备只能在配网窗口内入网。

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 120
```

日志里必须立刻出现：

```
INFO espnow2mqtt: MQTT espnow2mqtt/bridge/request/permit_join => 120
INFO espnow2mqtt: coord: pairing_enabled
```

| 现象 | 原因 |
|---|---|
| 第一行都没有 | **base topic 不对**，或者 MQTT 订阅没建立 |
| 有第一行没第二行 | 串口写失败 / S3 没在跑 → [§2](#2-bridge-收不到-s3串口问题) |
| 两行都有 | 配网开了，问题在设备侧 |

窗口最长 300 秒（S3 会夹 `[1,300]`），到时自动关：

```
INFO espnow2mqtt: coord: pairing_disabled
```

> **别往 `permit_join` 发 retained 消息**（`mosquitto_pub -r`）。
> retained 命令会在每次 Bridge 重连时重放，配网窗口就永远开着。
> 清理：`mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -r -n`

### 3.3 设备侧根本没跑

在设备上接 USB 看它的日志（C3 例程的 console **在 USB 上**，和协调器相反）：

```bash
cd <device-repo>/examples/relay_switch
idf.py -p /dev/ttyACM1 monitor
```

`en2m` 组件只打两条 INFO 日志：

| 日志 | 含义 |
|---|---|
| `mesh init` | 组件初始化完成。**没有这条 = 固件没跑起来或 `en2m_start` 失败** |
| `parent=7C:DF:A1:00:11:22 cost=1 rssi=-58` | **选上父节点了**，这条是入网成功的标志 |

只有 `mesh init` 没有 `parent=...` ⇒ 收不到 beacon ⇒ 信道不对，
或者离协调器太远，或者协调器没在发 beacon。

（这两条日志可以被 `cfg.mesh.on_log` 重定向，如果你的应用设了它，
日志会走你的回调而不是 `ESP_LOGI`。）

### 3.4 距离/信号

协调器每 `EN2M_BEACON_INTERVAL_MS`（默认 5000 ms）发一次 beacon。
设备要收到 beacon 才能选父节点。

- 先把设备**放到协调器旁边 30 cm** 试，能上线就是距离问题
- 上线后看 RSSI：`bridge/devices` 里的 `rssi`

| RSSI | 评价 |
|---|---|
| > -60 dBm | 很好 |
| -60 ～ -75 | 正常 |
| -75 ～ -85 | 临界，会偶发丢包 |
| < -85 | **不可用**，需要加 router 节点 |

加 router：用 `firmware/router`（device 仓库）烧一块常电的 C3 放在中间。
router 会转发流量，设备的 `hop` 会变成 2。

### 3.5 peer 表满了

日志里：

```
INFO espnow2mqtt: coord: peer table full
```

S3 的 `s_peers[]` 长度是 `EN2M_MAX_ROUTES`（默认 **32**）。满了之后
新设备的帧**直接被丢弃**（连状态都不转发）。

解决：改 `CONFIG_EN2M_MAX_ROUTES` 重编**协调器和设备**两边的固件。
注意这会增加内存占用（每个 route 24 字节，每个 peer 44 字节）。

### 3.6 设备用的是不同的协议版本

```bash
mosquitto_sub -t espnow2mqtt/bridge/info -C 1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])'
```

当前 `EN2M_VERSION = 2`。设备侧的值在
`components/en2m/include/en2m_proto.h`。**两边必须一致**，
不一致的帧会在传输层被丢掉，没有任何日志。

---

## 4. 设备时上时下

### 症状

日志里反复：

```
INFO espnow2mqtt: device online AA:BB:CC:DD:EE:FF (living_room) role=leaf hop=1 via=7C:DF:A1:00:11:22
INFO espnow2mqtt: device offline AA:BB:CC:DD:EE:FF
INFO espnow2mqtt: device online AA:BB:CC:DD:EE:FF ...
```

### 4.1 先理解离线判定的时间窗

| 参数 | 默认 | 作用 |
|---|---|---|
| `EN2M_HEARTBEAT_MS` | 30000 ms | 设备每 30 s 发一次心跳 |
| `EN2M_OFFLINE_MS` | 90000 ms | S3 超过 90 s 没收到任何帧就判离线 |
| `PEER_SWEEP_PERIOD_MS` | 2000 ms | S3 每 2 s 扫一遍 peer 表 |

所以离线事件在**断电后 90～92 s** 才会发出。允许连丢 2 次心跳。

**`last_ms` 被任何上行帧刷新**（不只是心跳），所以一个每 30 s 上报状态的设备
即使心跳全丢也不会被判离线。

反过来说，**判离线意味着 90 秒内一个字节都没上来**——这是很严重的丢包。

### 4.2 信号太弱

看 RSSI（[§3.4](#34-距离信号)）。< -85 dBm 就会这样。

### 4.3 供电不足

ESP32-C3 在 Wi-Fi 发射瞬间的峰值电流可以到 300～400 mA。电源撑不住就会
掉电复位，表现正是"反复上下线"。

| 检查 | 怎么做 |
|---|---|
| 电源能力 | 用能提供 ≥ 500 mA 的适配器，别用电脑 USB 口（尤其是 hub） |
| 去耦电容 | 模块 3V3 引脚旁加 100 µF 电解 + 100 nF 陶瓷 |
| USB 线 | 细线的压降很大，换短的粗线 |

在设备上接 `idf.py monitor`，如果看到反复的启动横幅
（`ESP-ROM:esp32c3-...`）就是复位，不是通讯问题。

### 4.4 中继节点不稳

如果设备的 `hop` 是 2 或更多，它依赖中继。看 `bridge/devices` 里它的 `via`：

```bash
mosquitto_sub -t espnow2mqtt/bridge/devices -C 1 \
  | python3 -c 'import json,sys; [print(d["mac"], "hop="+str(d["hop"]), "via="+d["via"], d["online"]) for d in json.load(sys.stdin)]'
```

**如果 `via` 对应的那个 router 也在上下线，先修它。** 一个不稳的 router 会
让它下面所有设备都不稳。

### 4.5 `rx_dropped` 在涨

```
INFO espnow2mqtt: coord: rx_dropped=47
```

协调器的接收队列（`EN2M_QUEUE_LEN`，默认 8）满了，帧被丢弃。
`N` 是**累计值**，只会涨。

| 增长速度 | 评价 |
|---|---|
| 几小时涨 1～2 | 正常，忽略 |
| 每分钟涨几十 | **过载**，需要处理 |

处理办法（按优先级）：

1. 调长设备的上报间隔（device 仓库 `docs/kconfig.md` 的
   `EN2M_REPORT_INTERVAL_*`）
2. 调大协调器的 `CONFIG_EN2M_QUEUE_LEN`（每个槽位 240 字节）
3. 减少设备数量，或者分成两个协调器

---

## 5. 设备在线但没有状态

### 症状

`bridge/devices` 里 `"online": true`，但 `espnow2mqtt/<slug>/state` 没内容。

### 5.1 先确认 slug

```bash
mosquitto_sub -t 'espnow2mqtt/+/state' -v
```

这会列出所有实际存在的状态主题。**slug 可能和你以为的不一样：**

| 设备有 `name` | slug |
|---|---|
| `"Living Room"` | `living_room`（空格→下划线，全小写） |
| 没有 | `aabbccddeeff`（MAC 去冒号小写） |

### 5.2 等一个上报周期

叶子节点默认 `EN2M_REPORT_INTERVAL_LEAF_MS = 30000` ms，
常电节点 15000 ms。**刚上线可能要等 30 秒。**

入网成功后会有一次快速上报（`on_link_change` 里
`next_report_ms = now + 200`），所以正常情况下几百毫秒内就该有第一条状态。
30 秒还没有就不正常了。

### 5.3 设备只发心跳不发状态

`EN2M_MSG_HEARTBEAT` 帧**不带 payload**，只产生 `device`/`info` 行，
不产生 `state` 行。

如果设备侧的上报模式设成了 `EN2M_REPORT_MANUAL` 而应用又没调
`en2m_report_now()`，就会出现"设备在线但永远没状态"。

检查设备固件里的 `cfg.report_mode`。四种模式的区别见 device 仓库
`docs/reporting.md`。

### 5.4 `state` 里出现 `raw` 字段

```json
{"raw":"{\"switch\":\"ON\",\"brig"}
```

**这是明确的故障信号**：设备发的 payload 不是合法 JSON，S3 包了一层
（见 [usb-protocol.md §5](usb-protocol.md#5-s3--主机state)）。

99% 的情况是**设备侧自己拼的字符串超过了 `EN2M_DATA_MAX = 160` 字节被截断**。
用 `en2m` 组件的内置上报不会有这个问题（它有三级降级机制）；
手写 `en2m_send_state()` 才会。

### 5.5 `state` 里出现 `value` 字段

```json
{"value": 42}
```

说明 S3 发来的 `payload` 不是 JSON 对象（是数字/字符串/数组）。
Bridge 的兜底是包成 `{"value": …}`。同样是设备侧的 bug。

### 5.6 Bridge 刚重启，状态是"残缺"的

`last_state` **不持久化**。Bridge 重启后第一条上报如果被 160 字节挤掉了
`caps`/`node_role`，`<slug>/state` 会比重启前**少几个字段**——
而且因为是 retained，它会**覆盖掉 broker 上那份完整的旧消息**。

几轮上报之后会补齐。等 1～2 分钟，或者手工 toggle 一下设备强制上报。
详见 [bridge.md §10](bridge.md#10-devicesjson持久化了什么没持久化什么)。

### 5.7 扁平主题订不到

```bash
mosquitto_sub -t 'espnow2mqtt/living_room/switch' -v
# 什么都没有
```

**扁平主题不 retain。** 新订阅者要等下一次上报（最多 30 秒）才有值。

要"订上来就有值"，用 `<slug>/state`。见
[mqtt.md §9](mqtt.md#9-slugkey-扁平主题)。

---

## 6. HA 里没有实体

### 6.1 base topic 不一致

**最常见的原因。** Bridge 的 `--base-topic` 和 HA 集成配置里的必须一样。

```bash
mosquitto_sub -t '#' -v | head -20     # 看 Bridge 实际用的前缀
```

然后在 HA 里 **Settings → Devices & Services → espnow2mqtt → Configure** 核对。

### 6.2 集成没装

实体是由 [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha) 创建的，
**不是** Bridge。Bridge 默认**不发** MQTT Discovery。

装法见那个仓库的 README（HACS 自定义仓库，或者手工拷到 `custom_components/`）。

### 6.3 你以为开着 discovery 但其实没开

`--ha-discovery` **默认关闭**。关闭时 `_publish_discovery()` 什么都不发：

```python
def _publish_discovery(self, dev: Device) -> None:
    if not self.ha_discovery:
        dev.discovered = True
        return
```

确认：

```bash
mosquitto_sub -t 'homeassistant/+/espnow2mqtt_+/config' -v
```

有输出 = discovery 开着；没输出 = 关着（正常）。

> **别同时开 `--ha-discovery` 和 HA 集成**，会出现两套实体
> （`sensor.x_temperature` 和 `sensor.x_temperature_2`）。选一个。

### 6.4 开了 discovery 但实体不全

内置 discovery **只支持 7 种能力**：`switch`、`temperature`、`humidity`、
`contact`、`power`、`energy`、`button`。

**灯、窗帘、门锁、风扇、温控器一个都没有。** 组件侧支持它们，
但内置 discovery 没实现。用 HA 集成。

见 [bridge.md §9](bridge.md#9-ha_discovery为什么默认关)。

### 6.5 HA 里有个永远不可用的旧设备

slug 变了（你改了固件里的 `name` 重新烧），老主题上的 retained 消息还在：

```bash
# 找出孤儿
mosquitto_sub -t 'espnow2mqtt/+/state' -v

# 清掉（空 payload + retain = 删除）
mosquitto_pub -t espnow2mqtt/old_name/state -r -n
mosquitto_pub -t espnow2mqtt/old_name/availability -r -n

# 如果开过 discovery，也要清 config 主题
mosquitto_pub -t homeassistant/switch/espnow2mqtt_old_name/config -r -n
```

然后在 HA 里手工删掉那个设备。

想彻底避免这个问题：**把固件里的 `en2m_config_t.name` 留空**，
slug 就恒等于 MAC，永远不变。

### 6.6 实体是灰的（unavailable）

HA 集成把 hub 可用性和设备可用性串联了。两个主题都要看：

```bash
mosquitto_sub -t espnow2mqtt/bridge/state -C 1
mosquitto_sub -t espnow2mqtt/living_room/availability -C 1
```

| `bridge/state` | `<slug>/availability` | 含义 |
|---|---|---|
| `offline` | 任意 | **Bridge 挂了** → [§1](#1-bridge-没连上-mqtt) |
| `online` | `offline` | 设备离线 → [§3](#3-设备不上线) / [§4](#4-设备时上时下) |
| `online` | `online` | MQTT 上是好的，问题在 HA 集成侧 |

> **⚠️ Bridge 挂掉时设备的 `availability` 会停在 `online`**
>
> `bridge/state` 会通过 LWT 变成 `offline`，但每个设备的
> `<slug>/availability` 还是 retained 的 `online`——没人去改它。
> 所以**别只看 `<slug>/availability` 判断设备死活**。

---

## 7. 命令不生效

### 症状

往 `<slug>/set` 发命令，设备没反应。

### 7.1 Bridge 收到了吗

日志里必须有：

```
INFO espnow2mqtt: MQTT espnow2mqtt/living_room/set => {"switch":"ON"}
```

**没有这行** = Bridge 没收到这条 MQTT 消息：

- base topic 不对
- 主题拼错了（必须正好是 `<base>/<slug>/set`）
- MQTT 订阅没建立（看日志有没有 `MQTT connected`）

### 7.2 `unknown device slug`

```
WARNING espnow2mqtt: unknown device slug old_name
```

Bridge 的设备表里没有这个 slug。原因：

| 原因 | 解决 |
|---|---|
| HA 里留着改过名/已移除的旧实体 | 清 retained 主题 + 删 HA 设备（[§6.5](#65-ha-里有个永远不可用的旧设备)） |
| 设备从没上线过 | 先让它上线（[§3](#3-设备不上线)） |
| slug 拼错了 | `mosquitto_sub -t 'espnow2mqtt/+/state' -v` 看真实的 slug |

**临时办法：直接用紧凑 MAC 当 slug。** `_find_by_slug()` 接受这种写法：

```bash
mosquitto_pub -t espnow2mqtt/aabbccddeeff/set -m '{"switch":"ON"}'
```

### 7.3 `failed: send_fail`

```
WARNING espnow2mqtt: command 7 to AA:BB:CC:DD:EE:FF failed: send_fail
```

**S3 根本没把命令发出去。** `en2m_send_downlink()` 同步返回了错误。

| 原因 | 解决 |
|---|---|
| S3 的路由表里没有这个 MAC | 设备从没上线过；或者 `EN2M_ROUTE_STALE_MS = 120000` ms 后路由过期了。等设备下一次心跳（≤ 30 s）路由就会恢复 |
| pending 表满（`EN2M_MAX_PENDING = 4` 条未确认命令） | 慢一点发；或者调大这个值。通常意味着前几条命令都在超时 |

**路由过期这一条值得注意**：一个设备如果 120 秒没上行，S3 会忘掉它的路由，
这时命令会立刻 `send_fail`——即使设备还活着。设备的心跳是 30 秒一次，
所以正常情况下路由不会过期。反复出现 `send_fail` 说明上行丢包严重。

### 7.4 `failed: timeout`

```
WARNING espnow2mqtt: command 7 to AA:BB:CC:DD:EE:FF failed: timeout
```

命令发出去了，S3 重传了 4 次（0/400/800/1200 ms），1.6 秒内没收到 ACK。

| 原因 | 排查 |
|---|---|
| 设备断电了 | 看 `bridge/devices` 里它的 `online`（但离线判定要 90 s，可能还是 true） |
| 信号太差 | RSSI < -85（[§3.4](#34-距离信号)） |
| 中继节点掉了 | `hop > 1` 的设备看它的 `via` |
| 设备侧崩了/卡住了 | 接 `idf.py monitor` 看 |
| **payload 超过 160 字节被截断** | 见下 |

### 7.5 payload 太长被无声截断

S3 侧：

```c
(uint8_t)strnlen(ps ? ps : "{}", EN2M_DATA_MAX)
```

`EN2M_DATA_MAX = 160`。**超过就截断，不做 JSON 合法性检查。**
截断后设备侧 `cJSON_Parse` 失败 → 命令被丢弃 → 不回 ACK →
1.6 秒后你收到 `timeout`。

正常的 HA 命令远小于 160 字节。只有手工发很大的 payload 才会撞上。

### 7.6 ACK 成功但状态没变

日志（开 `-v`）里有 `ack: {... 'ok': True}`，但 `<slug>/state` 里那个字段没变。

**这说明设备收到了命令但拒绝了它。** 设备侧的写回调返回了非 `ESP_OK`，
属性没有提交。原因通常是：

- 值超出范围（`brightness` 给了 300）
- 值类型不对（`{"brightness":"洗衣机"}`）
- 那个属性是只读的
- 设备的 endpoint / cluster 上没有这个属性

**Bridge 对 payload 零校验**，所有校验都在设备侧。要看拒绝原因，
接 `idf.py monitor` 看设备日志，或者在写回调里加日志。

字段和取值的权威表在 [mqtt.md §8](mqtt.md#8-设备状态字段) 和
device 仓库的 `docs/data-model.md`。

### 7.7 命令在重启后自动重放

**你往 `<slug>/set` 发过 retained 消息。** retained 的命令会在
Bridge 每次重连 MQTT 时重新投递，于是"重启就自动开灯"。

```bash
mosquitto_pub -t espnow2mqtt/living_room/set -r -n    # 清掉
```

**规则：状态 retain，命令不 retain。**

### 7.8 一条命令触发了两次写回调

这不是 bug。设备侧的风扇 cluster 会维护 `fan_mode` 和 `percentage` 的一致性：

| 你发的 | 设备侧实际发生的 |
|---|---|
| `{"fan_mode":"off"}` | 写 `FAN_MODE=OFF`，**再**写 `PERCENT_SETTING=0` |
| `{"percentage":0}` | 写 `PERCENT_SETTING=0`，**再**写 `FAN_MODE=OFF` |
| `{"percentage":50}`（原来是 off） | 写 `PERCENT_SETTING=50`，**再**写 `FAN_MODE=ON` |

所以你的写回调可能被调用两次。如果回调里有非幂等的副作用（比如计数、
发 HTTP 请求），要自己去重。

---

## 8. 性能与容量

### 8.1 硬上限

| 限制 | 默认值 | 超了会怎样 | 怎么改 |
|---|---|---|---|
| 设备数（S3 peer 表） | 32 | `coord: peer table full`，新设备的帧被丢 | `CONFIG_EN2M_MAX_ROUTES` |
| 单次上报 payload | 160 B | 设备侧三级降级（丢 `node_role`、丢 `caps`） | `EN2M_DATA_MAX`（改了要两边同步） |
| 下行 payload | 160 B | **无声截断** → 设备解析失败 → timeout | 同上 |
| 未确认命令 | 4 | `send_fail` | `CONFIG_EN2M_MAX_PENDING` |
| 协调器接收队列 | 8 | `rx_dropped=N` | `CONFIG_EN2M_QUEUE_LEN` |
| USB 单行长度 | 1024 B | **整行丢弃，无任何响应** | `usb_host_link.c` 里的 `char line[1024]` |
| mesh 跳数 | 8 | 帧被丢 | `CONFIG_EN2M_HOP_LIMIT` |

### 8.2 日志量

**生产环境别开 `-v`。** 32 个设备在 DEBUG 级别下大约每秒 4 行，
一天 30 万行，systemd 的 journal rate limit 会开始丢日志。

INFO 级别下只打：设备上下线、MQTT 连接、收到的 MQTT 命令、S3 的 `log` 行、
失败的 ACK。这个量很小。

### 8.3 `devices.json` 的写入频率

每一条 `device` 行都会触发一次同步 `write_text()`，也就是每个设备每 30 s 一次。
32 个设备 ≈ 每秒一次。

对 SSD/eMMC 无所谓。**SD 卡的树莓派**要留意（持续小文件写）。
缓解见 [deployment.md §8.2](deployment.md#82-devicesjson-的写入频率)。

### 8.4 USB 写超时会丢行

S3 的 `usb_host_link_write()` 超时是 100 ms（换行符 20 ms）。
**主机不读的时候，S3 的输出会在 100 ms 后超时丢掉，没有重传、没有排队。**

所以"Bridge 重启期间设备的状态上报会丢"是设计内的，靠设备的周期性上报补齐。
不要用 `cat /dev/ttyACM0 > file` 之外的方式长时间占着串口而不消费。

---

## 9. 应急手段

### 9.1 手工和 S3 对话

**先停掉 Bridge**（串口独占）。

```bash
# 终端 1：看
cat /dev/ttyACM0

# 终端 2：发
printf '{"type":"ping"}\n'  > /dev/ttyACM0          # 探活
printf '{"type":"list"}\n'  > /dev/ttyACM0          # 要一遍设备表
printf '{"type":"pair","seconds":120}\n' > /dev/ttyACM0
printf '{"type":"cmd","mac":"AA:BB:CC:DD:EE:FF","id":1,"payload":{"switch":"ON"}}\n' > /dev/ttyACM0
printf '{"type":"unpair","mac":"AA:BB:CC:DD:EE:FF"}\n' > /dev/ttyACM0
```

**用 `printf` 不要用 `echo`**，而且 `\n` 是必需的——没有换行 S3 永远不处理这一行。

全部命令的参考见 [usb-protocol.md](usb-protocol.md)。

### 9.2 从头清一遍 MQTT

```bash
# 看看有哪些 retained 消息
mosquitto_sub -t 'espnow2mqtt/#' -v --retained-only

# 全清（谨慎！会让 HA 里所有实体变 unknown）
mosquitto_sub -t 'espnow2mqtt/#' --retained-only -W 2 -F '%t' \
  | while read t; do mosquitto_pub -t "$t" -r -n; done
```

清完重启 Bridge，它会重发 `bridge/state`/`bridge/info`/`bridge/devices`，
设备状态则要等下一轮上报。

### 9.3 重置设备表

```bash
systemctl stop espnow2mqtt
rm /var/lib/espnow2mqtt/devices.json      # 或 ./data/devices.json
systemctl start espnow2mqtt
```

设备会重新上报 name/model，Bridge 重新学。
**代价**：重新学到之前 slug 会退化成紧凑 MAC，HA 里的实体 ID 会变一轮。

S3 侧的 peer 表在**复位后**自动清空（`s_peers` 是静态数组，`app_main` 里 memset）。

### 9.4 完全重来

```bash
# 1. 停 Bridge
systemctl stop espnow2mqtt

# 2. 重烧协调器
cd firmware/coordinator && idf.py -p /dev/ttyACM0 erase-flash flash

# 3. 清设备表
rm -f /var/lib/espnow2mqtt/devices.json

# 4. 清 MQTT retained（见 §9.2）

# 5. 重烧设备（每块 C3）
cd <device-repo>/examples/relay_switch && idf.py -p /dev/ttyACM1 erase-flash flash

# 6. 起 Bridge，开网，逐个上电
systemctl start espnow2mqtt
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 300
```

`erase-flash` 会清掉设备的 NVS，**包括持久化的属性值和已记住的父节点**
（见 device 仓库 `docs/persistence.md`）。

---

## 10. 日志消息全表

### 10.1 Bridge 自己的日志（logger 名 `espnow2mqtt`）

**INFO 级**

| 消息 | 含义 |
|---|---|
| `loaded N devices from <path>` | 从 `devices.json` 恢复了 N 个设备名 |
| `connecting MQTT <host>:<port>` | 开始连 broker |
| `opening serial <port> @ <baud>` | **这行告诉你它实际打开了哪个串口** |
| `MQTT connected rc=<rc>` | 连上了。`rc=Success` 才算成功 |
| `coordinator hello: {...}` | **收到 S3 的 hello。没有这行 = 串口不通** |
| `coord: <msg>` | S3 的 `log` 行，见 [§10.2](#102-s3-转述的日志coord-前缀) |
| `device online <mac> (<name>) role=<r> hop=<h> via=<v>` | 设备上线 |
| `device offline <mac>` | 设备离线（S3 判的，90 s 无帧） |
| `MQTT <topic> => <payload>` | 收到一条 MQTT 命令 |
| `stopping...` | 收到 SIGINT/SIGTERM |

**WARNING 级**

| 消息 | 含义 | 处理 |
|---|---|---|
| `devices load failed: <exc>` | `devices.json` 损坏/格式不对 | 删掉重新学（[§9.3](#93-重置设备表)） |
| `command <id> to <mac> failed: timeout` | 命令重传 4 次无 ACK | [§7.4](#74-failed-timeout) |
| `command <id> to <mac> failed: send_fail` | S3 没发出去 | [§7.3](#73-failed-send_fail) |
| `command <id> to <mac> failed: unknown` | `ack` 行里 `ok:false` 但没有 `error` 字段 | 不该出现，固件异常 |
| `unknown device slug <slug>` | 往不存在的设备发命令 | [§7.2](#72-unknown-device-slug) |

**ERROR 级**

| 消息 | 含义 | 处理 |
|---|---|---|
| `serial error: <exc>` | 串口读失败。**每秒刷一次 = USB 掉了** | **必须手工重启 Bridge**，它不会自愈 |
| `no serial port found; pass --port /dev/ttyACM0` | 自动探测失败，进程以退出码 **2** 退出 | 显式给 `--port` |

**DEBUG 级（需要 `-v`）**

| 消息 | 含义 |
|---|---|
| `non-json: <line>` | S3 发来的行不是 JSON。**持续刷 = console 配在 USB 上了**（[§2.6](#26-只看到非-json-行)） |
| `ignored: {...}` | 未知的 `type`，协议向前兼容 |
| `pong {...}` | `ping` 的响应 |
| `ack: {...}` | 成功的命令确认 |

### 10.2 S3 转述的日志（`coord:` 前缀）

这些是 S3 通过 `{"type":"log","msg":...}` 发来的，Bridge 以
`INFO espnow2mqtt: coord: <msg>` 打出来。

| `msg` | 含义 | 处理 |
|---|---|---|
| `coordinator ready (esp-idf mesh)` | **S3 初始化完成。这是最重要的一条** | — |
| `mesh init` | `en2m` 组件的 mesh 层起来了 | — |
| `pairing_enabled` | 配网窗口打开 | — |
| `pairing_disabled` | 配网窗口到时自动关闭 | — |
| `peer table full` | 已有 32 个设备，**这一帧被丢弃** | [§3.5](#35-peer-表满了) |
| `rx_dropped=<n>` | 接收队列溢出，累计丢了 n 帧 | [§4.5](#45-rx_dropped-在涨) |
| `bad json` | 主机发来的行不是合法 JSON | 手工调试时打错了 |
| `missing type` | JSON 合法但没有字符串型 `type` | 同上 |
| `unknown cmd` | `type` 不是 5 种已知命令之一 | 同上 |
| `bad mac` | `cmd`/`unpair` 的 `mac` 缺失或格式错 | MAC 要写成 `AA:BB:CC:DD:EE:FF` |

> **`pairing_enabled` 的 `seconds` 字段不会出现在日志里。**
> Bridge 打的是 `msg.get("msg") or msg`，`msg` 字段存在时 `or` 短路了，
> 所以看不到夹过之后的实际秒数。要看就直接 `cat /dev/ttyACM0`。

### 10.3 S3 的 ESP-IDF 日志（不在这条链路上）

`ESP_LOGx` 走 **UART0**，不会出现在 USB 的 NDJSON 流里，
所以 **Bridge 日志里永远看不到 S3 的崩溃回溯**。

要看：接 UART0 跑 `idf.py monitor`，或临时把 console 切回 USB
（[§2.5](#25-s3-固件没跑)）。

S3 固件的 `ESP_LOGx` 标签只有两个：

| 标签 | 来自 |
|---|---|
| `coord` | `main.c`，只有一条 `espnow2mqtt coordinator (ESP-IDF)` |
| `usb_link` | `usb_host_link.c`：`USB Serial/JTAG host link ready`，或 `usb_serial_jtag_driver_install` 失败 |

### 10.4 设备侧的日志（另一个仓库）

C3 设备的 console **在 USB 上**（和协调器相反），`idf.py monitor` 直接能看。

`en2m` 组件有四个标签：`en2m`（mesh）、`en2m_model`、`en2m_dm`、`en2m_event`。
组件只打两条 INFO 日志：`mesh init` 和 `parent=<mac> cost=<n> rssi=<n>`。

完整的日志表和排查流程在 device 仓库的 `docs/troubleshooting.md`。

---

## 相关文档

- [quickstart.md](quickstart.md) — 端到端跑通的正确顺序
- [deployment.md](deployment.md) — 串口权限、by-id 路径、udev、自动重启
- [usb-protocol.md](usb-protocol.md) — 手工和 S3 对话
- [mqtt.md](mqtt.md) — 每个主题的语义和 retain 规则
- [bridge.md](bridge.md) — Bridge 的内部实现，理解上面每条日志的来源
- [coordinator.md](coordinator.md) — S3 固件的内部实现
- device 仓库 `docs/troubleshooting.md` — 设备侧的排查
