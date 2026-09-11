#!/usr/bin/with-contenv bashio
set -euo pipefail

PORT=$(bashio::config 'port')
BAUD=$(bashio::config 'baud')
BASE=$(bashio::config 'base_topic')
HOST=$(bashio::config 'mqtt_host')
MPORT=$(bashio::config 'mqtt_port')
USER=$(bashio::config 'mqtt_user')
PASS=$(bashio::config 'mqtt_password')
DISC=$(bashio::config 'ha_discovery')

# Prefer Home Assistant's Mosquitto when host left empty
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

ARGS=(
  --port "$PORT"
  --baud "$BAUD"
  --base-topic "$BASE"
  --mqtt-host "$HOST"
  --mqtt-port "$MPORT"
  --devices /data/devices.json
)

if [[ -n "${USER:-}" ]]; then
  ARGS+=(--mqtt-user "$USER" --mqtt-pass "$PASS")
fi

if [[ "$DISC" == "true" ]]; then
  ARGS+=(--ha-discovery)
fi

bashio::log.info "Starting espnow2mqtt bridge on ${PORT} → mqtt://${HOST}:${MPORT} (topic ${BASE})"
exec python3 -m espnow2mqtt "${ARGS[@]}"
