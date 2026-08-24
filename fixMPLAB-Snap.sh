#!/bin/bash
# set to avr mode
# https://arduino-craft-corner.de/index.php/2025/04/16/snap-debugging-for-the-masses/
AVRDUDE="$HOME/.platformio/packages/tool-avrdude/avrdude"
AVRDUDE_CONF="$HOME/.platformio/packages/tool-avrdude/avrdude.conf"

"$AVRDUDE" \
    -C "$AVRDUDE_CONF" \
    -c snap_updi \
    -p attiny412 \
    -P usb \
    -x mode=avr