// Build-wide constants.
#pragma once

#define FIRMWARE_VERSION_STR "3.0"

// Pins: generic ESP32 WROOM-32 dev board (DOIT ESP32 DEVKIT V1, 4MB flash).
// Neither I2C pin is a boot-strapping pin, so a sensor holding the bus low at
// power-up cannot trip the 1.8V flash mode the old GPIO12 layout was prone to.
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define PIN_LED     2   // onboard blue LED, active low