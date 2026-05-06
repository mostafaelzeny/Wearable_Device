#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

sudo install -m 644 mosquitto-websockets.conf /etc/mosquitto/conf.d/websockets.conf
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto

echo "Mosquitto is configured and running with MQTT on 1883 and WebSockets on 9001."
echo "Test MQTT with:"
echo "  mosquitto_sub -h localhost -t wearable/vitals"
