# ESP-NOW 2 MQTT — Bridge

主机侧 **USB Serial ↔ MQTT** 桥接程序。  
对接插在电脑 / HA 主机上的 **ESP32-S3 协调器**（USB Serial/JTAG），把 ESP-NOW 子设备的状态转到 MQTT，并把控制命令下发回协调器。

风格接近 Zigbee2MQTT 的 “协调器 + bridge + 主题约定”，但无线侧是 ESP-NOW。

配套：

- 固件：https://github.com/SFNFIH/espnow2mqtt-firmware  
- HA 集成：https://github.com/SFNFIH/espnow2mqtt-ha  
- 总览：https://github.com/SFNFIH/espnow2mqtt  

---

## 做什么

```
ESP32-S3 Coordinator  ←USB NDJSON→  Bridge (本程序)  ←MQTT→  Home Assistant / 其它客户端
         ↑
    ESP-NOW mesh
         ↑
   Leaf / Router 设备
```

Bridge 负责：

- 打开串口，读写协调器 NDJSON
- 维护设备表（`data/devices.json`）
- 发布 `bridge/state`、`bridge/devices`、`<slug>/state`、`availability`
- 订阅 `<slug>/set`、`bridge/request/permit_join` 等并转发
- （可选）HA MQTT Discovery —— **默认关闭**，推荐用自定义集成建实体

---

## 快速运行（本机 Python）

需要：Python 3.11+、协调器已插上并出现串口。

```bash
python3 -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r requirements.txt

python -m espnow2mqtt \
  --port /dev/ttyACM0 \
  --mqtt-host homeassistant.local \
  --mqtt-port 1883 \
  -v
```

串口可留空让程序尝试自动检测：

```bash
python -m espnow2mqtt --mqtt-host 192.168.1.10 -v
```

有 MQTT 账号时：

```bash
python -m espnow2mqtt \
  --port /dev/ttyACM0 \
  --mqtt-host homeassistant.local \
  --mqtt-user mqttuser \
  --mqtt-pass secret
```

---

## 命令行参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--port` | 空（自动） | 串口路径，如 `/dev/ttyACM0`、`COM3` |
| `--baud` | `115200` | 波特率（需与协调器一致） |
| `--mqtt-host` | `localhost` | Broker 地址 |
| `--mqtt-port` | `1883` | Broker 端口 |
| `--mqtt-user` / `--mqtt-pass` | 空 | 认证 |
| `--base-topic` | `espnow2mqtt` | 主题前缀，需与 HA 集成一致 |
| `--discovery-prefix` | `homeassistant` | HA Discovery 前缀（仅开启 Discovery 时有用） |
| `--ha-discovery` | **关闭** | 打开则写 HA MQTT Discovery |
| `--devices` | `data/devices.json` | 设备持久化文件 |
| `-v` / `--verbose` | off | 调试日志 |

> 推荐保持 **不** 开 `--ha-discovery`，用 [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha) 管理实体，避免两套发现机制打架。

---

## Docker Compose

仓库自带 `docker-compose.yml` 示例：

```bash
# 按需改 devices 映射与 mqtt-host
docker compose up -d --build
```

要点：

- 把主机串口映射进容器（如 `/dev/ttyACM0:/dev/ttyACM0`）
- `--mqtt-host` 指向能访问的 broker 主机名（Compose 网络内可能是 `homeassistant` / `core-mosquitto`）
- `./data` 挂载用于保存 `devices.json`

Linux 上若权限不足，确认用户在 `dialout` 组，或用 by-id 路径：

```text
/dev/serial/by-id/usb-Espressif_...
```

---

## Home Assistant Add-on

见 [`addon/`](addon/)：

- `config.yaml` — Add-on 元数据与选项
- `Dockerfile` / `run.sh` — 容器启动

在 HA OS / Supervised 下把本仓库当作本地 Add-on 仓库或按文档拷贝到 `addons/`。  
Add-on 选项里配置串口与 MQTT；同样建议关闭 Discovery，改用自定义集成。

---

## MQTT 主题（默认 base=`espnow2mqtt`）

| 主题 | 方向 | 说明 |
|------|------|------|
| `espnow2mqtt/bridge/state` | 发布 | `online` / `offline`（LWT + retain） |
| `espnow2mqtt/bridge/devices` | 发布 | 已知设备 JSON 数组 |
| `espnow2mqtt/bridge/info` | 发布 | Bridge 信息（如有） |
| `espnow2mqtt/bridge/request/permit_join` | 订阅 | 载荷为秒数，例如 `60` |
| `espnow2mqtt/<slug>/state` | 发布 | 设备状态 JSON（来自固件上报） |
| `espnow2mqtt/<slug>/set` | 订阅 | 控制 JSON，转发到协调器 |
| `espnow2mqtt/<slug>/availability` | 发布 | `online` / `offline` |

开网示例：

```bash
mosquitto_pub -h localhost -t espnow2mqtt/bridge/request/permit_join -m 60
```

控制示例：

```bash
mosquitto_pub -t espnow2mqtt/relay1/set -m '{"switch":"ON"}'
mosquitto_pub -t espnow2mqtt/light1/set -m '{"switch":"ON","brightness":200}'
```

---

## 与固件 / HA 的协作

1. 烧录 **coordinator** 固件到 S3，插入主机  
2. 启动 **本 Bridge**，确认 `bridge/state=online`  
3. 安装 **espnow2mqtt-ha**，base topic 一致  
4. `permit_join` 后烧录 / 复位 leaf 设备  
5. HA 根据 state 里的 `caps` 自动建实体  

串口协议与空中包说明见固件仓库 [`protocol/PROTOCOL.md`](https://github.com/SFNFIH/espnow2mqtt-firmware/blob/main/protocol/PROTOCOL.md)。

---

## 开发

```bash
pip install -r requirements.txt
python -m espnow2mqtt -v --port /dev/ttyACM0 --mqtt-host localhost
# 可选：pytest（见 tests/）
```

包入口：`python -m espnow2mqtt`（`espnow2mqtt/__main__.py`）。

---

## 故障排查

| 现象 | 检查 |
|------|------|
| 打不开串口 | 路径、权限、`dialout`、是否被其它进程占用 |
| Bridge offline | MQTT 地址/账号、防火墙、LWT 主题 |
| 收不到设备 | 协调器固件、USB 线、permit_join、信道是否一致 |
| 能上报不能控制 | `…/set` 主题、slug 是否匹配、固件是否实现 set 驱动 |

打开 `-v` 可看到串口收发与 MQTT 细节。

---

## License

MIT
