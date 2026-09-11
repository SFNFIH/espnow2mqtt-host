# 主机快速开始（S3 + Bridge）

1. 烧录协调器：

```bash
cd firmware/coordinator
idf.py set-target esp32s3
idf.py build flash
```

2. 插入 USB，确认串口（如 `/dev/ttyACM0`）

3. 启动 Bridge：

```bash
pip install -r requirements.txt
python -m espnow2mqtt --port /dev/ttyACM0 --mqtt-host <broker> -v
```

4. 安装 [espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)，再烧 [C3 设备](https://github.com/SFNFIH/espnow2mqtt-device)
