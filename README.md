# HelloESP

[![HelloESP status](https://esp.ecobbina.work/status.svg)](https://esp.ecobbina.work)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/license/mit/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-Arduino-orange)](https://platformio.org/)
[![GitHub stars](https://img.shields.io/github/stars/EnochT14/hello-esp?style=social)](https://github.com/EnochT14/hello-esp/stargazers)

A public website running entirely on a single ESP32 with 520 KB of RAM. Every page, every sensor reading, every guestbook entry is served by the microcontroller itself. There is no backend server.

**Live at [esp.ecobbina.work](https://esp.ecobbina.work)**

![HelloESP in its display frame](.github/assets/hero.jpg?v=2)

---

## Why?

Because it's fun to see how far a $10 microcontroller can go. The last version ran until 2023 on an ESP32 behind a Cloudflare tunnel. It eventually went offline and the domain lapsed.

Years later I came back to it. The internet had gotten heavier in the meantime: more of everything, most of it wasteful. This whole website weighs less than a single phone wallpaper. I wanted to see what a single small chip could still do against all that. The domain was available again, which felt like permission.

This time it stays up. If it goes down, I fix it. That's the point now.

## How it works

The ESP32 holds a persistent outbound WebSocket to a Cloudflare Worker. When a browser hits `helloesp.com`, the Worker relays the request over that socket and streams the response back. The ESP never accepts an inbound TCP connection from the internet, only from the LAN.

```
 Browser ──HTTPS──▶ Cloudflare Worker ──WSS──▶ ESP32 (on your home network)
                         ▲                        │
                         └────── response ────────┘
```

Responses larger than a single WebSocket frame are chunked and base64-encoded. Admin endpoints return 404 through the relay and are only reachable on the LAN. A second shared secret (HMAC) can be enabled so a leaked Worker secret alone cannot impersonate the device.

The same WebSocket also carries **live push events** the other direction: every 15 seconds the device pushes sensor stats, and every tracked public request triggers a console event. The Worker fans these out to connected browsers via Server-Sent Events (`/_stream`), so the homepage ticks in real time without polling.

Without the Worker, the site still runs on LAN via mDNS at `http://helloesp.local`.

## Hardware

| Part | Qty | Role |
|---|---|---|
| Generic ESP32 WROOM-32 dev board (e.g. DOIT ESP32 DEVKIT V1, 4 MB flash) | 1 | MCU (classic ESP32, 520 KB RAM, dual-core Xtensa) + USB-serial, 3.3 V regulator, boot button, onboard blue LED on GPIO2 |
| GME12864-78 OLED (1.3", SH1106 128×64, I2C) | 1 | On-device display, 7 auto-rotating pages |
| GY-BME280-3.3V breakout | 1 | Temperature, humidity, barometric pressure |
| CJMCU-811 (CCS811) breakout | 1 | eCO₂, VOC |

There is no SD card: all persisted state (web assets, config, logs, stats) lives
in an on-chip LittleFS partition. No keypad either — display pages auto-rotate.
Wiring is just the two sensors + the OLED on the stock I²C bus.

**Power:** USB only (no battery). Any 1 A USB charger works; total draw averages
well under 1 W with brief spikes during WiFi association and flash writes. This
build has no UPS — and the "dumsor" tracker (below) deliberately watches the
grid cuts this makes visible.

### Wiring

| Signal | ESP32 Pin |
|---|---|
| I²C SDA / SCL (OLED, BME280, CCS811) | GPIO 21 / 22 (stock I²C pins) |
| OLED VCC / GND | 3.3 V / GND (some modules accept 5 V VCC; keep logic lines 3.3 V) |
| CCS811 WAKE | GND |
| Notification LED (onboard blue LED, active-low) | GPIO 2 |

I²C addresses: OLED `0x3C`, BME280 `0x76`, CCS811 `0x5A`. Neither GPIO21 nor
GPIO22 is a boot-strapping pin, so a sensor that holds its line low at
power-up can't trip the 1.8 V flash mode that plagued the old GPIO12 layout.

**No RTC.** This board has no battery-backed clock. Time comes from NTP after
boot, so anything persisted before the first sync is held back: the guestbook
rejects submissions until the clock is authoritative, and the dumsor tracker
(see below) measures outages from a LittleFS "last seen" marker written only
after the clock syncs.

## Features

**Frontend**
- Live dashboard: 12 sensor/system metrics, trend arrows, degraded-sensor indicators
- Real-time SSE updates (push every 15s from the device), with graceful fallback to 30s polling
- Live connection indicator (pulsing green dot) and a "Right now" request ticker on the homepage
- Historical CSV charts with day/week/month switcher; weekly/monthly/yearly aggregate archives
- Hall of Fame (lifetime extremes: peak CO₂, temp range, busiest day, longest uptime) and year-over-year monthly visitor deltas on `/history`
- Visitor country map, request-rate chart, changelog, photo carousel
- `/console` live feed: last 50 public requests with country flags, updated instantly via SSE
- Outdoor weather + air quality context (via Cloudflare Worker proxy to Open-Meteo): conditions with day/night-aware icons, feels-like, dewpoint, wind direction, pressure trend, AQI + PM2.5, atmospheric CO₂, UV index
- Guestbook with two-level reply threading, tombstone deletes, moderation queue, rate limiting
- Dark mode, responsive, SRI-pinned CDN scripts, no tracking

**Firmware**
- Full async HTTP server over WebSocket-relayed traffic
- Non-blocking WebSocket client with linear backoff (500 ms first retry, then 5 s × fails up to 30 s cap), WiFi reassociation on consecutive fails, and a 60 s wall-clock safety-net reboot if recovery stalls
- Atomic writes for all critical files (`tmp → bak → rename`)
- Period aggregation: weekly, monthly, yearly checkpoints
- CSV sensor logging every 5 minutes
- NTP with three-server failover and bounded boot retry; timestamps are correct only after the clock syncs, so pre-sync data is never persisted (guestbook submissions are rejected until then)
- Dumsor (power outage) tracker: the firmware writes an epoch "last seen" marker to LittleFS every 5 minutes; once per boot, after NTP lands, any gap longer than 90 seconds is recorded to `power_events.csv` (with outage start, restore time, duration; capped at 500 rows). `/stats` JSON and the on-device display expose the last outage plus monthly/total counts. Disable via `dumsor_tracking=false` in config
- WiFi runtime watchdog: reboots if disconnected >10 min
- Heap safety net: reboots at <30 KB free rather than hang
- Event-driven SSE push for sensor stats (15s) and console entries (on each request)
- Admin panel (LAN-only): OTA updates, file manager (with overwrite-confirm + gzip-pair tooltips), full storage backup/restore, R2 backup health + liveness test, SMTP2GO test email, self-test (with I²C bus scanner), sensor + error log viewers, device health sparklines (heap + RSSI), Worker link status, data management (reset counters / export state / repair monthly+yearly stats from weekly archives / drop out-of-spec CSV rows / fix truncated record timestamps), Durable Object storage explorer (read + delete-by-key for incident recovery, gated on the worker secret), maintenance mode toggle
- GME12864-78 OLED (SH1106 128×64, I²C) with 7 auto-rotating pages (no keypad): clock, environment, air quality (with CO₂ bar), stats, guestbook, dumsor, QR. Auto-rotation shifts pages every 10 s to prevent burn-in

**Edge**
- Cloudflare Worker (Durable Object) with per-IP rate limiting, 8 KB POST cap
- SSE fanout hub: multiple browser viewers, constant ESP load
- Maintenance mode with auto-expiring window and dedicated 503 page
- Auto-retry + pulsing indicator on all error pages (offline / timeout / maintenance)
- Cloudflare edge caching honors the device's `Cache-Control: max-age` headers so repeat visitors hit the edge instead of the chip
- Optional `worker_exclusive` mode: LAN visitors to public pages (homepage, guestbook, etc.) get redirected to the public site so they go through the Worker's edge cache rather than hitting the chip directly. `/admin` stays direct on LAN. Eliminates LAN-burst-vs-WS-write collisions
- Hourly outdoor-weather refresh cached in the Durable Object
- Inline guestbook translation via Workers AI: `@cf/meta/m2m100-1.2b` for primary translation, `@cf/meta/llama-3.2-1b-instruct` for source-language detection on diacritic-less Latin text and as a final fallback when m2m100 produces nothing usable. Per-(message-id, target-lang, text-hash) cached in DO storage so each unique pair costs at most one neuron set
- Embeddable live status badges at `/status.svg` (and `/status-wide.svg`)
- Optional HMAC challenge-response device auth
- Optional SMTP2GO integration for guestbook-pending alerts, dead-man's-switch (device silent >N hours), backup failures, and overdue-backup warnings
- Optional daily off-site backups to Cloudflare R2 with GFS rotation (7 daily + 4 weekly + 12 monthly + yearly) and sha256 integrity manifests; Worker also writes a daily full Durable Object snapshot to `state/do-snapshot/YYYY-MM-DD.json` (30-day retention) so DO state (translation caches, moderation queues) is recoverable independently if DO is ever wiped
- Optional Shelly Gen 2+ smart plug integration: energy column in CSV logs, per-period energy on `/history` cards, admin observability panel + self-test entry (homepage banner/chart were dropped in this build — no plug wired)
- Security headers, no-cache list for dynamic endpoints
- RSS feeds (`/changelog.rss`, `/guestbook.rss`), `sitemap.xml`, `robots.txt`, `.well-known/security.txt`

### Embeddable status badges

Live badges you can drop into your own README, blog, or status page. All variants pull from the Worker's cache, so embedding them puts zero load on the ESP no matter how many pages link them. Edge-cached 60 seconds. Keep working (showing "offline") even when the device is unreachable.

**Compact badges** (shields.io-compatible layout, 20px tall):

| Variant | Live preview | URL |
|---|---|---|
| Uptime *(default)* | ![](https://esp.ecobbina.work/status.svg) | `https://esp.ecobbina.work/status.svg` |
| Visit count | ![](https://esp.ecobbina.work/status.svg?metric=visits) | `https://esp.ecobbina.work/status.svg?metric=visits` |
| Current temperature | ![](https://esp.ecobbina.work/status.svg?metric=temp) | `https://esp.ecobbina.work/status.svg?metric=temp` |
| Live power draw | ![](https://esp.ecobbina.work/status.svg?metric=power) | `https://esp.ecobbina.work/status.svg?metric=power` |
| Online indicator | ![](https://esp.ecobbina.work/status.svg?metric=online) | `https://esp.ecobbina.work/status.svg?metric=online` |

**Wide stat card** (340×78, multi-line mini-dashboard):

![](https://esp.ecobbina.work/status-wide.svg)

`https://esp.ecobbina.work/status-wide.svg`

**Embedding**

Markdown:
```markdown
[![HelloESP status](https://esp.ecobbina.work/status.svg)](https://esp.ecobbina.work)
```

HTML:
```html
<a href="https://esp.ecobbina.work">
  <img src="https://esp.ecobbina.work/status.svg" alt="HelloESP status">
</a>
```

**State colors** (applies to both compact and wide):

| Color | Meaning |
|---|---|
| Blue | Live: device online, heard from within 45s |
| Gray | Offline or stale: no stats received in >2 minutes, or WS socket is gone |
| Orange | Maintenance: owner-declared downtime window |

**Accessibility**: every badge includes `<title>` text and `role="img"`, so screen readers announce the state rather than saying "image." The `aria-label` reflects the current value ("HelloESP: up 5d 3h" etc.).

**Cache policy**: `Cache-Control: public, max-age=60`. Most embedders don't need sub-minute freshness; plan accordingly if you do.

## Setup

### 1. Firmware

```bash
git clone https://github.com/EnochT14/hello-esp.git
cd helloesp
pio run -t upload
```

Requires [PlatformIO](https://platformio.org/).

**Upgrading from before firmware 1.4:** the boot-time CSV migrations were stripped in 1.4 to free flash. If your storage has guestbook data from before April 23, 2026, flash firmware 1.3 first to run the v1→v3 migrations, then upgrade. The chip checks at boot and halts with a display message if it detects stale data, so you'll know if you skipped this step.

### 2. Flash the filesystem (LittleFS)

The `data/` folder is **not** part of the firmware binary — it is uploaded to the
on-chip LittleFS partition, which is where the device serves web assets, reads
`config.txt`, and persists logs/stats:

```bash
pio run -t uploadfs     # flash the 1.9 MB LittleFS image
pio run -t upload       # flash the firmware (do this after uploadfs)
```

### 3. Config

Edit `config.txt` in `data/` (it ships alongside `config.example.txt`) and fill in:

```
wifi_ssid=YOUR_SSID
wifi_pass=YOUR_PASSWORD
admin_user=admin
admin_pass=your-admin-password
timezone=GMT0
```

Timezone is a POSIX TZ string. Common examples are listed in the file (for
Ghana: `GMT0`). Leave `worker_url`, `worker_key`, and `device_key` blank to run
LAN-only. `dumsor_tracking=true` (the default) enables the power-outage tracker.

Optional `worker_exclusive=true` redirects LAN public-page hits (`/`, `/guestbook`, etc.) to the Worker so they go through CF's edge cache. `/admin` always serves direct on LAN. Default off; flip to `true` only if your Worker is set up and you want LAN visitors to share the same cache layer as public visitors.

### 4. Cloudflare Worker (optional, for public access)

```bash
# Generate a shared secret; don't pick one by hand
openssl rand -hex 32
```

The same value goes in two places:
- `worker_key` in `config.txt`
- Worker secret `WORKER_SECRET`

Then deploy:

```bash
cd worker
wrangler deploy
wrangler secret put WORKER_SECRET   # paste the value from openssl
```

Add `worker_url` to `config.txt` (e.g. `esp.ecobbina.work`). The ESP will connect on next boot.

### 5. HMAC device auth (optional)

Adds a second secret so a leaked `WORKER_SECRET` alone cannot impersonate the device. On every reconnect, the Worker sends a random nonce and the ESP signs it with the device key.

```bash
openssl rand -hex 32
```

Set the same value as `device_key` in `config.txt` and:

```bash
wrangler secret put HMAC_SECRET
```

If either side is unset, HMAC is disabled and auth falls back to `WORKER_SECRET` only.

### 6. Email alerts via SMTP2GO (optional)

Configuring [SMTP2GO](https://smtp2go.com/) once unlocks every alert channel the Worker uses:

- Guestbook moderation notifications (throttled 1/5min)
- Dead-man's-switch: device silent for longer than `DEADMAN_HOURS` (default 6) + recovery email when it comes back
- Backup failures (throttled 1/hr) and overdue-backup warnings (>48 h since last success, 1/day)
- Manual test email from the admin panel

```bash
wrangler secret put SMTP2GO_KEY
wrangler secret put NOTIFY_EMAIL
wrangler secret put NOTIFY_FROM    # optional, e.g. "HelloESP <no-reply@yourdomain>"
wrangler secret put DEADMAN_HOURS  # optional, default 6, accepts fractional hours
```

The ESP never blocks on outbound HTTPS; all email sending lives on the Worker. If any required secret is unset, the corresponding alerts silently no-op.

### 7. Off-site backups to R2 (optional)

Full LittleFS snapshots go to Cloudflare R2 once a day at 4 AM local. Excludes `config.txt`, `*.tmp`/`*.bak` files, and `/logs`. Each backup lives at `state/YYYY-MM-DD/` with a sha256 manifest; `state/latest.json` is the atomic commit pointer.

```bash
wrangler r2 bucket create helloesp-backup
wrangler deploy
```

The binding is declared in `wrangler.toml`. If the binding isn't present, the Worker falls back to emailing the bundle as an attachment (if SMTP2GO is configured) so you don't silently lose backups.

**Rotation (Grandfather-Father-Son).** Keeps 7 daily + 4 weekly (Sundays) + 12 monthly (1st of month) + every Jan 1 forever. Anything outside those rules older than 8 days is pruned. A prefix guard refuses to delete anything not matching `state/YYYY-MM-DD/`.

**Storage math.** ~1.4 MB per snapshot × ~23 retained snapshots ≈ 32 MB/year. Well under R2's 10 GB free tier.

**Alerting.** No email on success. A single email fires if a backup fails (throttled 1/hr) or if no successful backup has been committed for >48 h (once per day).

### Restoring from an R2 backup

1. Re-upload the filesystem image: `pio run -t uploadfs` (data/ contents are deploy-recreatable and not in the backup).
2. Open the R2 bucket. Read `state/latest.json` for the most recent snapshot date.
3. Download every object under `state/{date}/`, preserving subdirectories (e.g. `stats/weekly/2026-W16.json` → `/stats/weekly/2026-W16.json`), and drop them in via the admin file manager (or re-uploadfs with a temporary data/ folder).
4. Recreate `config.txt` from your own records. Secrets aren't in the backup on purpose.

For single-file fixes, download one object from R2 and drop it in via the admin file manager. No full restore needed.

## Security

- `config.txt` is in `.gitignore`. Do not commit it. Treat `WORKER_SECRET` and `device_key` like passwords.
- Admin endpoints (`/admin`, `/admin/*`, `/_upload`, `/_ota`, `/guestbook/pending`, `/guestbook/moderate`) return 404 to any request arriving through the Worker. They are only reachable from the LAN.
- Admin Basic Auth uses a per-IP lockout (5 failed attempts → 10-minute block).
- Worker enforces 60 req/min per IP, caps POST bodies at 8 KB, and strips hop-by-hop headers.
- CDN scripts are pinned with SHA-384 SRI hashes.
- Guestbook input is stripped of control bytes and CSV-breaking characters before storage; output is JSON- and HTML-escaped on both sides.

## Limitations

If you're thinking about building your own, here's what to expect:

- **Concurrency is modest.** Roughly 5 simultaneous requests before latency is noticeable. Cloudflare's edge absorbs bursts of static content; dynamic endpoints hit the chip.
- **No HTTPS origin.** TLS terminates at the Worker. LAN access is HTTP only, so don't treat the admin panel as secure without a VPN or physical access.
- **Single point of failure.** One chip, one WiFi link, one flash chip. A power blip takes the site down until reboot (usually under 30 seconds).
- **Flash wear.** CSV logging every 5 min plus guestbook writes is a few hundred writes per day to the same flash die. The LittleFS partition is rated for ~100K erase cycles across its 1.9 MB; expect years of service, but a board that restarts in a tight loop is the main wear risk (the heap/net watchdogs prevent that).
- **Memory is tight.** 520 KB total, ~180 KB free at idle. Large responses or many concurrent frames can OOM; a heap watchdog reboots at under 30 KB free rather than hang.

## Build your own

The full stack is documented above: firmware, hardware, Worker, optional R2 backups, optional email alerts. Everything you need to run your own is here.

**Got one running?** Open a PR or issue with the link. I'll list notable builds in this section as they come in. Curious to see what people do with it.

## Repository layout

```
src/main.cpp                 Firmware (Arduino framework)
data/                        LittleFS contents: HTML, CSS, JS, images, favicon SVG
data/*.html.gz               Pre-gzipped HTML (auto-generated, see below)
worker/worker.js             Cloudflare Worker + Durable Object (relay, SSE, weather, badges)
worker/wrangler.toml         Worker config
scripts/gzip_assets.py       PlatformIO pre-build step that gzips data/*.html
platformio.ini               Build config + cppcheck flags
.github/workflows/           CI: PlatformIO build, CodeQL, Lighthouse
.github/dependabot.yml       Dependency update automation (GitHub Actions)
.github/ISSUE_TEMPLATE/      Issue chooser (security policy + discussions)
.github/assets/              README hero image
SECURITY.md                  Security disclosure policy
```

### HTML asset gzipping

HTML files under `data/` are pre-gzipped before upload. `scripts/gzip_assets.py`
runs automatically on every `pio run` (wired via `extra_scripts` in
`platformio.ini`) and produces `.html.gz` companions next to each source.

The firmware's `beginResponseGzipOrRaw()` helper checks for a `.gz` version in
LittleFS and serves it with `Content-Encoding: gzip` when the client accepts it.
Falls back transparently to the uncompressed file if no `.gz` exists.

**Why this matters:** a 90+ KB uncompressed `index.html` served directly
over LAN can saturate LWIP's pbuf pool mid-stream. Under that pressure,
colliding writes on the outbound WebSocket to Cloudflare fail with EAGAIN,
and DNS lookups (which share the same pool) also start failing. Gzipping
drops wire size ~4× and keeps the pool clear.

When you edit an `.html` file, `pio run` regenerates the `.gz` automatically,
and `pio run -t uploadfs` uploads both versions to LittleFS.

## Credits

- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
- [AsyncTCP](https://github.com/me-no-dev/AsyncTCP)
- [U8g2](https://github.com/olikraus/u8g2) (SH1106 128×64 OLED)
- [Adafruit BME280](https://github.com/adafruit/Adafruit_BME280_Library)
- [Adafruit CCS811](https://github.com/adafruit/Adafruit_CCS811_Library)
- [Uptime Library](https://github.com/YiannisBourkelis/Uptime-Library)

## Contact

Bug reports and feature questions → [Issues](https://github.com/EnochT14/hello-esp/issues).
For anything else → [Discussion](https://github.com/EnochT14/hello-esp/discussions).

## License

MIT. See [LICENSE](LICENSE).
