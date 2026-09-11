# ESP-NOW 2 MQTT — Bridge

Host-side **USB Serial ↔ MQTT** bridge for the ESP32-S3 coordinator.

Pairs with:
- Firmware (ESP-IDF component `en2m`, Matter-style interaction layer): https://github.com/SFNFIH/espnow2mqtt-firmware
- Home Assistant integration: https://github.com/SFNFIH/espnow2mqtt-ha
- Umbrella: https://github.com/SFNFIH/espnow2mqtt

## Run locally

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python -m espnow2mqtt --port /dev/ttyACM0 --mqtt-host homeassistant.local -v
```

## Docker

```bash
docker compose up -d --build
```

## Home Assistant Add-on

See `addon/`. Default leaves HA MQTT Discovery off — use **espnow2mqtt-ha** for entities.
