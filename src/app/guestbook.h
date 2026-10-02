// Guestbook: a flat CSV with a 7-field, unescaped layout.
//
//   time,country,name,message,id,reply_to,status
//
// This is byte-compatible with schema v3 of the previous firmware, because the
// file is the only record of every message ever posted and there is no way to
// re-derive it. status is one digit: 0 pending, 1 approved, 2 denied, 3
// tombstone.
//
// Three invariants the format depends on, and which the writer is responsible
// for keeping:
//   * exactly 6 commas per row, so readers can index fields without a CSV
//     parser;
//   * no comma, quote or backslash inside name/message (sanitised on write);
//   * rows are appended, never reordered, so reply chains resolve in one
//     forward pass.
#pragma once

#include <Arduino.h>

namespace guestbook {

struct Entry {
  char time[32];
  char country[4];
  char name[34];
  char message[208];
  char id[10];
  char replyTo[10];
  char status;
};

// Loads guestbook.csv and recomputes the approval/pending/all counters, which
// live in the state blob rather than being derived on every request.
void begin();

// Rows matching the schema and public content only.
uint16_t approvedCount();
uint16_t allCount();
uint16_t pendingCount();

// True when the reply_to chain is valid: parent exists, is approved, and is at
// most one level deep.
bool validReplyParent(const char* replyToId);

// Appends a new pending entry. Returns false (and sets *err) on rejection.
// `err` receives a short reason suitable for an HTTP body.
bool submit(const char* name, const char* message, const char* replyToId,
            const char* country, const char** err);

// Applies "<idx>:<status>,..." to the moderation queue. Returns the number of
// rows actually changed.
int moderateBatch(const char* ops, uint16_t* applied);

// Rejects a new submission from this IP for one hour.
bool rateLimited(const char* ip);
void noteSubmitAttempt(const char* ip);

// Guard against an unbounded moderation queue filling the 1.3MB partition.
uint16_t pendingLimitReached();

}  // namespace guestbook