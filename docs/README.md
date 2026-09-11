# espnow2mqtt-host 文档

这里是**主机侧**的完整文档：ESP32-S3 协调器固件 + Python Bridge + HA Add-on。

- C3 终端设备固件和 `en2m` 库文档在
  **[espnow2mqtt-device](https://github.com/SFNFIH/espnow2mqtt-device)**
  （那边的 [`docs/`](https://github.com/SFNFIH/espnow2mqtt-device/tree/main/docs)
  是整套系统里最厚的一份，库的架构、状态流转、回调契约都在那）
- HA 集成在 **[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)**

## 这个仓库在整条链路里的位置

```
[ C3 设备 ]  --ESP-NOW-->  [ S3 协调器 ]  --USB NDJSON-->  [ Bridge ]  --MQTT-->  [ HA 集成 ]
   device 仓库                  ↑ 本仓库 firmware/          ↑ 本仓库 espnow2mqtt/      ha 仓库
```

本仓库负责把**空口的二进制帧**变成**MQTT 上的 JSON**，两头各有一段协议：

| 段 | 协议 | 文档 |
|---|---|---|
| C3 ↔ S3 | ESP-NOW，`en2m_pkt_t` 二进制帧 | [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md)，以及 device 仓库的 `docs/mesh.md` |
| S3 ↔ 主机 | USB Serial/JTAG 上的 NDJSON（一行一个 JSON） | [usb-protocol.md](usb-protocol.md) |
| Bridge ↔ HA | MQTT，主题 + JSON | [mqtt.md](mqtt.md) |

## 按需求找文档

| 我想…… | 看这篇 |
|---|---|
| 先跑起来 | [quickstart.md](quickstart.md) |
| 搞懂整条链路每一跳做了什么 | [architecture.md](architecture.md) |
| 搞懂 S3 固件（peer 表、配网、ACK、离线判定） | [coordinator.md](coordinator.md) |
| 搞懂 Bridge（状态合并、caps、discovery、slug） | [bridge.md](bridge.md) |
| 查 USB 上的每一种 JSON 行 | [usb-protocol.md](usb-protocol.md) |
| 查 MQTT 主题和 payload | [mqtt.md](mqtt.md) |
| 用 HA Add-on / Docker 部署 | [deployment.md](deployment.md) |
| 设备不上线 / 命令不生效 / HA 里没实体 | [troubleshooting.md](troubleshooting.md) |
| 看空中协议 | [../protocol/PROTOCOL.md](../protocol/PROTOCOL.md) |
| 看 `en2m` 组件的 API | [../components/en2m/README.md](../components/en2m/README.md) |

## 建议的阅读路线

**只想部署**

1. [quickstart.md](quickstart.md)
2. [deployment.md](deployment.md) 选一种跑法（裸机 / Docker / HA Add-on）
3. 卡住了查 [troubleshooting.md](troubleshooting.md)

**想改代码 / 想搞懂内部**

1. [architecture.md](architecture.md) 先看清整条链路的分工
2. [usb-protocol.md](usb-protocol.md) 和 [mqtt.md](mqtt.md) 两头的协议
3. [coordinator.md](coordinator.md) → [bridge.md](bridge.md) 逐层深入

## 关于 `components/en2m`

本仓库的 `components/en2m` 和 device 仓库里的**逐字节相同**，是同一份代码的两份
vendored 拷贝。协调器只用它的**传输层**（`en2m_mesh_init` + `on_uplink`），
不建任何 cluster，所以数据模型那一半在协调器里是不参与的。

改了组件记得同步另一个仓库，否则两边会跑在不同的协议实现上。
组件本身的文档在 device 仓库的 `docs/`。

## 版本

`EN2M_FW_VERSION = "0.4.0-idf"`，空中协议版本 `EN2M_VERSION = 2`，
基于 ESP-IDF v5.5.5 验证。Bridge 需要 Python 3.10+（用到 `X | None` 语法）。
