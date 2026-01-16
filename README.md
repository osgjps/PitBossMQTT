This is a Bluetooth Low Energy proxy to MQTT gateway designed for PitBoss Smokers, specificially starting out with the Pro 850 DX.

Why?  I wanted integration into Home Assistant.  The smoker is too far from my HA server to natively speak BLE and using the BLE proxy for ESPHome on ESP32 hardware is flakey at best and wouldn't keep a stable connection so I couldn't use the HA integration directly.

This code should work on any ESP32 that has BLE hardware just by changing the board type in platformio.ini.  I chose a tiny ESP32-C3 since they're tiny and this would be a single function device.

The Wifi and MQTT information need to be added as environment variables before running "pio run" to build.
