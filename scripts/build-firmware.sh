#!/usr/bin/env bash
# Build the Pico firmware from source and copy the .uf2 into the CLI package.
# Needs arduino-cli (macOS: brew install arduino-cli). Installs the board core and
# libraries below the first time (pinned versions, so the build matches the release).
set -euo pipefail
cd "$(dirname "$0")/.."
FQBN="rp2040:rp2040:rpipico:usbstack=tinyusb,freq=120"
CORE="rp2040:rp2040@6.4.0"
LIBS=("Pico PIO USB@0.7.3" "Adafruit TinyUSB Library@3.7.7")
INDEX="https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json"

if ! arduino-cli core list | grep -q "^rp2040:rp2040 *6.4.0"; then
  echo "installing board core $CORE..."
  arduino-cli config add board_manager.additional_urls "$INDEX" 2>/dev/null || true
  arduino-cli core update-index
  arduino-cli core install "$CORE"
fi
for lib in "${LIBS[@]}"; do
  name="${lib%@*}"; ver="${lib#*@}"
  if ! arduino-cli lib list | grep -q "^$name *$ver"; then
    echo "installing library $lib..."
    arduino-cli lib install "$lib"
  fi
done

arduino-cli compile --fqbn "$FQBN" --build-path build/cache --output-dir build firmware/keycrypt
cp build/keycrypt.ino.uf2 keycrypt_cli/firmware/keycrypt.uf2
echo "firmware -> keycrypt_cli/firmware/keycrypt.uf2"
