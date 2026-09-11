# ESP-NOW 2 MQTT — 主机（ESP32-S3）

**本仓库 = S3 主机侧整套东西：**

1. **`firmware/coordinator`** — ESP32-S3 协调器固件（USB Serial/JTAG ↔ ESP-NOW mesh）
2. **`espnow2mqtt`（Python）** — 跑在电脑 / HA 主机上的 USB ↔ MQTT Bridge
3. **`addon/`** — Home Assistant Add-on（可选）

C3 终端设备固件在：**[espnow2mqtt-device](https://github.com/SFNFIH/espnow2mqtt-device)**  
HA 集成（插件）在：**[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)**  
总览：**[espnow2mqtt](https://github.com/SFNFIH/espnow2mqtt)**

```
[ C3 设备 ] --ESP-NOW--> [ S3 协调器 ] --USB--> [ Bridge ] --MQTT--> [ HA 插件 ]
                 ↑ 本仓库固件              ↑ 本仓库 Python
```

---

## 文档

完整文档在 **[`docs/`](docs/)**。按需求找：

| 我想…… | 看这篇 |
|------|------|
| 从零跑通主机侧（烧 S3 → 跑 Bridge → 验证 MQTT → 加第一个设备） | [docs/quickstart.md](docs/quickstart.md) |
| 搞懂整条链路每一跳做了什么、状态和命令怎么流动 | [docs/architecture.md](docs/architecture.md) |
| 搞懂 S3 固件（peer 表、配网、ACK/重传、离线判定） | [docs/coordinator.md](docs/coordinator.md) |
| 搞懂 Bridge（状态合并、caps 推断、slug、discovery） | [docs/bridge.md](docs/bridge.md) |
| 查 USB 上的每一种 JSON 行（含手工调试命令） | [docs/usb-protocol.md](docs/usb-protocol.md) |
| 查 MQTT 主题、payload 字段、retain 语义 | [docs/mqtt.md](docs/mqtt.md) |
| 用 HA Add-on / Docker / systemd 部署 | [docs/deployment.md](docs/deployment.md) |
| 设备不上线 / 命令不生效 / HA 里没实体 | [docs/troubleshooting.md](docs/troubleshooting.md) |
| 看空中协议 | [protocol/PROTOCOL.md](protocol/PROTOCOL.md) |
| 看 `en2m` 组件的 API | [espnow2mqtt-device `docs/`](https://github.com/SFNFIH/espnow2mqtt-device/tree/main/docs) |

文档索引和推荐阅读路线：**[docs/README.md](docs/README.md)**

---

## 1. 烧录 ESP32-S3 协调器

需要 [ESP-IDF 5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)。

```bash
cd firmware/coordinator
idf.py set-target esp32s3
idf.py build flash monitor
```

- 组件路径：`components/en2m`（mesh 传输；主机不做外设 cluster）
- 插上 USB 后，主机应出现串口（常见 `/dev/ttyACM0` 或 `by-id`）
- **S3 不连家庭 Wi‑Fi**，只固定信道跑 ESP-NOW + USB

---

## 2. 运行 Bridge（USB ↔ MQTT）

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

python -m espnow2mqtt \
  --port /dev/ttyACM0 \
  --mqtt-host homeassistant.local \
  --mqtt-port 1883 \
  -v
```

### 常用参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--port` | 自动 | S3 串口 |
| `--mqtt-host` | `localhost` | Broker |
| `--mqtt-port` | `1883` | 端口 |
| `--mqtt-user` / `--mqtt-pass` | 空 | 认证 |
| `--base-topic` | `espnow2mqtt` | 须与 HA 插件一致 |
| `--ha-discovery` | **关** | 推荐关，用 HA 插件建实体 |
| `-v` | off | 详细日志 |

### Docker

```bash
docker compose up -d --build
```

按需改 `docker-compose.yml` 里的串口映射和 `--mqtt-host`。

### Home Assistant Add-on

见 [`addon/`](addon/)。实体仍用 **espnow2mqtt-ha**，不要依赖 MQTT Discovery。

---

## MQTT 主题（默认 base=`espnow2mqtt`）

| 主题 | 说明 |
|------|------|
| `espnow2mqtt/bridge/state` | Bridge online/offline |
| `espnow2mqtt/bridge/info` | 协调器自述（信道、固件、MAC） |
| `espnow2mqtt/bridge/devices` | 设备列表 |
| `espnow2mqtt/bridge/request/permit_join` | 开网（秒） |
| `espnow2mqtt/<slug>/state` | 设备状态 |
| `espnow2mqtt/<slug>/set` | 设备控制 |
| `espnow2mqtt/<slug>/availability` | 设备在线 |
| `espnow2mqtt/<slug>/command_result` | 一条命令的结局（成功 / 超时 / 发送失败） |

开网：

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60
```

---

## 仓库结构

```
components/en2m/           # 协调器依赖的 mesh 组件
firmware/coordinator/      # ESP32-S3 固件工程
espnow2mqtt/               # Python Bridge 包
addon/                     # HA Add-on
docker-compose.yml
protocol/PROTOCOL.md       # USB NDJSON / 空中协议摘要
```

---

## 和另外两个仓库怎么配合

| 步骤 | 仓库 |
|------|------|
| 烧 S3、跑 Bridge | **本仓库（host）** |
| 烧 C3 开关/灯/传感器… | [espnow2mqtt-device](https://github.com/SFNFIH/espnow2mqtt-device) |
| HA 里自动出实体 | [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha) |

---

## License

MIT
