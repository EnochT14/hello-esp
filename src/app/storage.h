// Storage layer: the only module allowed to touch the filesystem.
//
// Design notes (these exist because of four boot-loop incidents):
//
//  1. Every mutable file is replaced atomically: write <path>.tmp, then swap.
//     Nothing is ever rewritten in place, so a power cut can only lose the
//     newest version, never corrupt the directory metadata of the old one.
//
//  2. The hot counters live in ONE crc-protected blob (/state.bin) instead of
//     six separate files. That cuts steady-state flash writes from ~12/min to
//     ~1 per commit interval, which is both the wear problem and the
//     corruption-window problem.
//
//  3. LittleFS can mount successfully while its directory metadata is torn.
//     Reads then work but allocating writes fault inside lfs_alloc() with an
//     unhandled CPU exception that cannot be caught. So we never try to
//     "detect corruption by writing": we watch for the SYMPTOM (a boot that
//     never completes) using an RTC_NOINIT_ATTR stamp, and on two
//     consecutive failed boots we latch the filesystem read-only. A read-only
//     device still serves every page.
//
//  4. Every write is checked. A short write, a failed rename, or a size
//     mismatch latches fsx::suspendWrites() so one bad write cannot cascade
//     into a loop of them.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <functional>

namespace fsx {

// --- mount / health -------------------------------------------------------

// Must be called as the first statement of setup(), before any filesystem
// access, so the outcome of the previous boot is known before we write.
void armBootGuard();

// Call once setup() has completed. The device then counts as healthy.
void disarmBootGuard();

// Consecutive previous boots that never reached the end of setup().
unsigned bootFailures();

// Mounts the filesystem, formatting only if the partition is genuinely
// unusable (bad superblock). Returns true when a usable mount exists.
bool begin();

// True when the filesystem is mounted AND believed writable. Callers on the
// write path should bail out early when this is false rather than discovering
// it via an error return.
bool writable();

// True when writes are latched off (panic loop detected, or a write failed).
// The site keeps serving reads in this state.
bool writesSuspended();

// Short human-readable health string for /admin/info and the OLED.
const char* health();

// Total / used bytes.
size_t totalBytes();
size_t usedBytes();

// Records that a write failed, latching read-only mode. Idempotent.
void suspendWrites(const char* why);

// --- primitives -----------------------------------------------------------

bool exists(const char* path);
bool remove(const char* path);
bool rename(const char* from, const char* to);

// Creates a directory and any missing parents ("/stats/weekly/2026").
bool mkdirs(const char* dir);

// Atomic whole-file replace. Returns false (and latches read-only mode on a
// hard failure) instead of throwing. `data` need not be NUL-terminated.
bool writeFile(const char* path, const void* data, size_t len);
bool writeText(const char* path, const char* s);

// Appends without rewriting the file. Used for CSV logs and the guestbook,
// where the existing content is always valid. Returns false on failure.
bool append(const char* path, const void* data, size_t len);
bool appendText(const char* path, const char* s);

// Reads a whole file, truncating at maxLen. Returns false if the file is
// missing or larger than maxLen.
bool readAll(const char* path, std::string& out, size_t maxLen);

// Reads the first line, stripping a trailing CR/LF and spaces.
bool readLine(const char* path, char* buf, size_t bufLen);

// Convenience: read a small text file and trim it.
bool readTrimmed(const char* path, char* buf, size_t bufLen);

// --- crc-protected blob ---------------------------------------------------

// Writes `len` bytes as: magic, version, length, payload, crc32. Atomic, and
// the crc catches truncation, bit rot and partial writes that the filesystem
// itself happily accepted.
bool writeBlob(const char* path, uint8_t version, const void* data, size_t len);

// Reads a blob written by writeBlob. On a crc/header mismatch it transparently
// retries <path>.bak, so a torn write still leaves one good generation.
// Returns false only when neither generation is usable.
bool readBlob(const char* path, uint8_t* version, void* out, size_t len);

uint32_t crc32(const void* data, size_t len);

// --- iteration (backup + pruning) -----------------------------------------

// Depth-first walk yielding every regular file as "/a/b/c". Return false from
// `fn` to stop the walk early. Symlinks do not exist in LittleFS.
void walk(const std::function<bool(const char* absPath, const char* base, size_t size)>& fn);

// Direct FS handle, for the rare caller that needs openNextFile() directly.
fs::FS& vol();

}  // namespace fsx