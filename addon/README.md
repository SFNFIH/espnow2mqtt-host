# ESP-NOW 2 MQTT — Home Assistant Add-on

把 [espnow2mqtt-bridge](https://github.com/SFNFIH/espnow2mqtt-bridge) 以 Add-on 形式跑在 HA OS / Supervised 上。

## 作用

- 打开协调器 USB 串口
- 连接 HA 的 MQTT broker
- 发布设备状态、接收控制与 `permit_join`

实体请用自定义集成：[espnow2mqtt-ha](https://github.com/SFNFIH/espnow2mqtt-ha)（不要依赖本 Add-on 开 HA Discovery）。

## 安装提示

1. 将本仓库作为本地 Add-on 仓库，或拷贝 `addon/` 相关文件到 HA 的 `addons/` 目录（构建上下文为**仓库根目录**，见 `Dockerfile`）
2. 在 Add-on 选项中填写串口设备与 MQTT 参数
3. 启动后确认 MQTT 主题 `espnow2mqtt/bridge/state` 为 `online`
4. 安装并配置 HA 集成，base topic 保持一致

更完整的 Bridge 说明见仓库根目录 [README.md](../README.md)。
