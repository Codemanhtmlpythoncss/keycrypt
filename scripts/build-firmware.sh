#!/usr/bin/env bash
# Build the Pico firmware and copy the .uf2 into the CLI package.
# Needs arduino-cli with the rp2040:rp2040 core (see README "Build from source").
set -euo pipefail
cd "$(dirname "$0")/.."
FQBN="rp2040:rp2040:rpipico:usbstack=tinyusb,freq=120"
arduino-cli compile --fqbn "$FQBN" --build-path build/cache --output-dir build firmware/keycrypt
cp build/keycrypt.ino.uf2 keycrypt_cli/firmware/keycrypt.uf2
echo "firmware -> keycrypt_cli/firmware/keycrypt.uf2"
