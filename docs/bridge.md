# Bridge 深入（`espnow2mqtt/__main__.py`）

Bridge 是一个**单文件 Python 程序**（742 行），职责只有一句话：

> 把 S3 从 USB 吐出来的 NDJSON 行，翻译成 MQTT 上的主题 + JSON；
> 把 MQTT 上的命令，翻译成 USB 上的 `cmd` 行。

它**不解析空口帧**，**不认识 cluster / attribute / endpoint**，
甚至**不知道"灯"和"温度传感器"的区别**——除了 `--ha-discovery` 那一小段（见
[§9](#9-ha_discovery为什么默认关)）。

- 上游协议：[usb-protocol.md](usb-protocol.md)
- 下游协议：[mqtt.md](mqtt.md)
- 整条链路的分工：[architecture.md](architecture.md)

目录：

1. [进程结构与三个线程](#1-进程结构与三个线程)
2. [`Device`：Bridge 记住的全部东西](#2-devicebridge-记住的全部东西)
3. [slug：MQTT 主题里的设备名](#3-slugmqtt-主题里的设备名)
4. [启动序列](#4-启动序列)
5. [串口读取：`_serial_loop` 与 `_handle_serial`](#5-串口读取_serial_loop-与-_handle_serial)
6. [状态上行：`_on_state` 的合并逻辑](#6-状态上行_on_state-的合并逻辑)
7. [`caps` 推断：`_parse_caps`](#7-caps-推断_parse_caps)
8. [命令下行：`_on_mqtt_message`](#8-命令下行_on_mqtt_message)
9. [`ha_discovery`：为什么默认关](#9-ha_discovery为什么默认关)
10. [`devices.json`：持久化了什么、没持久化什么](#10-devicesjson持久化了什么没持久化什么)
11. [串口自动探测](#11-串口自动探测)
12. [命令行参数全表](#12-命令行参数全表)
13. [已知的并发弱点](#13-已知的并发弱点)

---

## 1. 进程结构与三个线程

Bridge 跑在一个进程里，有 **3 个线程**：

| 线程 | 谁创建 | 干什么 | 会碰哪些状态 |
|---|---|---|---|
| **主线程** | Python | `start()` 里建连接、起线程，然后 `while not self._stop: time.sleep(0.5)` 空转 | 启动时写 `self.devices`（`_load_devices`） |
| **serial** | `start()` 里 `threading.Thread(name="serial", daemon=True)` | 读串口、切行、`_handle_serial()` 分发 | **读写 `self.devices`**、`publish()` |
| **paho 网络线程** | `self.mqtt.loop_start()` | 收 MQTT 消息 → `_on_mqtt_message()`；重连 → `_on_mqtt_connect()` | **读 `self.devices`**、`self.cmd_id++`、`_serial_write()` |

主线程只是个"活着"的占位，真正的两条数据流各自跑在自己的线程上：

```
                    ┌──────────────┐
  USB ──read──────► │ serial 线程  │ ──publish──► MQTT
                    └──────────────┘
                    ┌──────────────┐
  MQTT ──on_message►│ paho 线程    │ ──write────► USB
                    └──────────────┘
```

**唯一的锁**是 `self._lock`，它只保护一件事：往串口写整行。

```python
def _serial_write(self, obj: dict[str, Any]) -> None:
    if not self.ser:
        return
    line = json.dumps(obj, separators=(",", ":")) + "\n"
    with self._lock:
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()
```

这个锁是必要的：`start()`（主线程）和 `_on_mqtt_message()`（paho 线程）都会写串口，
没有锁的话两行 JSON 可能交错，S3 那边就会收到一行语法错误的 JSON。
`self.devices` 和 `self.cmd_id` **没有锁**，为什么在实践中还能跑、风险在哪，见
[§13](#13-已知的并发弱点)。

进程退出靠信号：

```python
signal.signal(signal.SIGINT, _sig)
signal.signal(signal.SIGTERM, _sig)
```

`_sig` 调 `bridge.stop()`，它会主动往 `bridge/state` 发一条 retained `offline`，
然后 `loop_stop()` / `disconnect()` / 关串口。
如果进程是被 `SIGKILL` 或者断电干掉的，走不到 `stop()`，这时靠 MQTT 的 LWT
（在 `__init__` 里 `will_set`）由 broker 代发 `offline`。

---

## 2. `Device`：Bridge 记住的全部东西

```python
@dataclass
class Device:
    mac: str
    name: str = ""
    model: str = "c3-env"
    online: bool = False
    rssi: Optional[int] = None
    hop: Optional[int] = None
    via: str = ""
    node_role: str = ""          # leaf | router
    caps: list[str] = field(default_factory=list)
    last_state: dict[str, Any] = field(default_factory=dict)
    discovered: bool = False
    discovery_sig: str = ""      # caps 变了就重建 discovery
```

`self.devices` 是 `dict[str, Device]`，**key 是 MAC 字符串**（`"AA:BB:CC:DD:EE:FF"` 这种
大写带冒号的形式，因为 S3 的 `en2m_mac_to_str` 就是这么打的）。

| 字段 | 谁写 | 用在哪 |
|---|---|---|
| `mac` | `_ensure_device()` 建对象时 | `cmd` 下行寻址、`connections` |
| `name` | `device` 行里的 `name` | slug、HA 实体名 |
| `model` | `device` 行里的 `model` | caps 推断、HA `model` 字段 |
| `online` | `device` 的 online/offline 事件；`_on_state` 里"收到状态说明它活着" | `availability` 主题、设备列表 |
| `rssi` | `device` / `state` 行 | `bridge/devices` 里的诊断值 |
| `hop` / `via` | `device` / `state` 行（S3 加的，不是设备发的） | 注入到 `state` payload |
| `node_role` | `device` 行的 `node_role`，或 `state` payload 里的 `node_role` | 诊断 |
| `caps` | `_parse_caps()` | 决定要不要重建 discovery |
| `last_state` | `_on_state()` 的合并结果 | **下一次合并的基准**，见 [§6](#6-状态上行_on_state-的合并逻辑) |
| `discovered` / `discovery_sig` | `_publish_discovery()` | 避免重复发 discovery |

`_ensure_device()` 是唯一的建对象入口，它保证"只要收到过这个 MAC 的任何一行，
`self.devices` 里就有它"：

```python
def _ensure_device(self, mac: str) -> Device:
    if mac not in self.devices:
        self.devices[mac] = Device(mac=mac)
    return self.devices[mac]
```

注意 `model` 的默认值是 `"c3-env"`——这是个历史遗留的默认，只在设备一直没上报
`model` 时才会看到。正常设备的 `en2m_config_t.model` 会通过每一帧的包头带上来。

---

## 3. slug：MQTT 主题里的设备名

```python
def _slug(self, dev: Device) -> str:
    if dev.name:
        return dev.name.replace(" ", "_").lower()
    return dev.mac.replace(":", "").lower()
```

规则就两条：

| 条件 | slug | 例子 |
|---|---|---|
| 设备上报了 `name` | `name` 里空格换下划线，全小写 | `name="Living Room"` → `living_room` |
| 没有 `name` | MAC 去冒号全小写 | `aabbccddeeff` |

slug 决定了**所有**设备级主题：`espnow2mqtt/<slug>/state`、`/set`、`/availability`。

> **⚠️ slug 会变，变了老主题会成为孤儿**
>
> 设备第一帧（HELLO）通常已经带 `name`，所以正常情况下 slug 从一开始就是稳定的。
> 但如果你**改了固件里的 `en2m_config_t.name` 再重新烧**，slug 就变了：
>
> - 新主题 `espnow2mqtt/new_name/state` 开始更新
> - 老主题 `espnow2mqtt/old_name/state` 上的 **retained 消息还在**，而且永远不会再更新
> - HA 里如果是走 MQTT Discovery，会多出一个永远 unavailable 的旧设备
>
> 清理办法是往老主题发空 retained 消息：
>
> ```bash
> mosquitto_pub -t espnow2mqtt/old_name/state -r -n
> mosquitto_pub -t espnow2mqtt/old_name/availability -r -n
> ```
>
> 如果你不希望 slug 依赖 `name`，把固件的 `name` 留空，slug 就恒等于 MAC。

反向查找是 `_find_by_slug()`，它比 `_slug()` 宽松，接受两种写法：

```python
def _find_by_slug(self, slug: str) -> Optional[Device]:
    for d in self.devices.values():
        if self._slug(d) == slug:
            return d
    # 也接受不带冒号的裸 MAC
    compact = slug.replace(":", "").upper()
    for d in self.devices.values():
        if d.mac.replace(":", "").upper() == compact:
            return d
    return None
```

所以即使设备有 `name`，你也可以直接往 `espnow2mqtt/aabbccddeeff/set` 发命令，
这在调试时很方便（不用先知道设备叫什么）。

---

## 4. 启动序列

```python
def start(self) -> None:
    self.mqtt.connect(self.mqtt_host, self.mqtt_port, keepalive=60)
    self.mqtt.loop_start()

    self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
    time.sleep(0.5)
    self._serial_write({"type": "ping"})
    self._serial_write({"type": "list"})

    t = threading.Thread(target=self._serial_loop, name="serial", daemon=True)
    t.start()

    while not self._stop.is_set():
        time.sleep(0.5)
```

逐步看：

| 步 | 做什么 | 为什么 |
|---|---|---|
| 0 | `__init__` 里 `_load_devices()` | 先把 `devices.json` 里的名字读回来，这样第一条状态就能用对的 slug |
| 1 | `mqtt.connect()` + `loop_start()` | **先连 MQTT**。串口一打开就可能有数据进来，MQTT 没连上就发不出去 |
| 2 | `serial.Serial(..., timeout=0.2)` | 200 ms 读超时，让 `_serial_loop` 能定期检查 `_stop` |
| 3 | `time.sleep(0.5)` | 给 USB CDC 枚举/S3 复位留一点时间 |
| 4 | `{"type":"ping"}` | 探活，S3 会回 `pong` |
| 5 | `{"type":"list"}` | **要一遍全量 peer 表**，这样 Bridge 重启后不用等 30 s 心跳就能知道谁在线 |
| 6 | 起 serial 线程 | 之后所有上行都在这个线程里处理 |
| 7 | 主线程空转 | 只为让进程不退出，信号处理器在这里生效 |

注意第 3 步的 `time.sleep(0.5)` 之后**没有等 `pong` 才继续**。
`ping`/`list` 是"发了就不管"的，如果 S3 此刻正在复位（打开串口可能触发复位），
这两行会丢掉。这不致命——S3 每 30 s 会自己发 `hello`，
peer 表也会随着设备心跳逐渐填上，只是"Bridge 重启后立刻看到全量设备"这个优化会失效。
真的要确保拿到，重发一次 `list` 就行（见 [troubleshooting.md](troubleshooting.md)）。

`_on_mqtt_connect` 每次连上（包括自动重连）都会重放一遍状态：

```python
def _on_mqtt_connect(self, client, userdata, flags, reason_code, properties=None):
    client.subscribe(f"{self.base}/+/set")
    client.subscribe(f"{self.base}/bridge/request/+")
    client.publish(f"{self.base}/bridge/state", "online", retain=True)
    self._publish_bridge_discovery()
    for dev in self.devices.values():
        self._publish_discovery(dev)
    self._publish_device_list()
```

这一点很重要：**订阅是在 `on_connect` 里做的，不是在 `start()` 里**。
paho 断线重连后订阅会丢，放在 `on_connect` 里才能自动恢复。

---

## 5. 串口读取：`_serial_loop` 与 `_handle_serial`

```python
def _serial_loop(self) -> None:
    buf = ""
    while not self._stop.is_set():
        try:
            chunk = self.ser.read(256)
            if not chunk:
                continue
            buf += chunk.decode("utf-8", errors="ignore")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if line:
                    self._handle_serial(line)
        except Exception as exc:
            LOG.error("serial error: %s", exc)
            time.sleep(1)
```

几个设计点：

- **`read(256)` + `timeout=0.2`**：最多阻塞 200 ms，保证 `_stop` 能及时生效。
- **`errors="ignore"`**：USB 上偶尔会有半个 UTF-8 字符跨 chunk，或者 S3 复位时喷出的乱码字节。忽略掉总比抛异常好。
- **`buf` 跨 chunk 累积**：NDJSON 的行边界和 USB 的包边界无关，必须自己切行。
- **异常 → `sleep(1)` 继续**：串口被拔掉时 `read` 会抛异常，这里不退出、只是降频重试。**但 `self.ser` 不会被重新打开**——拔了 USB 再插回去，Bridge 不会自愈，得重启进程。用 systemd / Docker `restart: unless-stopped` / HA Add-on 的自动重启来兜。

`_handle_serial` 是一个纯分发：

```python
def _handle_serial(self, line: str) -> None:
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        LOG.debug("non-json: %s", line)
        return
    mtype = msg.get("type")
    if mtype == "hello":   ...
    elif mtype == "pong":  ...
    elif mtype == "log":   ...
    elif mtype == "device": self._on_device_event(msg)
    elif mtype == "state":  self._on_state(msg)
    elif mtype == "ack":    self._on_ack(msg)
    else: LOG.debug("ignored: %s", msg)
```

| `type` | 处理 | 副作用 |
|---|---|---|
| `hello` | `LOG.info`，发 `bridge/state=online`（retained），发 `bridge/info`（retained，整条 hello 原样转发），发 bridge discovery | 这是 Bridge 认定"S3 在"的唯一依据 |
| `pong` | 只 `LOG.debug` | 无。`ping` 的意义只是"看看串口通不通" |
| `log` | `LOG.info("coord: %s", msg["msg"])` | **不进 MQTT**。S3 的 mesh 日志只出现在 Bridge 的 stdout |
| `device` | `_on_device_event()` | 上下线、availability、`devices.json` |
| `state` | `_on_state()` | 状态合并 + 发 MQTT |
| `ack` | `_on_ack()` | 日志 + 发 `<slug>/command_result`，见下 |
| 其他 | `LOG.debug("ignored")` | 无。协议向前兼容：S3 以后加新行类型不会让老 Bridge 崩 |

**非 JSON 行走 `LOG.debug` 静默丢弃**，这是故意的：S3 复位时 ROM bootloader 会往
USB 喷一堆非 JSON 的启动信息，不该当成错误刷屏。所以调试"S3 到底在说什么"时
**必须开 `-v`**，否则你看不到这些行。

`_on_ack` 记日志，然后把协调器的裁决重新发布到 MQTT：

```python
def _on_ack(self, msg: dict[str, Any]) -> None:
    cid = msg.get("id")
    ok = bool(msg.get("ok"))
    error = str(msg.get("error") or "") or None
    pending = self.pending.pop(cid, None) if isinstance(cid, int) else None
    self._expire_pending()

    mac = str(msg.get("mac") or (pending.mac if pending else ""))
    dev = self.devices.get(mac)
    slug = self._slug(dev) if dev else (pending.slug if pending else "")

    if ok:
        LOG.debug("command %s to %s acked", cid, slug or mac or "?")
    else:
        LOG.warning(
            "command %s to %s failed: %s", cid, slug or mac or "?", error or "unknown"
        )

    if not slug:
        # An ack for something we cannot attribute — most likely a command
        # issued from the coordinator's own console rather than from us.
        return
    result: dict[str, Any] = {"id": cid, "ok": ok, "mac": mac or None}
    if error:
        result["error"] = error
    if pending is not None:
        result["payload"] = pending.payload
        result["elapsed_ms"] = int((time.time() - pending.sent_at) * 1000)
    self.mqtt.publish(
        f"{self.base}/{slug}/command_result",
        json.dumps(result),
        retain=False,
    )
```

主题格式和字段见 [mqtt.md §7](mqtt.md#11-slugcommand_result)。

> **为什么状态上报不够？**
>
> 成功的命令确实会紧跟一条新的状态上报（见
> [PROTOCOL.md](../protocol/PROTOCOL.md) 的"Acknowledgement and retries"），
> 所以 HA 能从"状态变了"推出"命令成功了"。
> **但失败的命令没有任何后续上报**——状态不变和"命令还在路上"、
> "设备本来就是这个值"在 MQTT 上完全无法区分。
>
> 而且有一类命令根本没有可读状态（`identify` 让设备闪灯），
> 对它们来说状态上报一点信息也提供不了。
>
> **0.3.x 里 `_on_ack` 只写一行日志就结束了。** 失败的命令在
> 这个进程之外完全不可见，HA 侧的唯一表现是"实体状态没变"。

### `pending`：为了能说出是哪条命令失败了

协调器的 ack 里只有 `id`、`ok`、`error`、`mac`——**没有原始命令内容**。
光说"命令 7 失败了"对使用者没用，所以 Bridge 自己记着发过什么：

```python
PENDING_TTL_S = 30.0

@dataclass
class PendingCommand:
    mac: str
    slug: str
    payload: dict[str, Any]
    sent_at: float
```

`_on_cmd`（MQTT `<slug>/set` 的处理）往 `self.pending[cmd_id]` 存一条，
`_on_ack` 把它取出来，于是 `command_result` 里能带上
`payload`（原始命令）和 `elapsed_ms`（往返耗时）。

`pending` 还解决了一个归属问题：ack 里的 `mac` 字段在
`send_fail` 的情况下**可能是空的**（S3 连路由都没查到）。
这时 `pending.mac` / `pending.slug` 是唯一的线索。

| 字段来源优先级 | |
|---|---|
| `mac` | ack 里的 → `pending.mac` → `""` |
| `slug` | 按 `mac` 查设备表 → `pending.slug` → `""` |

两者都拿不到就**不发 MQTT**，只留日志。那种 ack 通常是
协调器自己的串口控制台发出的命令，和 Bridge 无关。

### `_expire_pending`：协调器重启时不泄漏

```python
def _expire_pending(self) -> None:
    """Forget commands the coordinator never acked.

    It always should — it acks with `timeout` once the retries run out — but
    a coordinator reset mid-command would otherwise leak an entry forever.
    """
    if not self.pending:
        return
    deadline = time.time() - PENDING_TTL_S
    stale = [cid for cid, p in self.pending.items() if p.sent_at < deadline]
    for cid in stale:
        LOG.debug("forgetting unacked command %s", cid)
        del self.pending[cid]
```

正常情况下每条命令都会被 ack（重传耗尽后协调器会回
`error: "timeout"`），所以 `pending` 不会长。
但协调器在命令中途复位的话那条记录就永远等不到 ack 了。
30 秒的 TTL 远大于 1.6 秒的重传窗口，所以不会误删还在飞的命令。

清理时机是**每次收到 ack 时顺手做一次**，没有定时器——
这样即使 Bridge 长时间没有下行命令，也不会有后台任务空转。

`_on_device_event` 处理三种 `event`：

| `event` | 动作 |
|---|---|
| `online` | `dev.online = True`；`LOG.info` 带 role/hop/via；发 discovery；`<slug>/availability = "online"`（retained） |
| `offline` | `dev.online = False`；`<slug>/availability = "offline"`（retained） |
| `info` | **只有 `not dev.discovered` 时才发 discovery**；不动 `online` |

三种都会在最后调 `_save_devices()`（写文件 + 重发 `bridge/devices`）。
`info` 是 S3 对"已知设备的心跳"和对 `list` 的响应，出现得很频繁
（每个设备 30 s 一条），所以它刻意什么都不做——但它**会**更新
`dev.rssi`/`hop`/`via`/`name`/`model`（在 `_on_device_event` 开头的通用字段更新里），
这就是 `bridge/devices` 里的 RSSI 能保持新鲜的原因。

`offline` 事件里 S3 **不带** `rssi`/`hop`/`via`（见 `emit_device()` 里的
`if (strcmp(event, "offline") != 0)`），所以 `dev.rssi` 会**保留最后一次已知值**而不是清零。
`bridge/devices` 上一个 offline 设备的 RSSI 是"它掉线前的 RSSI"。

---

## 6. 状态上行：`_on_state` 的合并逻辑

这是整个 Bridge 最关键的一段。完整代码：

```python
def _on_state(self, msg: dict[str, Any]) -> None:
    mac = msg.get("mac", "")
    if not mac:
        return
    dev = self._ensure_device(mac)
    if msg.get("via"):
        dev.via = str(msg["via"])
    if "hop" in msg:
        try:
            dev.hop = int(msg["hop"])
        except (TypeError, ValueError):
            pass
    payload = msg.get("payload") or {}
    if not isinstance(payload, dict):
        payload = {"value": payload}
    if payload.get("node_role"):
        dev.node_role = str(payload["node_role"])
    new_caps = self._parse_caps(payload, dev.model)
    if new_caps and new_caps != dev.caps:
        dev.caps = new_caps
        dev.discovered = False
    if not dev.online:
        dev.online = True
        self._publish_discovery(dev)
        self.mqtt.publish(f"{self.base}/{self._slug(dev)}/availability", "online", retain=True)
    merged = dict(dev.last_state)
    merged.update(payload)
    if "switch" in merged:
        sw = str(merged["switch"]).upper()
        merged["switch"] = "ON" if sw in ("ON", "1", "TRUE") else "OFF"
    if "contact" in merged:
        c = str(merged["contact"]).upper()
        merged["contact"] = "ON" if c in ("ON", "1", "TRUE", "OPEN") else "OFF"
    if dev.hop is not None:
        merged["hop"] = dev.hop
    if dev.via:
        merged["via"] = dev.via
    if dev.caps:
        merged["caps"] = dev.caps
    dev.last_state = merged
    if not dev.discovered:
        self._publish_discovery(dev)
    topic = f"{self.base}/{self._slug(dev)}"
    self.mqtt.publish(f"{topic}/state", json.dumps(merged), retain=True)
    for key, val in merged.items():
        if key == "caps":
            continue
        self.mqtt.publish(f"{topic}/{key}", str(val), retain=False)
```

### 6.1 合并：为什么 160 字节的截断不是丢数据

```python
merged = dict(dev.last_state)
merged.update(payload)
```

两行代码，但它是整个系统能在 **160 字节**的 ESP-NOW 载荷里跑一个多属性设备的原因。

设备侧的上报有个硬上限 `EN2M_DATA_MAX = 160`。当属性太多装不下时，设备**不截断 JSON**，
而是按固定顺序丢掉可选字段（先 `node_role`，再 `caps`）——详见 device 仓库
`docs/reporting.md`。Bridge 这边把每一条新 payload **叠加**在上一条之上，
所以：

| 时刻 | 设备发的 payload | Bridge 的 `merged` |
|---|---|---|
| T0 | `{"switch":"ON","brightness":200,"node_role":"leaf","caps":["light"]}` | `switch=ON, brightness=200, node_role=leaf, caps=[light]` |
| T1（挤掉了 caps 和 node_role） | `{"switch":"ON","brightness":128,"color_temp":370}` | `switch=ON, **brightness=128**, color_temp=370, node_role=leaf, caps=[light]` |

T1 没带 `node_role`/`caps`，但 `merged` 里它们还在。这就是 PROTOCOL.md 里那句
"The host merges successive reports for a device, so a report that omits fields is not lossy"
的具体实现。

> **⚠️ 合并的代价：`merged` 只增不减**
>
> `dict.update()` 不会删 key。所以**设备没法通过"不发某个字段"来表达"这个字段消失了"**。
> 一旦某个 key 出现过，它就会永远留在 `<slug>/state` 里，直到 Bridge 重启
> （`last_state` 只在内存里，不进 `devices.json`，见 [§10](#10-devicesjson持久化了什么没持久化什么)）。
>
> 实践上这不是问题，因为设备的属性集是固定的。但如果你改了固件、去掉了一个属性，
> 老的 key 会一直挂在 retained 的 `<slug>/state` 上，直到你重启 Bridge **并且**
> 清掉 retained 消息。

### 6.2 归一化：只有 `switch` 和 `contact`

```python
if "switch" in merged:
    merged["switch"] = "ON" if str(merged["switch"]).upper() in ("ON","1","TRUE") else "OFF"
if "contact" in merged:
    merged["contact"] = "ON" if str(merged["contact"]).upper() in ("ON","1","TRUE","OPEN") else "OFF"
```

| 字段 | 认作 ON 的输入 | 其他一切 |
|---|---|---|
| `switch` | `"ON"` / `"1"` / `"TRUE"`（大小写无关） | `"OFF"` |
| `contact` | `"ON"` / `"1"` / `"TRUE"` / **`"OPEN"`** | `"OFF"` |

只有这两个字段被归一化，**其他字段（`brightness`、`temperature`、`cover`、`lock`、
`fan_mode`、`hvac_mode`…）原样透传**。原因是 HA 的 `switch` 和 `binary_sensor`
对 payload 敏感（必须严格匹配 `payload_on`/`payload_off`），而数值和枚举类实体
用 `value_template` 就能处理。

`contact` 多认一个 `"OPEN"` 是为了兼容早期固件——现在的 `en2m` 组件发的是
`"ON"`/`"OFF"`，但这条兼容留着没坏处。

注意归一化作用在 **`merged`** 上而不是 `payload` 上，所以它每次都会重新跑一遍
（包括对上一次留下的值）。这是幂等的，`"ON"` 归一化还是 `"ON"`。

### 6.3 注入：`hop` / `via` / `caps`

```python
if dev.hop is not None: merged["hop"] = dev.hop
if dev.via:             merged["via"] = dev.via
if dev.caps:            merged["caps"] = dev.caps
```

这三个是**Bridge/S3 加的，不是设备发的**（`hop`/`via` 由 S3 在 `on_uplink` 里填，
`caps` 可能来自设备也可能是 Bridge 推断的）。它们在**每次**发布时重新注入，
所以即使设备发的 payload 挤掉了 `caps`，MQTT 上的 `state` 也一定带着 `caps`。

顺序很重要：注入在 `merged.update(payload)` **之后**，所以
**S3 提供的 `hop`/`via` 会覆盖设备 payload 里同名的字段**。这是对的——
拓扑信息以协调器的观测为准。

### 6.4 发布：一个聚合主题 + 一堆扁平主题

```python
self.mqtt.publish(f"{topic}/state", json.dumps(merged), retain=True)
for key, val in merged.items():
    if key == "caps":
        continue
    self.mqtt.publish(f"{topic}/{key}", str(val), retain=False)
```

| 主题 | retain | 内容 |
|---|---|---|
| `<base>/<slug>/state` | **是** | 整个 `merged` 的 JSON。**这是 HA 集成唯一读的主题** |
| `<base>/<slug>/<key>` | 否 | `str(val)`，每个字段一个主题 |

扁平主题是给"手搓 MQTT 自动化 / Node-RED / `mosquitto_sub` 调试"用的方便主题。
关于它们的两个坑：

1. **`str(val)` 是 Python 的 `str()`，不是 JSON。**
   `True` → `"True"`（不是 `"true"`），`None` → `"None"`，
   列表 → `"['a', 'b']"`（单引号，不是合法 JSON）。
   所以**别在自动化里解析扁平主题的复杂值**，聚合主题才是权威。
   `caps` 被显式跳过就是因为它是列表，`str()` 出来的东西没意义。
2. **不 retain。** 新订阅者订上来不会立刻收到值，得等下一次上报
   （叶子节点默认 30 s，见 device 仓库 `docs/reporting.md`）。
   要"订上来就有值"，用 `<slug>/state`。

### 6.5 `_on_state` 的隐含在线判定

```python
if not dev.online:
    dev.online = True
    self._publish_discovery(dev)
    self.mqtt.publish(f"{...}/availability", "online", retain=True)
```

**收到任何状态 ⇒ 设备在线。** 这是对 S3 `device` 事件的补充：如果 Bridge 是在
设备已经加入 mesh 之后才启动的，它可能永远收不到那个设备的 `online` 事件
（S3 只在 `first`，也就是 peer 表里首次见到时才发 `online`），但只要收到一条状态
就能认定它活着。

反过来，**`_on_state` 永远不会把设备置为 offline**——离线判定只有一个来源：
S3 的 `device`/`offline` 事件（S3 侧 `EN2M_OFFLINE_MS` 超时，见
[coordinator.md](coordinator.md)）。Bridge 自己不做超时。

---

## 7. `caps` 推断：`_parse_caps`

`caps` 决定 `--ha-discovery` 要建哪些实体。它有**三级 fall-through**：

```python
@staticmethod
def _explicit_caps(payload: dict[str, Any]) -> list[str]:
    """The caps the device stated outright, if the report had room for them."""
    caps_raw = payload.get("caps")
    if isinstance(caps_raw, list):
        return [str(c).strip().lower() for c in caps_raw if str(c).strip()]
    if isinstance(caps_raw, str) and caps_raw.strip():
        return [c.strip().lower() for c in caps_raw.split(",") if c.strip()]
    return []

def _parse_caps(self, payload: dict[str, Any], model: str = "") -> list[str]:
    caps = self._explicit_caps(payload)
    if caps:
        return caps
    # Infer from payload keys / model
    inferred: list[str] = []
    for key in ("temperature", "humidity", "switch", "contact", "button", "power", "energy"):
        if key in payload:
            inferred.append(key)
    model_l = (model or "").lower()
    if not inferred:
        if "th" in model_l or "temp" in model_l:
            inferred = ["temperature", "humidity"]
        elif "contact" in model_l or "door" in model_l:
            inferred = ["contact"]
        elif "plug" in model_l:
            inferred = ["switch", "power", "energy"]
        elif "switch" in model_l or "relay" in model_l:
            inferred = ["switch"]
    return inferred
```

| 级 | 依据 | 结果 |
|---|---|---|
| 1 | payload 里的 `caps`，可以是 JSON 数组，**也可以是逗号分隔的字符串** | 直接用（小写、去空格） |
| 2 | payload 里出现了这 7 个 key 之一：`temperature` `humidity` `switch` `contact` `button` `power` `energy` | 用出现的那些 |
| 3 | `model` 名里的子串 | `th`/`temp` → 温湿度；`contact`/`door` → contact；`plug` → 开关+功率+电量；`switch`/`relay` → 开关 |

级 1 接受逗号字符串是为了省字节：`"caps":"light"` 比 `"caps":["light"]` 短 2 字节，
在 160 字节的预算里有时候就差这么点。

级 2/3 只覆盖那 7 个"老"能力，**不认识 `light` / `cover` / `lock` / `fan` /
`climate`**。这不是 bug，是因为 `--ha-discovery` 本身只支持那 7 个（见
[§9](#9-ha_discovery为什么默认关)），推断更多也没用。现在的 `en2m` 组件**总是**
上报显式 `caps`，所以级 2/3 实际上只在下面两种情况生效：

- 设备的 `caps` 被 160 字节预算挤掉了，而且 Bridge 刚重启（`last_state` 是空的，
  所以 `merged` 里也没有 `caps`）
- 你在用第三方/手搓固件，不发 `caps`

`_explicit_caps` 被单独拆出来，是因为**"设备明说的"和"我们猜的"
必须区别对待**。合并规则在 `_update_caps` 里：

```python
def _update_caps(self, dev: Device, payload: dict[str, Any]) -> None:
    """Fold a report's capabilities into what we already knew.

    A device drops `caps` from its report when the 160-byte budget gets
    tight, and what we can infer from the remaining keys is much coarser
    than what the firmware would have told us. Replacing the stored caps
    with that guess used to make capabilities flicker — a light would be
    downgraded to a plain switch for one report. So only an explicit list
    may replace; a guess may only add.
    """
    explicit = self._explicit_caps(payload)
    if explicit:
        new_caps = explicit
    else:
        new_caps = list(dev.caps)
        for cap in self._parse_caps(payload, dev.model):
            if cap not in new_caps:
                new_caps.append(cap)
    if new_caps != dev.caps:
        dev.caps = new_caps
        dev.discovered = False      # 下面会重新 _publish_discovery
```

| 这条上报 | 对已存的 `caps` 做什么 |
|---|---|
| 带显式 `caps` 列表 | **整个替换**。设备是权威，能力真的可以变少 |
| 没带 `caps`，但能猜出点东西 | **只增不减**。猜出来的比设备明说的粗得多，不许它降级 |
| 没带 `caps`，也猜不出东西 | 什么都不做 |

第二行是关键。一个调光灯的上报在 160 字节预算紧张时会被砍成
`{"switch":"ON","brightness":180}`——`caps` 不见了。
这时级 2 的推断只能看出 `switch`（`brightness` 不在那 7 个 key 里）。

> **0.3.x 里这一条会直接替换：**
>
> ```python
> if new_caps and new_caps != dev.caps:
>     dev.caps = new_caps
> ```
>
> `and new_caps` 保住了"全空不覆盖"的情况，
> 但**猜出来的非空结果会盖掉设备明说过的**。
> 于是一个 `caps: ["light"]` 的调光灯，在某一条挤掉了 caps 的上报之后
> 变成 `caps: ["switch"]`，下一条完整上报又变回 `["light"]`——
> **能力在两个值之间抖动**，而且每次抖动都会
> `dev.discovered = False`，触发一次 discovery 重建。
>
> HA 集成 0.4.0 加了"cap 消失就删实体"之后，这个抖动的代价从
> "多一次 discovery 重建"变成了**实体被反复创建和删除**，
> 所以这里必须先修。

---

## 8. 命令下行：`_on_mqtt_message`

订阅了两个 wildcard：

```python
client.subscribe(f"{self.base}/+/set")
client.subscribe(f"{self.base}/bridge/request/+")
```

`_on_mqtt_message` 里先匹配 `permit_join`，再匹配 `/set`：

### 8.1 `bridge/request/permit_join`

```python
if topic == f"{self.base}/bridge/request/permit_join":
    seconds = 60
    try:
        body = json.loads(raw) if raw.startswith("{") else {}
        seconds = int(body.get("value", raw) or 60)
    except Exception:
        try:
            seconds = int(raw)
        except Exception:
            seconds = 60
    self._serial_write({"type": "pair", "seconds": seconds})
    return
```

三种 payload 都能用：

| 发什么 | `seconds` |
|---|---|
| `60` | 60 |
| `{"value":60}` | 60 |
| `{"value":"120"}` | 120 |
| `""` / `garbage` / `{}` | 60（兜底默认） |

S3 侧会把 `seconds` 夹到 `[1, 300]`，所以往这里发 `99999` 实际得到 300 s。

`bridge/request/+` 订阅的是**所有** `request` 下的主题，但只有 `permit_join`
被处理，其他的会掉到下面的 `topic.endswith("/set")` 检查（不匹配）然后**静默丢弃**。
往 `bridge/request/unpair` 发东西不会有任何反应——**`unpair` 在 S3 侧实现了，
但 Bridge 没有暴露 MQTT 入口**。要用它得直接往串口写（见
[usb-protocol.md](usb-protocol.md)）。

### 8.2 `<slug>/set`

```python
if topic.endswith("/set"):
    slug = topic[len(self.base) + 1 : -4]
    dev = self._find_by_slug(slug)
    if not dev:
        LOG.warning("unknown device slug %s", slug)
        return
    try:
        payload = json.loads(raw)
        if not isinstance(payload, dict):
            payload = {"switch": str(payload)}
    except json.JSONDecodeError:
        payload = {"switch": raw.strip().upper()}
    cid = self.cmd_id
    self.cmd_id += 1
    self._serial_write({"type": "cmd", "mac": dev.mac, "id": cid, "payload": payload})
```

slug 提取是纯字符串切片：`topic[len(base)+1 : -4]` 去掉前缀 `base/` 和后缀 `/set`。

payload 解析有三条路：

| MQTT payload | 解析成 | 说明 |
|---|---|---|
| `{"switch":"ON"}` | `{"switch":"ON"}` | 正常路径，dict 原样透传 |
| `{"ep":1,"cluster":"on_off","command":"toggle"}` | 原样透传 | cluster 风格，Bridge 完全不看内容 |
| `ON` | `{"switch":"ON"}` | 不是合法 JSON → `JSONDecodeError` → 裸值路径，**转大写** |
| `on` | `{"switch":"ON"}` | 同上 |
| `1` | `{"switch":"1"}` | `json.loads("1")` **成功**，得到 int `1` → 不是 dict → `{"switch": str(1)}`。注意这条**没走大写路径**，但设备侧会把 `"1"` 当 ON |
| `[1,2]` | `{"switch":"[1, 2]"}` | 合法 JSON、不是 dict → `str()` 出来是 Python repr。没意义，但不会崩 |

**Bridge 对 payload 的内容零校验。** `{"brightness":"洗衣机"}` 会被原样送到设备，
由设备侧的写回调去拒绝（返回非 `ESP_OK`，属性不提交）。这是刻意的分层：
Bridge 不知道有哪些属性、取值范围是什么，那是数据模型层的事。

`id` 用的是自增的 `self.cmd_id`（从 1 开始），**非零**，所以 S3 会启动
ACK/重传（4 次、0/400/800/1200 ms）。想要 fire-and-forget 就得手工发 `id:0` 的串口行，
MQTT 路径上没法做到。

`cmd_id` 会一直涨，**溢出后 S3 侧会截成 `uint16_t`**（`(uint16_t)idj->valueint`）。
65536 条命令后 `id` 会回绕到小数字。由于 ACK 匹配是 `(dest, cmd_id)` 且窗口只有
1.6 s，回绕在实践中撞不上。

找不到 slug 只 `LOG.warning` 就返回：

```python
LOG.warning("unknown device slug %s", slug)
```

最常见的触发场景是 **HA 里存着一个已经被 unpair / 换了 name 的老实体**。
排查见 [troubleshooting.md](troubleshooting.md)。

---

## 9. `ha_discovery`：为什么默认关

```python
parser.add_argument(
    "--ha-discovery",
    action="store_true",
    help="Publish Home Assistant MQTT Discovery (default off; use HA integration instead)",
)
```

默认 **off**。关掉时 `_publish_discovery` 退化成一个赋值：

```python
def _publish_discovery(self, dev: Device) -> None:
    if not self.ha_discovery:
        # 实体由 Home Assistant 自定义集成创建
        dev.discovered = True
        return
    ...
```

（把 `discovered` 置 `True` 是为了让 `_on_state` 里那些
`if not dev.discovered: self._publish_discovery(dev)` 的分支不要每条状态都白跑一遍。）

**为什么推荐关：** 内置 discovery 只覆盖 7 种能力：

| cap | HA 平台 | 实体 |
|---|---|---|
| `switch` | `switch` | Switch |
| `temperature` | `sensor` | Temperature（°C，`measurement`） |
| `humidity` | `sensor` | Humidity（%，`measurement`） |
| `contact` | `binary_sensor` | Contact（`device_class: door`） |
| `power` | `sensor` | Power（W，`measurement`） |
| `energy` | `sensor` | Energy（Wh，`total_increasing`） |
| `button` | `sensor` | Button（不是 HA 的 `event` 实体，只是个字符串 sensor） |

另外**无条件**发三个诊断实体（不看 caps）：

| 实体 | state_topic | 备注 |
|---|---|---|
| RSSI | `bridge/devices` | `value_template` 是个 Jinja 循环，在整个设备列表里找自己的 MAC |
| Mesh Hop | `<slug>/state` | `value_json.hop \| default(0)` |
| Node Role | `<slug>/state` | `value_json.node_role \| default('unknown')` |

**这 7 种能力是整个 `en2m` 支持的设备类型的一个很小的子集。** 组件侧支持
调光灯、彩灯、窗帘、门锁、风扇、温控器、占位传感器…（device 仓库
`docs/data-model.md` 里有 16 种设备配方），这些在内置 discovery 里**一个都没有**。

所以：

- **想要完整的设备支持** → 关掉 `--ha-discovery`，装
  [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha) 集成。
  它读 `<slug>/state` 里的 `caps` 和字段，自己建实体。
- **只用温湿度/开关/门磁这种简单设备，又不想装集成** → 开 `--ha-discovery`，
  能用，但别指望灯的色温能出来。
- **两个都开** → 会出现重复实体（一套来自 discovery，一套来自集成）。别这么干。

还有个 `--no-ha-discovery` 参数，它是 `argparse.SUPPRESS` 隐藏的历史别名，
只为了让老的命令行不报错。最终值是：

```python
ha_discovery=bool(args.ha_discovery) and not bool(args.no_ha_discovery)
```

即 `--no-ha-discovery` 会**否决** `--ha-discovery`。

discovery 的重发由 `discovery_sig` 控制：

```python
sig = ",".join(sorted(caps)) + "|" + dev.model
if dev.discovered and dev.discovery_sig == sig:
    return
```

只有 caps 集合或 model 变了才会重发。`sorted()` 保证 caps 顺序变化不会误触发。

---

## 10. `devices.json`：持久化了什么、没持久化什么

```python
def _save_devices(self) -> None:
    self.devices_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        mac: {"name": d.name, "model": d.model, "node_role": d.node_role}
        for mac, d in self.devices.items()
    }
    self.devices_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    self._publish_device_list()
```

文件长这样（默认 `data/devices.json`，Add-on 里是 `/data/devices.json`）：

```json
{
  "AA:BB:CC:DD:EE:FF": {
    "name": "living_room_light",
    "model": "c3-light",
    "node_role": "leaf"
  }
}
```

| `Device` 字段 | 进文件？ | 重启后 |
|---|---|---|
| `mac` | 是（作为 key） | 恢复 |
| `name` | 是 | 恢复 → **slug 立刻正确** |
| `model` | 是 | 恢复 |
| `node_role` | 是 | 恢复 |
| `online` | **否** | 恒为 `False`，等 S3 的 `device` 行或第一条状态 |
| `rssi` / `hop` / `via` | **否** | `None` / `""` |
| `caps` | **否** | 空，等第一条带 `caps` 的上报 |
| `last_state` | **否** | 空 → 见下面的警告 |
| `discovered` / `discovery_sig` | 否 | `False` / `""`，重启后必然重发一次 discovery |

> **⚠️ Bridge 重启后第一条状态可能是"残缺"的**
>
> `last_state` 不持久化。所以重启后：
>
> 1. `dev.last_state = {}`
> 2. 第一条上报来了，比如被 160 字节挤掉了 `caps` 的
>    `{"switch":"ON","brightness":128,"color_temp":370}`
> 3. `merged = {} + payload` = 只有这 3 个字段
> 4. Bridge 把这个**比之前少**的 JSON 以 retained 发到 `<slug>/state`，
>    **覆盖掉 broker 上那份完整的旧 retained 消息**
>
> 之后每条上报都会往上叠，几轮之后就补齐了。但在那之前，HA 里可能有几个实体
> 短暂显示 unknown。
>
> 缓解办法：Bridge 启动时发的 `{"type":"list"}` 会让 S3 回一遍
> `device`/`info`（恢复 name/model/rssi/hop），但 **`list` 不会重放设备状态**——
> S3 不缓存 payload（见 [architecture.md](architecture.md) 的"状态存在哪"）。
> 状态只能等设备下一次上报。要快一点，可以在重启后手工 toggle 一下设备，
> 或者把设备的上报间隔调短（device 仓库 `docs/kconfig.md`）。

**`_save_devices()` 在每个 `device` 行都会调一次**，也就是每个设备每 30 s 一次同步写。
设备多的时候（比如 32 个）大约每秒一次 `write_text()`。对 SSD/eMMC 无所谓，
但如果你跑在 SD 卡的树莓派上、又有很多设备，这是个可以优化的点
（内容没变时其实不需要写）。

注意 `_save_devices()` 顺带调了 `_publish_device_list()`，所以
`bridge/devices` 的刷新频率和 `devices.json` 的写入频率是绑定的。

`bridge/devices` 是 retained 的 JSON 数组：

```python
lst = [{"mac": d.mac, "name": d.name or self._slug(d), "model": d.model,
        "online": d.online, "rssi": d.rssi, "hop": d.hop, "via": d.via,
        "node_role": d.node_role} for d in self.devices.values()]
```

注意 `"name": d.name or self._slug(d)`——没名字的设备在这里显示为它的 slug（紧凑 MAC），
不是空串。

---

## 11. 串口自动探测

```python
def autodetect_port() -> Optional[str]:
    keywords = ("esp32", "espressif", "cp210", "ch340", "usb jtag", "usb serial")
    for p in list_ports.comports():
        blob = f"{p.description} {p.manufacturer} {p.product}".lower()
        if any(k in blob for k in keywords):
            return p.device
    for candidate in ("/dev/ttyACM0", "/dev/ttyUSB0", "/dev/serial/by-id"):
        path = Path(candidate)
        if path.is_dir():
            kids = sorted(path.iterdir())
            if kids:
                return str(kids[0])
        if path.exists():
            return str(path)
    return None
```

两级：

1. 遍历 `pyserial` 枚举到的端口，把 `description` + `manufacturer` + `product`
   拼成一个字符串，匹配 6 个关键词。**第一个命中就返回。**
2. 没命中就按顺序试 `/dev/ttyACM0` → `/dev/ttyUSB0` → `/dev/serial/by-id`
   下的第一项（字典序）。

> **⚠️ 有多个 ESP 板子时别用自动探测**
>
> "第一个命中就返回"意味着如果你同时插着一块 S3 协调器和一块 C3 开发板
> （比如正在烧固件），自动探测可能选中 C3。C3 不会回 `hello`，
> Bridge 就会一直显示 `bridge/state = online`（因为 `_on_mqtt_connect` 里无条件发了
> `online`）但永远收不到设备。
>
> 生产部署**永远显式指定 `--port`**，而且用稳定路径：
>
> ```bash
> ls -l /dev/serial/by-id/
> # usb-Espressif_USB_JTAG_serial_debug_unit_AA:BB:CC:DD:EE:FF-if00 -> ../../ttyACM0
> --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AA:BB:CC:DD:EE:FF-if00
> ```
>
> `by-id` 路径包含 MAC，插拔和重启都不会变；`/dev/ttyACM0` 会变。

探测失败时 `main()` 直接退出：

```python
port = args.port or autodetect_port()
if not port:
    LOG.error("no serial port found; pass --port /dev/ttyACM0")
    return 2
```

**退出码 2**，方便 systemd / Docker 识别。

---

## 12. 命令行参数全表

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port` | `""`（自动探测） | S3 的串口设备。生产环境显式给，用 `by-id` 路径 |
| `--baud` | `115200` | USB Serial/JTAG 是 CDC，波特率**其实无所谓**（不是真 UART），留着是为了兼容用外置 USB-UART 芯片的板子 |
| `--mqtt-host` | `localhost` | Broker 地址 |
| `--mqtt-port` | `1883` | Broker 端口。**没有 TLS 支持**，要 TLS 得自己改 `mqtt.tls_set()` |
| `--mqtt-user` | `""` | 空 = 匿名。空字符串会被 `main()` 转成 `None` |
| `--mqtt-pass` | `""` | 配合上面 |
| `--base-topic` | `espnow2mqtt` | 所有主题的前缀。**必须和 HA 集成里配的一致** |
| `--discovery-prefix` | `homeassistant` | MQTT Discovery 前缀，只在 `--ha-discovery` 开启时用到 |
| `--ha-discovery` | **off** | 见 [§9](#9-ha_discovery为什么默认关) |
| `--no-ha-discovery` | off | 隐藏的历史别名，会否决 `--ha-discovery` |
| `--devices` | `data/devices.json` | 设备名缓存路径。Add-on 里传的是 `/data/devices.json`（持久卷） |
| `-v` / `--verbose` | off | `DEBUG` 级日志。**调协议问题必开**，否则看不到非 JSON 行和 `pong`/`ack` |

`--base-topic` 和 `--discovery-prefix` 都会 `.rstrip("/")`，所以
`--base-topic espnow2mqtt/` 和 `espnow2mqtt` 等价。

MQTT client id 是 `f"espnow2mqtt-{int(time.time())}"`——**每次启动都不一样**。
好处是不会和自己的旧连接冲突（broker 不会因为 client id 重复踢掉新连接）；
坏处是 broker 的连接日志里会不断出现新的 client id，而且**没有 persistent session**，
所以 Bridge 离线期间的消息不会补发（对本系统无影响，命令是即时的）。

---

## 13. 已知的并发弱点

和 [coordinator.md](coordinator.md) 里的 `s_peers` 一样，这里也把实情写清楚。

### 13.1 `self.devices` 没有锁

`self.devices` 这个 dict 被两个线程碰：

| 线程 | 操作 |
|---|---|
| serial | `_ensure_device()` 插入新 key；改 `Device` 的字段；`_save_devices()` 遍历 |
| paho | `_find_by_slug()` 遍历；`_on_mqtt_connect` 里 `for dev in self.devices.values()` 遍历 |
| 主线程 | 启动时 `_load_devices()` 插入（在 serial 线程起来**之前**，所以不冲突） |

| 竞态 | 后果 | 严重性 |
|---|---|---|
| serial 插入新 MAC 时 paho 正在 `for d in self.devices.values()` | CPython 会抛 `RuntimeError: dictionary changed size during iteration` | **中**。会让 `_on_mqtt_message` 里那一次命令丢掉，或者 `_on_mqtt_connect` 的 discovery 重放中断。paho 会捕获回调里的异常继续跑，所以进程不会死 |
| paho 读 `dev.name` 时 serial 正在写 | 读到新值或旧值，都是合法值 | **低**。`str` 赋值在 CPython 里是原子的 |
| 两个线程同时改同一个 `Device` 的不同字段 | 无 | **无**。属性赋值各自独立 |

实践中很少撞上，因为新设备加入是稀疏事件（配网时才有），而 paho 的遍历也不频繁。
但它是个真实的 bug，正确修法是给 `self.devices` 加一把
`threading.RLock`（或者复用 `self._lock`，它现在的临界区极短，扩大范围代价不大），
在所有遍历处用 `list(self.devices.values())` 先做快照。

### 13.2 `self.cmd_id` 没有锁

```python
cid = self.cmd_id
self.cmd_id += 1
```

这是经典的非原子读-改-写。但**实践上安全**，因为 `_on_mqtt_message` 只在
paho 的那一个网络线程里被调用（paho 的 `loop_start()` 只起一个线程，回调是串行的）。
只要不加第二个写命令的线程，就不会有两条命令拿到同一个 `id`。

如果真拿到了同一个 `id`，后果是 S3 侧 `(dest, cmd_id)` 的 ACK 匹配会把两条命令
当成一条，第二条可能被第一条的 ACK 提前"确认"掉，于是第二条的重传停止——
如果它实际上没送到，就静默丢了。

### 13.3 串口断开不会自愈

见 [§5](#5-串口读取_serial_loop-与-_handle_serial)。`_serial_loop` 的 `except`
只 `sleep(1)` 继续，但 `self.ser` 已经是个坏掉的句柄了，后续 `read()` 会一直抛。
**日志会以每秒一条的频率刷 `serial error:`**，这是"USB 掉了"的明确信号。

所以部署时必须有外部重启机制：

| 跑法 | 机制 |
|---|---|
| HA Add-on | `startup: application` + HA supervisor 自动重启 |
| Docker | `restart: unless-stopped`（`docker-compose.yml` 里已配） |
| 裸机 | 自己写 systemd unit，`Restart=always` |

见 [deployment.md](deployment.md)。

---

## 相关文档

- [architecture.md](architecture.md) — Bridge 在整条链路里的位置、完整的上下行路径
- [coordinator.md](coordinator.md) — USB 另一头的 S3 固件
- [usb-protocol.md](usb-protocol.md) — Bridge 读写的每一种 JSON 行
- [mqtt.md](mqtt.md) — Bridge 发布/订阅的每一个主题
- [troubleshooting.md](troubleshooting.md) — 按症状排查
- device 仓库 `docs/reporting.md` — 160 字节预算和降级顺序，理解 §6.1 的前提
