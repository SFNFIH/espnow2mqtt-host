# 主机快速开始（S3 协调器 + Bridge）

这一篇是**主机侧**的最短路径：烧协调器 → 跑 Bridge → 验证 MQTT。
跑通之后再去烧 C3 设备和装 HA 集成。

跨三个仓库的完整流程，看 device 仓库的
[`docs/quickstart.md`](https://github.com/SFNFIH/espnow2mqtt-device/blob/main/docs/quickstart.md)。

**你需要：**

| 东西 | 要求 |
|---|---|
| ESP32-S3 开发板 | **必须有芯片原生 USB**（USB Serial/JTAG），不能是外置 CP210x/CH340 的板子 |
| ESP-IDF | **v5.5.5**（其他 5.x 大概也行，但只在 5.5.5 上验证过） |
| MQTT broker | Mosquitto 之类，能从跑 Bridge 的机器访问 |
| Python | **3.10+**（代码用了 `X \| None` 语法） |

---

## 0. 板子要对

协调器固件用 `usb_serial_jtag` 驱动，**需要 S3 芯片的原生 USB**。

很多 DevKit 有两个 Type-C 口：

| 口 | 丝印 | 能用？ |
|---|---|---|
| 原生 USB | `USB` | **✓ 用这个** |
| 外置 UART 芯片 | `UART` / `COM` | ✗ 只能烧固件，跑不了协调器协议 |

插上之后确认：

```bash
udevadm info -n /dev/ttyACM0 | grep -E 'ID_VENDOR_ID|ID_MODEL_ID'
```

| VID:PID | 结论 |
|---|---|
| `303a:1001` | **对了**，这是 Espressif USB JTAG/serial debug unit |
| `10c4:ea60` / `1a86:7523` / `0403:6001` | 外置 UART 芯片，换口或换板子 |

---

## 1. 烧协调器固件

```bash
. ~/esp/esp-idf-v5.5.5/export.sh      # 激活 ESP-IDF

cd firmware/coordinator
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash
```

期望 `idf.py build` 结尾看到类似：

```
Project build complete.
espnow2mqtt-coordinator.bin binary size 0x5160 bytes
```

### 顺手确认信道

ESP-NOW 跑在**固定信道**上，协调器和设备必须一致。默认是 1。

```bash
grep EN2M_WIFI_CHANNEL sdkconfig
# CONFIG_EN2M_WIFI_CHANNEL=1
```

要改就 `idf.py menuconfig` → **Component config → en2m → Wi-Fi channel used by ESP-NOW**。
**记住这个值，烧 C3 设备时要配成一样的。**

### 协调器的 console 在 UART0

`sdkconfig.defaults` 里：

```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n
```

这是故意的：USB 上跑的是 NDJSON 协议，日志不能往里掺。
所以**`idf.py monitor` 接 USB 口什么都看不到**——要看 ESP-IDF 日志得接 UART0。

正常情况下你不需要看它，Bridge 会把 S3 的关键信息通过 `{"type":"log"}` 转述出来。

---

## 2. 找到稳定的串口路径

```bash
ls -l /dev/serial/by-id/
```

```
usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00 -> ../../ttyACM0
```

**记下这个 `by-id` 路径，后面都用它。**

`/dev/ttyACM0` 的编号取决于枚举顺序，多插一个 USB 串口设备或者重启就可能变。
`by-id` 路径包含 S3 的 MAC，永远不变。

### 权限

```bash
ls -l /dev/ttyACM0
# crw-rw---- 1 root dialout ...
sudo usermod -aG dialout $USER     # 然后重新登录
```

---

## 3. 手工验证 S3 在说话

在装 Bridge 之前，先用 30 秒确认固件真的跑起来了。

```bash
# 终端 1
cat /dev/ttyACM0

# 终端 2
printf '{"type":"ping"}\n' > /dev/ttyACM0
```

期望在终端 1 看到（顺序可能不同）：

```json
{"type":"hello","version":2,"role":"coordinator","mac":"7C:DF:A1:00:11:22","fw":"0.4.0-idf","channel":1,"mesh":true,"stack":"esp-idf"}
{"type":"log","msg":"mesh init"}
{"type":"log","msg":"coordinator ready (esp-idf mesh)"}
{"type":"pong","ms":1234}
```

**`hello` + `coordinator ready` 两条都有 = 协调器完全正常。**
`hello` 每 30 秒会自己重复一次，所以等一会儿也能看到。

什么都没有的话，**别继续往下走**——先解决它，见
[troubleshooting.md §2](troubleshooting.md#2-bridge-收不到-s3串口问题)。

> 记得 Ctrl+C 关掉 `cat`。**串口是独占的**，占着的话 Bridge 起不来。

---

## 4. 跑 Bridge

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

python -m espnow2mqtt \
  --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00 \
  --mqtt-host 192.168.1.10 \
  --mqtt-port 1883 \
  -v
```

依赖只有两个：`paho-mqtt>=2.0.0` 和 `pyserial>=3.5`。

### 期望的输出

```
INFO espnow2mqtt: connecting MQTT 192.168.1.10:1883
INFO espnow2mqtt: opening serial /dev/serial/by-id/... @ 115200
INFO espnow2mqtt: MQTT connected rc=Success
INFO espnow2mqtt: coordinator hello: {'type': 'hello', 'version': 2, ...}
INFO espnow2mqtt: coord: coordinator ready (esp-idf mesh)
```

**这四行都要有。** 缺了哪行对应什么问题：

| 缺的行 | 问题 |
|---|---|
| `MQTT connected` | broker 地址/认证错，或 broker 没跑 |
| `coordinator hello` | **串口选错了，或 S3 没跑**（第 3 步应该已经排除了） |
| `coord: coordinator ready` | 收到 hello 但没 ready → 固件版本不对 |

### 关于 `-v`

**第一次跑一定要开 `-v`。** 它会额外显示：

- 非 JSON 行（判断 console 有没有配错）
- `pong` 和成功的 `ack`

生产环境**关掉它**——32 个设备在 DEBUG 级别下一天能产生 30 万行日志。

### 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port` | 自动探测 | **生产环境必须显式指定** |
| `--mqtt-host` / `--mqtt-port` | `localhost` / `1883` | broker |
| `--mqtt-user` / `--mqtt-pass` | 空（匿名） | 认证 |
| `--base-topic` | `espnow2mqtt` | **必须和 HA 集成里配的一致** |
| `--ha-discovery` | **关** | 保持关闭，用 HA 集成建实体 |
| `--devices` | `data/devices.json` | 设备名缓存 |

全部参数见 [bridge.md §12](bridge.md#12-命令行参数全表)。

---

## 5. 验证 MQTT

另开一个终端：

```bash
mosquitto_sub -t 'espnow2mqtt/bridge/#' -v
```

期望（都是 retained 消息，订上来立刻就有）：

```
espnow2mqtt/bridge/state online
espnow2mqtt/bridge/info {"type":"hello","version":2,"role":"coordinator",...}
espnow2mqtt/bridge/devices []
```

**`bridge/info` 有内容是关键。** `bridge/state = online` 只说明 Bridge 活着，
`bridge/info` 有内容才说明它真的和 S3 说过话。

顺手把信道抄下来：

```bash
mosquitto_sub -t espnow2mqtt/bridge/info -C 1 \
  | python3 -c 'import json,sys; print("channel =", json.load(sys.stdin)["channel"])'
```

**烧 C3 设备时 `CONFIG_EN2M_WIFI_CHANNEL` 必须是这个值。**

---

## 6. 加第一个设备

### 6.1 开配网窗口

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 120
```

Bridge 日志里应该立刻出现：

```
INFO espnow2mqtt: MQTT espnow2mqtt/bridge/request/permit_join => 120
INFO espnow2mqtt: coord: pairing_enabled
```

窗口最长 300 秒（S3 会夹 `[1,300]`），到时自动关闭并打
`coord: pairing_disabled`。

> **别加 `-r`。** retained 的 `permit_join` 会在 Bridge 每次重连时重放，
> 配网窗口就永远开着。

### 6.2 烧一个 C3 设备并上电

去 [espnow2mqtt-device](https://github.com/SFNFIH/espnow2mqtt-device)，
挑一个例程（比如 `examples/relay_switch`），
**把信道配成和第 5 步查到的一样**，然后烧进 C3 并上电。

### 6.3 期望看到

Bridge 日志：

```
INFO espnow2mqtt: device online AA:BB:CC:DD:EE:FF (relay1) role=leaf hop=1 via=7C:DF:A1:00:11:22
```

MQTT：

```bash
mosquitto_sub -t 'espnow2mqtt/+/state' -v
```

```
espnow2mqtt/relay1/state {"node_role":"leaf","switch":"OFF","caps":["switch"],"hop":1,"via":"7C:DF:A1:00:11:22"}
```

叶子节点默认 30 秒一条上报，但**入网后会有一次快速上报**，所以
正常情况下几百毫秒内就该看到第一条状态。

设备一直不上线，最常见的原因是**信道不匹配**，见
[troubleshooting.md §3.1](troubleshooting.md#31-信道不匹配最常见)。

---

## 7. 发第一条命令

```bash
mosquitto_pub -t espnow2mqtt/relay1/set -m '{"switch":"ON"}'
```

Bridge 日志：

```
INFO espnow2mqtt: MQTT espnow2mqtt/relay1/set => {"switch":"ON"}
DEBUG espnow2mqtt: ack: {'type': 'ack', 'mac': 'AA:BB:CC:DD:EE:FF', 'id': 1, 'ok': True, ...}
```

（`ack` 那行需要 `-v`。）

然后 `<slug>/state` 里 `switch` 变成 `ON`：

```bash
mosquitto_sub -t espnow2mqtt/relay1/state -C 1 | python3 -m json.tool
```

**没有 `command 1 to ... failed` 警告**就说明命令链路通了。

裸值也行（Bridge 会包成 `{"switch":"ON"}`）：

```bash
mosquitto_pub -t espnow2mqtt/relay1/set -m ON
mosquitto_pub -t espnow2mqtt/relay1/set -m OFF
```

> **别加 `-r`。** retained 的命令会在每次重连时重放 → "重启就自动开灯"。
> **规则：状态 retain，命令不 retain。**

---

## 8. 接下来

到这里主机侧已经完全跑通了。三个方向：

**① 装 HA 集成，让实体自动出来**

装 [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)，
配置时 base topic 填和 Bridge 一样的值（默认 `espnow2mqtt`）。

**别开 `--ha-discovery`**——内置 discovery 只支持 7 种简单能力，
灯/窗帘/门锁/风扇/温控器一个都没有。见
[bridge.md §9](bridge.md#9-ha_discovery为什么默认关)。

**② 换成常驻部署**

现在是前台手动跑的。选一种长期跑法：

| 环境 | 看这节 |
|---|---|
| HA OS / HA Supervised | [deployment.md §1](deployment.md#1-home-assistant-add-on) |
| NAS / 独立 Linux 主机 | [deployment.md §2](deployment.md#2-docker-compose) |
| 树莓派裸机 | [deployment.md §3](deployment.md#3-systemd裸机) |

三种都要解决同一件事：**USB 断开后 Bridge 不会自愈**，必须有外部重启机制。

**③ 搞懂内部实现**

| 我想…… | 看这篇 |
|---|---|
| 搞懂整条链路每一跳做了什么 | [architecture.md](architecture.md) |
| 搞懂 S3 固件 | [coordinator.md](coordinator.md) |
| 搞懂 Bridge | [bridge.md](bridge.md) |
| 查 USB 上的每一种 JSON 行 | [usb-protocol.md](usb-protocol.md) |
| 查 MQTT 主题和 payload | [mqtt.md](mqtt.md) |
| 出问题了 | [troubleshooting.md](troubleshooting.md) |

---

## 常见卡点速查

| 症状 | 最可能的原因 | 跳到 |
|---|---|---|
| 手工 `ping` 没有 `pong` | 插的是 UART 口不是原生 USB 口 | [§0](#0-板子要对) |
| 日志没有 `coordinator hello` | 串口选错了 | [troubleshooting.md §2](troubleshooting.md#2-bridge-收不到-s3串口问题) |
| `mosquitto_sub` 什么都订不到 | base topic 不一致 | [troubleshooting.md §1.3](troubleshooting.md#1-bridge-没连上-mqtt) |
| 设备永远不上线 | **信道不匹配** | [troubleshooting.md §3.1](troubleshooting.md#31-信道不匹配最常见) |
| `unknown device slug` | 设备名变了留下的孤儿实体 | [troubleshooting.md §7.2](troubleshooting.md#72-unknown-device-slug) |
| HA 里没有实体 | base topic 不一致，或集成没装 | [troubleshooting.md §6](troubleshooting.md#6-ha-里没有实体) |
| `PermissionError` 打不开串口 | 用户不在 `dialout` 组 | [§2](#2-找到稳定的串口路径) |
| `could not open port` | 串口被 `cat` / `idf.py monitor` 占着 | [troubleshooting.md §2.2](troubleshooting.md#22-串口被占用) |
