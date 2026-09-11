# 部署指南

Bridge 有四种跑法。先看这张表选一个：

| 跑法 | 适合 | 自动重启 | 串口权限 | 数据持久化 |
|---|---|:-:|---|---|
| [HA Add-on](#1-home-assistant-add-on) | **HA OS / HA Supervised**（推荐） | ✓ supervisor | 自动（`usb: true`） | `/data`（HA 备份会带上） |
| [Docker Compose](#2-docker-compose) | 群晖 / NAS / 独立 Linux 主机 | ✓ `restart: unless-stopped` | `devices:` 映射 | `./data` 卷 |
| [systemd](#3-systemd裸机) | 树莓派 / 任意 Linux，不想用容器 | ✓ `Restart=always` | 用户加 `dialout` 组 | 自己指定路径 |
| [直接跑](#4-直接跑调试用) | 调试 | ✗ | 同上 | 当前目录 |

不管哪种，都有**三件事必须对**：

1. **串口路径** —— 用 `/dev/serial/by-id/...`，别用 `/dev/ttyACM0`（[§5](#5-串口路径与权限)）
2. **`--base-topic` 和 HA 集成里配的一致** —— 默认都是 `espnow2mqtt`
3. **进程必须能自动重启** —— USB 断开后 Bridge 不会自愈（[bridge.md §13.3](bridge.md#133-串口断开不会自愈)）

在部署之前，先确认 S3 协调器已经烧好了（[quickstart.md](quickstart.md) 第 1 步）。

---

## 1. Home Assistant Add-on

### 1.1 安装

Add-on 没有发布到官方仓库，得手工添加：

**Settings → Add-ons → Add-on Store → ⋮ → Repositories**，加：

```
https://github.com/SFNFIH/espnow2mqtt-host
```

然后在列表里找到 **ESP-NOW 2 MQTT** 安装。

> 如果 Store 里找不到，说明这个仓库缺 `repository.yaml`（本仓库把 add-on 放在
> `addon/` 子目录，HA 的 add-on 仓库要求每个 add-on 一个顶层目录 + 一个
> `repository.yaml`）。这种情况下用下面的"本地安装"。

**本地安装**（HA OS 上通过 SSH / Samba）：

```bash
cd /addons
git clone https://github.com/SFNFIH/espnow2mqtt-host.git espnow2mqtt-host
# add-on 的构建上下文是仓库根目录，addon/Dockerfile 里的 COPY 路径依赖这一点
```

然后 **Add-on Store → ⋮ → Check for updates**，本地 add-on 会出现在
"Local add-ons" 分组下。

### 1.2 配置

`addon/config.yaml` 暴露了 8 个选项：

| 选项 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `port` | str | `/dev/ttyACM0` | **改成 `by-id` 路径**，见 [§5](#5-串口路径与权限) |
| `baud` | int | `115200` | USB CDC 上无意义，留默认 |
| `base_topic` | str | `espnow2mqtt` | 必须和 HA 集成一致 |
| `mqtt_host` | str | `""` | **留空 = 自动发现 HA 的 Mosquitto**（推荐） |
| `mqtt_port` | port | `1883` | 留空 `mqtt_host` 时会被自动发现覆盖 |
| `mqtt_user` | str | `""` | 同上 |
| `mqtt_password` | password | `""` | 同上 |
| `ha_discovery` | bool | `false` | **保持 false**，用 espnow2mqtt-ha 集成 |

推荐配置：

```yaml
port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00
baud: 115200
base_topic: espnow2mqtt
mqtt_host: ""
mqtt_port: 1883
mqtt_user: ""
mqtt_password: ""
ha_discovery: false
```

### 1.3 MQTT 自动发现怎么工作

`addon/run.sh` 里：

```bash
if [[ -z "$HOST" ]]; then
  if bashio::services.available mqtt; then
    HOST=$(bashio::services mqtt host)
    MPORT=$(bashio::services mqtt port)
    USER=$(bashio::services mqtt username)
    PASS=$(bashio::services mqtt password)
    bashio::log.info "Using Home Assistant MQTT service at ${HOST}:${MPORT}"
  else
    HOST="core-mosquitto"
    bashio::log.warning "MQTT service not discovered; falling back to ${HOST}"
  fi
fi
```

`mqtt_host` 留空时，Add-on 向 supervisor 要 MQTT 服务的连接信息
（`config.yaml` 里声明了 `services: [mqtt:need]`）。**这是推荐做法**：
不用手抄用户名密码，Mosquitto add-on 换密码也不用改配置。

服务发现失败时退到 `core-mosquitto`（Mosquitto add-on 的容器名），
**但用户名密码是空的**——如果你的 Mosquitto 禁了匿名访问，这时会连不上。
日志里会有那条 `MQTT service not discovered` 警告。

三种情况的判断：

| 日志 | 含义 |
|---|---|
| `Using Home Assistant MQTT service at core-mosquitto:1883` | 一切正常 |
| `MQTT service not discovered; falling back to core-mosquitto` | Mosquitto add-on 没装 / 没跑 / MQTT 集成没配 |
| 手填了 `mqtt_host` | 完全不走自动发现，用你填的 |

### 1.4 devices.json 的位置

`run.sh` 固定传 `--devices /data/devices.json`。`/data` 是 add-on 的持久卷，
**会被 HA 的备份带上**，所以设备名在 add-on 重装/HA 迁移后还在。

想看/改它：

```bash
# HA OS 上，通过 SSH & Web Terminal add-on
cat /mnt/data/supervisor/addons/data/<slug>_espnow2mqtt/devices.json
```

`<slug>` 是 add-on 的安装标识（本地安装是 `local`，仓库安装是仓库的 hash）。
更简单的办法是从 add-on 的 **Log** 标签看它启动时打的
`loaded N devices from /data/devices.json`。

### 1.5 config.yaml 里几个关键字段的意义

```yaml
uart: true          # 允许访问 /dev/tty* 和 /dev/serial/by-id
usb: true           # 允许访问 USB 设备
init: false         # 用 s6-overlay 的 init，run.sh 里的 exec 才是 PID 1
startup: application
boot: auto          # HA 启动时自动起
services:
  - mqtt:need       # 声明依赖 MQTT 服务（供自动发现）
map:
  - share:rw        # 挂 /share，本 add-on 其实没用到
arch: [amd64, aarch64, armv7]
```

`uart: true` + `usb: true` 两个都要——前者给 `/dev/tty*`，后者给底层 USB 节点。
少一个就会 `PermissionError` 或者 `by-id` 路径不存在。

### 1.6 自动重启

`startup: application` + `boot: auto` 让 supervisor 在 HA 启动时拉起它。
**进程崩了 supervisor 也会重启**（s6-overlay 的默认行为）。
这就是"USB 断开后 Bridge 不自愈"的兜底——USB 掉了 Bridge 会开始刷
`serial error:` 但不会退出，所以**这个兜底在这种情况下不生效**。
USB 真的掉了要手工重启 add-on。

---

## 2. Docker Compose

仓库根目录有现成的 `docker-compose.yml`：

```yaml
services:
  espnow2mqtt:
    build:
      context: .
      dockerfile: Dockerfile
    container_name: espnow2mqtt-host
    restart: unless-stopped
    devices:
      - /dev/ttyACM0:/dev/ttyACM0
    environment:
      PYTHONUNBUFFERED: "1"
    command:
      - python
      - -m
      - espnow2mqtt
      - --port
      - /dev/ttyACM0
      - --mqtt-host
      - homeassistant
      - --mqtt-port
      - "1883"
    volumes:
      - ./data:/app/data
```

**直接用之前必须改两处：**

### 2.1 串口映射改成 by-id

```yaml
    devices:
      - /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00:/dev/ttyACM0
```

左边是宿主机上的稳定路径，右边是容器里的固定名字。这样容器里的
`--port /dev/ttyACM0` 不用改，而宿主机重启后设备号变了也不影响。

> Docker 的 `devices:` 映射**会解析符号链接**，所以 `by-id` 路径在宿主机上
> 必须真实存在（`ls -l /dev/serial/by-id/` 能看到）才能启动容器。
> S3 没插上就 `docker compose up` 会直接失败——这其实是好事，早失败早发现。

### 2.2 MQTT 地址

```yaml
      - --mqtt-host
      - 192.168.1.10          # 你的 broker
      - --mqtt-user
      - mqttuser
      - --mqtt-pass
      - mqttpass
```

`homeassistant` 这个默认值只在容器和 HA 在同一个 Docker 网络里才解析得出来。

> **⚠️ 密码写在 compose 文件里不安全。** 更好的做法是用 `env_file`
> 加一层，或者在 broker 上给 Bridge 建一个只能读写
> `espnow2mqtt/#` 的受限账号。Bridge 本身**不支持从环境变量读 MQTT 密码**
> （所有配置都走命令行参数），要改得动代码。

### 2.3 跑起来

```bash
mkdir -p data                    # devices.json 会写在这里
docker compose up -d --build
docker compose logs -f
```

期望看到：

```
espnow2mqtt-host  | 2026-09-11 12:00:00 INFO espnow2mqtt: connecting MQTT 192.168.1.10:1883
espnow2mqtt-host  | 2026-09-11 12:00:00 INFO espnow2mqtt: opening serial /dev/ttyACM0 @ 115200
espnow2mqtt-host  | 2026-09-11 12:00:00 INFO espnow2mqtt: MQTT connected rc=Success
espnow2mqtt-host  | 2026-09-11 12:00:01 INFO espnow2mqtt: coordinator hello: {'type': 'hello', ...}
espnow2mqtt-host  | 2026-09-11 12:00:01 INFO espnow2mqtt: coord: coordinator ready (esp-idf mesh)
```

`PYTHONUNBUFFERED: "1"` 是必需的——不然 Python 的 stdout 会被缓冲，
`docker logs` 里看不到实时日志。

### 2.4 加参数

`command:` 是个 YAML 列表，加参数就往里添两行（参数和值各一行）：

```yaml
    command:
      - python
      - -m
      - espnow2mqtt
      - --port
      - /dev/ttyACM0
      - --mqtt-host
      - 192.168.1.10
      - --base-topic
      - espnow2mqtt
      - -v
```

**别把 `--mqtt-host 192.168.1.10` 写成一行**，Docker 会把它当成一个参数传进去，
argparse 会报 `unrecognized arguments`。

完整参数表见 [bridge.md §12](bridge.md#12-命令行参数全表)。

---

## 3. systemd（裸机）

### 3.1 装

```bash
sudo useradd -r -s /usr/sbin/nologin -G dialout espnow2mqtt
sudo mkdir -p /opt/espnow2mqtt /var/lib/espnow2mqtt
sudo chown espnow2mqtt: /var/lib/espnow2mqtt

cd /opt/espnow2mqtt
sudo git clone https://github.com/SFNFIH/espnow2mqtt-host.git .
sudo python3 -m venv .venv
sudo .venv/bin/pip install -r requirements.txt
```

`-G dialout` 是关键：串口设备（`/dev/ttyACM*`）属于 `dialout` 组，
不加进去会 `PermissionError: [Errno 13]`。有些发行版用 `uucp` 而不是 `dialout`，
用 `ls -l /dev/ttyACM0` 看第四列。

### 3.2 unit 文件

`/etc/systemd/system/espnow2mqtt.service`：

```ini
[Unit]
Description=ESP-NOW to MQTT bridge
Documentation=https://github.com/SFNFIH/espnow2mqtt-host
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=espnow2mqtt
Group=dialout
WorkingDirectory=/opt/espnow2mqtt
Environment=PYTHONUNBUFFERED=1
ExecStart=/opt/espnow2mqtt/.venv/bin/python -m espnow2mqtt \
    --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00 \
    --mqtt-host 127.0.0.1 \
    --mqtt-port 1883 \
    --base-topic espnow2mqtt \
    --devices /var/lib/espnow2mqtt/devices.json

Restart=always
RestartSec=5

# 收紧权限
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/espnow2mqtt

[Install]
WantedBy=multi-user.target
```

几个要点：

| 指令 | 为什么 |
|---|---|
| `Restart=always` + `RestartSec=5` | Bridge 崩了自动起。**但 USB 断开时 Bridge 不会崩**（只刷日志），所以这不是万能的 |
| `Group=dialout` | 串口访问权限。用户已经在组里了，这里再声明一次更明确 |
| `ProtectSystem=strict` + `ReadWritePaths` | 只有 `devices.json` 那个目录可写 |
| `--devices /var/lib/...` | **必须指定绝对路径**。默认的 `data/devices.json` 是相对 `WorkingDirectory` 的，配合 `ProtectSystem=strict` 会写不进去 |
| `After=network-online.target` | 等网络起来再连 MQTT。其实 paho 会自己重试，但这样日志干净点 |

### 3.3 启动

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now espnow2mqtt
sudo systemctl status espnow2mqtt
journalctl -u espnow2mqtt -f
```

### 3.4 用 udev 规则代替 by-id

如果 `by-id` 路径太长看着难受，可以自己造一个固定名：

`/etc/udev/rules.d/99-espnow2mqtt.rules`：

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", ATTRS{serial}=="7C:DF:A1:00:11:22", SYMLINK+="espnow-coord", GROUP="dialout", MODE="0660"
```

`303a:1001` 是 Espressif 的 USB Serial/JTAG VID:PID。`serial` 就是 S3 的 MAC，
用 `udevadm info -a -n /dev/ttyACM0 | grep serial` 查。

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
ls -l /dev/espnow-coord      # → ../ttyACM0
```

然后 unit 里用 `--port /dev/espnow-coord`。

**有多个 S3 时这个规则特别有用**：按 `serial` 区分，每块板子一个固定名。

---

## 4. 直接跑（调试用）

```bash
git clone https://github.com/SFNFIH/espnow2mqtt-host.git
cd espnow2mqtt-host
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

python -m espnow2mqtt \
  --port /dev/ttyACM0 \
  --mqtt-host localhost \
  -v
```

**`-v` 一定要开**，否则你看不到：

- 非 JSON 行（`LOG.debug("non-json: …")`）
- `pong`
- 成功的 `ack`

这三样恰好是调协议问题时最需要的。

依赖只有两个（`requirements.txt`）：

```
paho-mqtt>=2.0.0
pyserial>=3.5
```

**Python 需要 3.10+**（代码里用了 `X | None` 的联合类型语法和
`dict[str, Any]` 的内置泛型）。`pyproject.toml` 里有 `requires-python = ">=3.10"`。

也可以装成命令：

```bash
pip install -e .
espnow2mqtt --port /dev/ttyACM0 --mqtt-host localhost -v
```

`pyproject.toml` 里定义了 entry point `espnow2mqtt = "espnow2mqtt.__main__:main"`。

### 跑测试

```bash
python3 -m pytest tests -q
```

`tests/test_offline.py` 是**纯离线测试**，不需要硬件也不需要 MQTT：
它用 `Bridge.__new__(Bridge)` 绕过 `__init__`，只测纯函数
（`_slug`、`_find_by_slug`、`_parse_caps`）和 JSON 样本的可解析性。

---

## 5. 串口路径与权限

### 5.1 找到正确的路径

```bash
ls -l /dev/serial/by-id/
```

```
usb-Espressif_USB_JTAG_serial_debug_unit_7C:DF:A1:00:11:22-if00 -> ../../ttyACM0
```

**这个路径包含 S3 的 MAC，插拔和重启都不会变。永远用它。**

`/dev/ttyACM0` 的编号取决于枚举顺序：多插一个 USB 串口设备、或者
重启后枚举顺序变了，`ttyACM0` 就可能指向别的东西。

### 5.2 确认是 S3 不是别的板子

```bash
udevadm info -n /dev/ttyACM0 | grep -E 'ID_VENDOR|ID_MODEL|ID_SERIAL'
```

S3 原生 USB 的特征：

| 字段 | 值 |
|---|---|
| `ID_VENDOR_ID` | `303a`（Espressif） |
| `ID_MODEL_ID` | `1001`（USB JTAG/serial debug unit） |
| `ID_SERIAL_SHORT` | S3 的 MAC，格式 `7C:DF:A1:00:11:22` |

如果 VID 是 `10c4`（CP210x）或 `1a86`（CH340），那是外置 USB-UART 芯片的板子——
**这块板子不能当协调器**，协调器固件用的是 `usb_serial_jtag` 驱动，
需要芯片原生 USB。见 [coordinator.md](coordinator.md)。

### 5.3 权限

```bash
ls -l /dev/ttyACM0
# crw-rw---- 1 root dialout 166, 0 Sep 11 12:00 /dev/ttyACM0
```

跑 Bridge 的用户必须在第四列那个组里：

```bash
sudo usermod -aG dialout $USER
# 重新登录才生效，或者 newgrp dialout
groups            # 确认
```

Docker 里用 `devices:` 映射就绕过了这个问题（容器里是 root）。
HA Add-on 的 `uart: true` + `usb: true` 也是。

### 5.4 串口被占用

**串口是独占的。** 同时只能有一个进程打开它。常见冲突：

```bash
sudo fuser -v /dev/ttyACM0
```

| 占用者 | 怎么办 |
|---|---|
| 另一个 Bridge 实例 | `systemctl stop espnow2mqtt` / `docker compose down` |
| `idf.py monitor` | Ctrl+] 退出 |
| `ModemManager`（某些发行版会去探测串口） | 见下 |

ModemManager 会在 USB 串口刚插上时往里发 AT 命令探测调制解调器，
这会干扰前几秒的通讯。屏蔽掉：

`/etc/udev/rules.d/99-espnow-nomm.rules`：

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ENV{ID_MM_DEVICE_IGNORE}="1"
```

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## 6. 部署检查清单

跑起来之后按顺序验证：

### 6.1 Bridge 起来了

```bash
journalctl -u espnow2mqtt -n 30      # systemd
docker compose logs --tail 30        # docker
# add-on 看 Log 标签
```

必须看到这三行：

```
INFO espnow2mqtt: MQTT connected rc=Success
INFO espnow2mqtt: coordinator hello: {'type': 'hello', ...}
INFO espnow2mqtt: coord: coordinator ready (esp-idf mesh)
```

| 少了哪行 | 问题 |
|---|---|
| `MQTT connected` | broker 地址/认证错，或 broker 没跑 |
| `coordinator hello` | **串口选错了，或 S3 没在跑**。这是最常见的错 |
| `coord: coordinator ready` | 收到 hello 但没收到 ready → S3 固件版本不对 |

### 6.2 MQTT 上有东西

```bash
mosquitto_sub -t 'espnow2mqtt/bridge/#' -v
```

```
espnow2mqtt/bridge/state online
espnow2mqtt/bridge/info {"type":"hello","version":2,...}
espnow2mqtt/bridge/devices []
```

`bridge/info` 有内容 = S3 真的在说话（见
[mqtt.md §2](mqtt.md#2-bridgestate) 的那张判断表）。

顺手确认信道：

```bash
mosquitto_sub -t espnow2mqtt/bridge/info -C 1 | python3 -c 'import json,sys; print("channel =", json.load(sys.stdin)["channel"])'
```

**这个值必须和 C3 设备固件的 `CONFIG_EN2M_WIFI_CHANNEL` 一致。**

### 6.3 能加设备

```bash
mosquitto_pub -t espnow2mqtt/bridge/request/permit_join -m 60
```

日志里应该立刻出现 `coord: pairing_enabled`。然后给 C3 上电，
60 秒内应该看到：

```
INFO espnow2mqtt: device online AA:BB:CC:DD:EE:FF (living_room) role=leaf hop=1 via=7C:DF:A1:00:11:22
```

### 6.4 状态能上来

```bash
mosquitto_sub -t 'espnow2mqtt/+/state' -v
```

叶子节点默认 30 秒一条。等一分钟，应该至少看到两条。

### 6.5 命令能下去

```bash
mosquitto_pub -t espnow2mqtt/living_room/set -m '{"switch":"ON"}'
```

日志里：

```
INFO espnow2mqtt: MQTT espnow2mqtt/living_room/set => {"switch":"ON"}
```

**没有** `command N to ... failed` 警告，而且 `<slug>/state` 里
`switch` 变成 `ON`——这说明 ACK 和后续状态上报都正常了。

### 6.6 HA 里出实体

装 [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha) 集成，
配置时的 base topic 填和 Bridge 一样的值。

---

## 7. 升级

### 7.1 只升 Bridge

```bash
# systemd
cd /opt/espnow2mqtt && sudo git pull && sudo .venv/bin/pip install -r requirements.txt
sudo systemctl restart espnow2mqtt

# docker
git pull && docker compose up -d --build

# add-on：Add-on 页面点 Update，或者 cd /addons/espnow2mqtt-host && git pull
```

`devices.json` 向前兼容（只有 3 个字段），升级不会丢设备名。

**升级会让 `last_state` 清空**，所以重启后几轮上报内 HA 里可能有实体
短暂 unknown。见 [bridge.md §10](bridge.md#10-devicesjson持久化了什么没持久化什么)。

### 7.2 升协调器固件

```bash
cd firmware/coordinator
git pull
idf.py build flash
```

**空口协议版本 `EN2M_VERSION` 目前是 2，和 C3 设备固件的版本必须一致。**
只要 `EN2M_VERSION` 没变，协调器和设备可以分别升级、不用同步刷机。
版本变了的话 `bridge/info` 里的 `version` 会变，那时才需要两边一起升。

烧固件时 Bridge 必须停掉（串口独占）：

```bash
sudo systemctl stop espnow2mqtt
idf.py -p /dev/ttyACM0 flash
sudo systemctl start espnow2mqtt
```

### 7.3 `components/en2m` 要两个仓库同步

本仓库的 `components/en2m` 和 device 仓库的**逐字节相同**。
改了组件必须同时更新两个仓库，否则协调器和设备会跑在不同的协议实现上。
见 [README.md](README.md#关于-componentsen2m)。

---

## 8. 运维要点

### 8.1 日志量

| 级别 | 每个设备每 30 s | 备注 |
|---|---|---|
| 默认 INFO | 0 行 | `device`/`info` 和 `state` 都不打日志 |
| `-v`（DEBUG） | 3～4 行 | 每条 `state`、`pong`、成功的 `ack` 都打 |

**生产环境别开 `-v`**——32 个设备会产生大约每秒 4 行日志，
一天 30 万行。systemd 的 journal 有 rate limit，会开始丢日志。

INFO 级别下只有这些会打：设备上下线、MQTT 连接/重连、收到的每条 MQTT 命令、
S3 的 `log` 行、以及失败的 ACK。这个量很小。

### 8.2 devices.json 的写入频率

`_save_devices()` 在**每一条 `device` 行**都会调一次，也就是每个设备每 30 s 一次
同步 `write_text()`。32 个设备大约每秒一次。

对 SSD/eMMC 无所谓。**跑在 SD 卡上的树莓派**要留意——这是持续的小文件写。
缓解办法：

- 把 `--devices` 指到 tmpfs（丢掉持久化，重启后设备名要重新学）
- 或者改代码：内容没变时跳过写入（当前实现没做这个优化）

### 8.3 需要监控什么

| 信号 | 怎么看 | 意味着 |
|---|---|---|
| `bridge/state` | MQTT | Bridge 死活 |
| `bridge/info` 为空 | MQTT | 串口错 / S3 没跑 |
| 日志刷 `serial error:` | 日志 | **USB 掉了，必须手工重启** |
| 日志有 `coord: rx_dropped=N` 且 N 快速增长 | 日志 | 协调器接收队列溢出，设备太多/上报太频繁 |
| 日志有 `coord: peer table full` | 日志 | 超过 32 个设备，需要重编固件 |
| 日志有 `failed: timeout` | 日志 | 某个设备失联或信号差 |
| `bridge/devices` 里 `rssi < -85` | MQTT | 信号临界，考虑加 router 节点 |

一个最小的 HA 告警（放 `configuration.yaml`）：

```yaml
template:
  - binary_sensor:
      - name: "ESP-NOW Bridge Down"
        state: "{{ states('sensor.espnow_bridge_state') != 'online' }}"
```

（`sensor.espnow_bridge_state` 由 espnow2mqtt-ha 集成提供。）

### 8.4 备份

| 跑法 | 要备份什么 |
|---|---|
| Add-on | **不用管**，`/data` 在 HA 的备份里 |
| Docker | `./data/devices.json` |
| systemd | `/var/lib/espnow2mqtt/devices.json` + unit 文件 |

`devices.json` 丢了不致命——设备会重新上报 name/model，Bridge 会重新学。
唯一的影响是重新学到之前 slug 会退化成紧凑 MAC，于是 HA 里的实体 ID 会变一轮。

---

## 相关文档

- [quickstart.md](quickstart.md) — 端到端第一次跑通
- [bridge.md](bridge.md) — Bridge 的内部实现和全部命令行参数
- [coordinator.md](coordinator.md) — S3 固件的编译和烧录
- [mqtt.md](mqtt.md) — 验证用的全部主题
- [troubleshooting.md](troubleshooting.md) — 按症状排查
