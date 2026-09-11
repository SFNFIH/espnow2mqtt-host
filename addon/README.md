# ESP-NOW 2 MQTT — HA Add-on（主机 Bridge）

本 Add-on 属于 **[espnow2mqtt-host](https://github.com/SFNFIH/espnow2mqtt-host)**：在 HA OS 上跑 USB↔MQTT Bridge。

- 串口连的是 **ESP32-S3 协调器**（同仓库 `firmware/coordinator`）
- 实体请用 **[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)**，不要开 HA MQTT Discovery
- C3 设备固件在 **[espnow2mqtt-device](https://github.com/SFNFIH/espnow2mqtt-device)**

构建上下文为 **host 仓库根目录**（见 `Dockerfile`）。完整说明见根目录 [README.md](../README.md)。
