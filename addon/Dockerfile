ARG BUILD_FROM=ghcr.io/home-assistant/amd64-base-python:3.12-alpine3.20
FROM ${BUILD_FROM}

WORKDIR /app

# Build from espnow2mqtt-host repository root:
#   docker build -f addon/Dockerfile .
COPY requirements.txt /app/requirements.txt
RUN pip3 install --no-cache-dir -r /app/requirements.txt
COPY espnow2mqtt /app/espnow2mqtt
COPY addon/run.sh /run.sh
RUN chmod a+x /run.sh

CMD ["/run.sh"]
