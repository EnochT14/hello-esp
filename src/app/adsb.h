#ifndef APP_ADSB_H
#define APP_ADSB_H

#include <Arduino.h>

// Compact ADS-B forwarder.
//
// The chip does the least work that still counts: fetch the local receiver's
// aircraft.json once a poll interval and forward a trimmed extract. Filtering,
// trail tracking, stale expiry and JSON shaping all happen in the Cloudflare
// Worker, which is where they are cheap and shared by every viewer.
//
// The device keeps no per-aircraft state. It holds only the last payload it
// sent, so /adsb.json can answer from the LAN without waiting for a poll.
namespace adsb {

void begin();

// Poll if due. Cheap and non-blocking when the interval has not elapsed.
void tick();

// True once at least one poll has succeeded.
bool live();

// The most recent compact payload, or an empty fleet before the first success.
// Valid until the next successful poll.
const char* lastJson();

}  // namespace adsb

#endif  // APP_ADSB_H