// Counters, period aggregation, Hall-of-Fame records and the sensor log.
//
// Everything volatile lives in RAM and is committed as ONE crc-protected blob
// (/state.bin) on a fixed-width, explicitly little-endian layout. v1.5 spread
// the same numbers across six independently-written files; consolidating them
// is what makes the commit atomic and cuts steady-state flash writes by about
// an order of magnitude.
//
// Period archives (/stats/weekly/YYYY/YYYY-Www.json etc.) keep the exact v1.5
// JSON shape, because the history pages already parse them and R2 already
// holds a year of them.
#pragma once

#include <Arduino.h>

namespace state {

// --- period accumulator ---------------------------------------------------
struct Period {
  uint32_t visitors    = 0;
  uint32_t guestbook   = 0;
  uint32_t peakReqs    = 0;
  float    tempMin     = 1000.0f;
  float    tempMax     = -1000.0f;
  double   tempSum     = 0.0;
  float    humMin      = 1000.0f;
  float    humMax      = -1000.0f;
  double   humSum      = 0.0;
  uint16_t co2Min      = 0xFFFF;
  uint16_t co2Max      = 0;
  uint32_t co2Sum      = 0;
  uint16_t vocMin      = 0xFFFF;
  uint16_t vocMax      = 0;
  uint32_t vocSum      = 0;
  uint32_t samples     = 0;
  uint32_t startedUnix = 0;

  void reset();
  void addSample(float tempC, float hum, uint16_t co2, uint16_t voc, uint32_t reqs);
  // Exact v1.5 periodToJson(), minus the Shelly-only energy_wh field.
  void toJson(char* out, size_t cap, const char* label) const;
};

// --- hall of fame ---------------------------------------------------------
// `at` is dual-format by design: moment-level records store a full ISO-8601
// stamp (YYYY-MM-DDTHH:MM:SS±HHMM); the busiest-day record stores only
// YYYY-MM-DD. The frontend parses both.
struct Record {
  float value = 0;
  char  at[28] = "";
  bool  set    = false;
};

// --- lifecycle ------------------------------------------------------------

// Loads /state.bin, or seeds from the v1.5 file set when it is absent (first
// boot after the rewrite). Safe to call before the clock is synced.
void begin();

// Flushes RAM state to /state.bin. `minIntervalMs` debounces non-forced
// commits so a burst of visitors does not turn into a burst of writes.
bool commit(bool force, unsigned long minIntervalMs = 20000);

// True when there are unsaved changes.
bool dirty();

// NTP state, cached here so the rest of the code has one place to ask.
void setClockOk(bool ok);
bool clockOk();

// --- visitors ---
uint32_t visitors();
uint32_t dailyVisitors();
void countVisit(const char* countryAlpha2);
uint32_t countriesTracked();
// Admin reset. Clears the lifetime counter, the period counters and the
// "busiest day" record in one commit so nothing can disagree afterwards.
void resetVisitors();

// --- guestbook counters ---
uint32_t guestbookApproved();
uint32_t guestbookPending();
uint32_t guestbookAll();
// Deltas, so the guestbook module can report what a moderation batch changed.
void adjustGuestbook(int32_t pending, int32_t approved, int32_t all);

// --- periods ---
void rollPeriods();
Period& week();
Period& month();
Period& year();
const char* weekLabel();
const char* monthLabel();
const char* yearLabel();

// --- records ---
void updateRecords(float tempC, uint16_t co2);
void noteUptime(float days, const char* when);
void recordsJson(char* out, size_t cap);

// --- dumsor (power outage tracking) ---
void dumsorBootCheck();
void dumsorHeartbeat();
uint32_t outagesTotal();
uint32_t outagesMonth();
uint64_t outageSecondsTotal();
uint64_t outageSecondsMonth();
uint32_t lastOutageSecs();
int64_t  lastOutageAt();

// --- CSV sensor log -------------------------------------------------------
// One row per interval to /logs/YYYY/YYYY-MM-DD.csv. The 17-column v1.5 header
// is preserved verbatim (power_w is left empty) so existing charts, which index
// columns positionally, keep working.
void logSample(float tempC, float tempF, float hum, float pressureHpa,
               float altitudeFt, uint16_t co2, uint16_t voc, float heatC,
               int rssi, uint32_t requestsThisInterval);

// Deletes log CSVs older than `days` and any leftover .tmp/.bak fragments.
// Returns the number of files removed.
int pruneLogs(int days);

// --- request counter for the current interval ------------------------------
void countRequest(uint32_t peakThisMinute);
uint32_t requestsThisInterval();
void resetIntervalRequests();

}  // namespace state