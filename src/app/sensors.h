// The two sensors the build actually has: BME280 (temperature / humidity /
// pressure) and CCS811 (eCO2 / TVOC), plus the on-board SH1106 OLED.
//
// Both readings go through a sanity gate before they are allowed near the rest
// of the system. An I2C bus glitch on a shared two-wire bus otherwise produces
// physically impossible values (355F, 99000ppm) that then poison the log, the
// charts and the Hall of Fame permanently.
#pragma once

#include <Arduino.h>

namespace sensors {

// BME280 datasheet operating range. Outside this, the reading is a bus fault.
constexpr float kTempMinC   = -40.0f;
constexpr float kTempMaxC   =  85.0f;
constexpr float kHumMin     =   0.0f;
constexpr float kHumMax     = 100.0f;
constexpr float kPressMin   = 300.0f;
constexpr float kPressMax   = 1100.0f;
// CCS811 register maximum is 32768ppm; beyond that is a bus fault.
constexpr uint16_t kCo2Max   = 32768;
constexpr uint16_t kVocMax   = 32768;

struct Reading {
  float    tempC     = NAN;
  float    tempF     = NAN;
  float    humidity  = NAN;
  float    pressure  = 1013.25f;
  float    altitude  = 0.0f;
  uint16_t co2       = 0;
  uint16_t voc       = 0;
  float    heatIndex = NAN;
  bool     bmeOk     = false;
  bool     ccsOk     = false;
};

void begin();

// Samples both sensors. Cheap on failure: a wedged bus is only re-seated once
// per cooldown window rather than on every 5-minute tick.
void read(Reading* out);

// True once a sensor has produced one good reading. Used to distinguish
// "no data yet" from "sensor is broken" on the dashboard.
bool bmeHealthy();
bool ccsHealthy();

// Recovers a wedged I2C bus (nine SCL pulses to release a slave that is holding
// SDA low), then re-initialises. Returns true if the bus came back.
bool recoverBus();

// --- sensor health / retirement -------------------------------------------
// A sensor that never produces a good read is retired after a few hours so the
// dashboard can say so plainly instead of showing seeded values forever.
void noteCycleResult(bool bmeOkNow, bool ccsOkNow);
bool bmeRetired();
bool ccsRetired();
// Clears the retired latch (used after a physical repair).
void clearRetired();

// --- display --------------------------------------------------------------
void displayBegin();
void displayCycle();
bool displayOk();

}  // namespace sensors