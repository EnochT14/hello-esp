// HelloESP Cloudflare Worker relay (WebSocket + chunked base64 streaming)

const PAGE_CSS = `*{margin:0;padding:0;box-sizing:border-box}:root{--bg:#f8f7f4;--ink:#1a1a1a;--mid:#888;--faint:#ccc}@media(prefers-color-scheme:dark){:root{--bg:#111110;--ink:#e8e6e1;--mid:#8a8a87;--faint:#2a2a28}}body{background:var(--bg);color:var(--ink);font-family:ui-monospace,"SF Mono","Cascadia Mono","Consolas",monospace;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:24px}main{margin:auto 0}main{max-width:480px;text-align:center}h1{font-size:clamp(48px,10vw,72px);font-weight:400;letter-spacing:-0.02em;margin-bottom:20px}p{font-size:13px;color:var(--mid);line-height:1.8;margin-bottom:16px}p.lede{color:var(--ink);font-size:14px;margin-bottom:24px}.note{font-size:11px;color:var(--mid);border-top:1px solid var(--faint);padding-top:20px;margin-top:28px;line-height:1.7}a{color:var(--ink);font-size:11px;letter-spacing:0.1em;text-transform:uppercase;text-underline-offset:3px;display:inline-block;margin:0 8px}.site-name{display:block;font-size:11px;letter-spacing:0.15em;text-transform:uppercase;color:var(--mid);text-decoration:none;margin:0 0 8px}.site-name:hover{color:var(--ink)}.status{font-size:10px;letter-spacing:0.1em;text-transform:uppercase;color:var(--mid);margin-top:28px}.dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--mid);margin-left:6px;vertical-align:middle;animation:pulse 1.4s ease-in-out infinite}@keyframes pulse{0%,100%{opacity:0.25}50%{opacity:1}}a:focus-visible{outline:2px solid var(--ink);outline-offset:2px;border-radius:2px}.action{color:var(--ink);font-weight:600}.helper{font-size:10px;color:var(--mid);letter-spacing:0.08em;text-transform:uppercase;margin-top:6px}@media(prefers-reduced-motion:reduce){.dot{animation:none}}`;

const CHIP_ICON_PATHS = `<path fill-rule="evenodd" d="M7 5a2 2 0 00-2 2v18a2 2 0 002 2h18a2 2 0 002-2V7a2 2 0 00-2-2H7zm6 7a1 1 0 00-1 1v6a1 1 0 001 1h6a1 1 0 001-1v-6a1 1 0 00-1-1h-6z"/><rect x="7.5" y="1" width="2" height="3" rx=".5"/><rect x="12.5" y="1" width="2" height="3" rx=".5"/><rect x="17.5" y="1" width="2" height="3" rx=".5"/><rect x="22.5" y="1" width="2" height="3" rx=".5"/><rect x="7.5" y="28" width="2" height="3" rx=".5"/><rect x="12.5" y="28" width="2" height="3" rx=".5"/><rect x="17.5" y="28" width="2" height="3" rx=".5"/><rect x="22.5" y="28" width="2" height="3" rx=".5"/><rect x="1" y="7.5" width="3" height="2" rx=".5"/><rect x="1" y="12.5" width="3" height="2" rx=".5"/><rect x="1" y="17.5" width="3" height="2" rx=".5"/><rect x="1" y="22.5" width="3" height="2" rx=".5"/><rect x="28" y="7.5" width="3" height="2" rx=".5"/><rect x="28" y="12.5" width="3" height="2" rx=".5"/><rect x="28" y="17.5" width="3" height="2" rx=".5"/><rect x="28" y="22.5" width="3" height="2" rx=".5"/>`;

const FAVICON = `<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,${encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" fill="#2686e6">${CHIP_ICON_PATHS}</svg>`).replace(/'/g, '%27').replace(/"/g, '%22')}">`;

// The header "HelloESP" breadcrumb used on every Worker-served page
const SITE_NAME_LINK = `<a href="/" class="site-name"><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="12" height="12" fill="#2686e6" style="vertical-align:-2px;margin-right:5px;">${CHIP_ICON_PATHS}</svg>HelloESP</a>`;

// Maintenance pages set body[data-until=<unix-ms>] so we can schedule a
// single reload at the end of the window instead of polling every 3s.
// Offline/timeout pages don't know when the ESP recovers, so they fall
const RETRY_JS = `<script>(function(){var u=document.body.getAttribute('data-until');if(u){var d=parseInt(u,10)-Date.now()+2000;if(d>0)setTimeout(function(){location.reload()},d);return;}setInterval(function(){if(document.visibilityState!=='visible')return;fetch('/ping?_='+Date.now(),{cache:'no-store'}).then(function(r){if(r.ok)location.reload()}).catch(function(){})},3000)})();</script>`;

const OFFLINE_HTML = `<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark"><meta name="robots" content="noindex">${FAVICON}<title>Offline / HelloESP</title><style>${PAGE_CSS}</style></head><body>${SITE_NAME_LINK}<main><h1>Offline</h1><p class="lede">The ESP32 serving this site isn't connected right now.</p><p>It might be rebooting, out of WiFi range, or unplugged. It'll come back on its own.</p><p><a href="/" class="action">Retry</a><a href="https://github.com/EnochT14/hello-esp" target="_blank" rel="noopener">GitHub</a></p><p class="status" role="status" aria-live="polite">Reconnecting<span class="dot" aria-hidden="true"></span></p><p class="helper">Auto-retrying every 3 seconds</p><p class="note">HelloESP runs entirely on an ESP32. When the chip is unreachable, Cloudflare serves this page instead.</p></main>${RETRY_JS}</body></html>`;

const TIMEOUT_HTML = `<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark"><meta name="robots" content="noindex">${FAVICON}<title>Timeout / HelloESP</title><style>${PAGE_CSS}</style></head><body>${SITE_NAME_LINK}<main><h1>Timeout</h1><p class="lede">The ESP32 got your request but didn't answer in time.</p><p>Probably busy handling something else. Try again in a moment.</p><p><a href="/" class="action">Retry</a><a href="https://github.com/EnochT14/hello-esp" target="_blank" rel="noopener">GitHub</a></p><p class="status" role="status" aria-live="polite">Retrying<span class="dot" aria-hidden="true"></span></p><p class="helper">Auto-retrying every 3 seconds</p><p class="note">HelloESP runs entirely on an ESP32. If a request takes over 30 seconds, Cloudflare shows this page.</p></main>${RETRY_JS}</body></html>`;

const SEC_HEADERS = {
  'Strict-Transport-Security': 'max-age=31536000; includeSubDomains',
  'X-Content-Type-Options': 'nosniff',
  'X-Frame-Options': 'DENY',
  'Referrer-Policy': 'strict-origin-when-cross-origin',
  // Allow cross-origin reads (portfolio site embeds live /stats + /adsb.json
  // data). Public site, no credentials, so `*` is safe; GETs with standard
  // headers need no preflight.
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type, Accept'
};

function applySecHeaders(h) {
  for (const [k, v] of Object.entries(SEC_HEADERS)) h.set(k, v);
  return h;
}

function offlineResponse() {
  return new Response(OFFLINE_HTML, { status: 502, headers: { 'Content-Type': 'text/html', 'Cache-Control': 'no-store', ...SEC_HEADERS } });
}

function timeoutResponse() {
  return new Response(TIMEOUT_HTML, { status: 504, headers: { 'Content-Type': 'text/html', 'Cache-Control': 'no-store', ...SEC_HEADERS } });
}

function escapeHtml(s) {
  // `/` is also escaped so this remains safe even inside a `<script>` block,
  // where `</script>` would otherwise terminate the script context.
  return String(s).replace(/[&<>"'\/]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;','/':'&#47;'}[c]));
}

// Parse "14 days, 3 hours, 22 minutes, 45 seconds" into a compact "14d 3h" / "3h 22m" / "22m 45s" string
function formatUptime(str) {
  if (!str) return '';
  const m = String(str).match(/(\d+)\s*days?[,\s]+(\d+)\s*hours?[,\s]+(\d+)\s*minutes?[,\s]+(\d+)\s*seconds?/);
  if (!m) return str;
  const d = +m[1], h = +m[2], mn = +m[3], s = +m[4];
  if (d > 0)  return `up ${d}d ${h}h`;
  if (h > 0)  return `up ${h}h ${mn}m`;
  if (mn > 0) return `up ${mn}m ${s}s`;
  return `up ${s}s`;
}

function maintenanceResponse(until, message) {
  const remainingMs = Math.max(0, until - Date.now());
  const safeMsg = message ? escapeHtml(String(message).slice(0, 200)) : '';
  const lede = safeMsg || "The ESP is down for maintenance.";
  let etaLine;
  let retryAfter;
  if (remainingMs < 60000) {
    etaLine = 'Back shortly.';
    retryAfter = 30;
  } else {
    const mins = Math.ceil(remainingMs / 60000);
    etaLine = `Back in about ${mins} ${mins === 1 ? 'minute' : 'minutes'}.`;
    retryAfter = Math.min(3600, mins * 60);
  }
  const html = `<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark"><meta name="robots" content="noindex">${FAVICON}<title>Maintenance / HelloESP</title><style>${PAGE_CSS}</style></head><body data-until="${until}">${SITE_NAME_LINK}<main><h1>Maintenance</h1><p class="lede">${lede}</p><p>${etaLine}</p><p><a href="/" class="action">Retry</a><a href="https://github.com/EnochT14/hello-esp" target="_blank" rel="noopener">GitHub</a></p><p class="status" role="status" aria-live="polite">Checking<span class="dot" aria-hidden="true"></span></p><p class="note">HelloESP runs entirely on an ESP32. Planned work is in progress. The page will refresh automatically when the site is back.</p></main>${RETRY_JS}</body></html>`;
  return new Response(html, {
    status: 503,
    headers: {
      'Content-Type': 'text/html',
      'Cache-Control': 'no-store',
      'Retry-After': String(retryAfter),
      ...SEC_HEADERS
    }
  });
}

function b64ToBytes(b64) {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

// Constant-time string compare; avoids leaking secret length/content via
// early-exit timing when comparing provided creds against WORKER_SECRET.
function timingSafeEqualStr(a, b) {
  const enc = new TextEncoder();
  const ab = enc.encode(String(a ?? ''));
  const bb = enc.encode(String(b ?? ''));
  const len = Math.max(ab.length, bb.length);
  let diff = ab.length ^ bb.length;
  for (let i = 0; i < len; i++) {
    const av = i < ab.length ? ab[i] : 0;
    const bv = i < bb.length ? bb[i] : 0;
    diff |= av ^ bv;
  }
  return diff === 0;
}

const MAX_BODY = 8192;
const RATE_LIMIT_WINDOW = 60000; // 1 min
const RATE_LIMIT_MAX = 60;       // per IP per window
const SSE_MAX_CLIENTS = 500;     // cap concurrent SSE connections so a flood can't exhaust DO memory
const SSE_MAX_PAYLOAD = 64 * 1024; // per-event byte cap so one fat event * 500 clients can't OOM the DO

// Weather proxy; Oyarifa, Ghana (~5.6986 N, 0.2108 W). Coords are the town
// center (this is the user's home region), so this is non-identifying. Metric
// units (celsius/kmh) match what the rest of the site uses.
const WEATHER_LAT = 5.6986;
const WEATHER_LON = -0.2108;
const WEATHER_LOCATION = 'Oyarifa, Ghana';
const WEATHER_REFRESH_MS = 3600000; // 1 hour
const WEATHER_STALE_MS   = 7200000; // after 2h with no successful refresh, stop sending outdoor data

// Email backup bundle limits. The Worker chunks the final bundle across however many emails
// are needed (see BACKUP_PART_SIZE). Per-file and per-session ceilings live on EspRelay
// (MAX_FILE_B64 / MAX_TOTAL_BYTES) and apply to the R2 path as well.
const BACKUP_SESSION_IDLE  = 15 * 60 * 1000;    // drop sessions idle > 15 min
const BACKUP_PART_SIZE     = 7 * 1024 * 1024;   // raw-byte slice per email (safely < SMTP2GO 10 MB rec.)
const BACKUP_PART_DELAY_MS = 2000;              // pause between multipart sends

function numOrNull(v) {
  if (v === null || v === undefined || v === '') return null;
  if (typeof v === 'number') return isFinite(v) ? v : null;
  if (typeof v === 'string') {
    const s = v.trim().toLowerCase();
    if (s === 'ground' || s === '') return null;
    const n = parseFloat(s);
    return isFinite(n) ? n : null;
  }
  return null;
}

function cleanCallsign(v) {
  if (typeof v !== 'string') return '';
  return v.replace(/[^A-Za-z0-9]/g, '').trim().slice(0, 8).toUpperCase();
}

function round4(n) {
  return Math.round(n * 1e4) / 1e4;
}

export class EspRelay {
  constructor(state, env) {
    this.state = state;
    this.env = env || {};
    this.espSocket = null;
    this.pendingRequests = new Map();
    this.activeResponses = new Map();
    this.requestId = 0;
    this.currentStreamId = null;
    this.lastActivity = 0;
    this.rateLimits = new Map();
    this.wsAuthFails = new Map();
    this.lastEmailAt = 0;
    this.hmacAuthenticated = false;
    this.maintenanceUntil = 0;
    this.maintenanceMessage = '';
    this.sseClients = new Set();
    this.lastStats = null;  // JSON string of the most recent ESP stats push
    this.lastStatsAt = 0;   // epoch ms when lastStats was set; used to detect staleness for badges
    this.lastAdsb = null;   // JSON string of the most recent processed fleet
    this.lastAdsbAt = 0;    // epoch ms when lastAdsb was set
    // Per-hex trail history. Bounded by ADSB_DROP_MS sweeps; RAM only, since
    // the receiver re-supplies the whole fleet every cycle.
    this.adsbTracks = new Map();
    this.lastWeather = null; // cached outdoor weather object
    this.lastAirQuality = null; // cached outdoor air-quality object (PM2.5, US AQI)
    this.deadmanAlertSent = false; // so we don't spam when offline persists past 24h
    this.backupSessions = new Map(); // seq -> { startedAt, meta, files[], currentFile, totalB64, aborted }
    this.lastBackupAt = 0;
    this.lastBackupDate = '';
    this.lastBackupFailureEmailAt = 0;
    this.lastBackupMissedEmailAt = 0;
    this.firstSeenAt = 0;  // DO construction time, used as fallback floor for missed-backup alert
    this.state.blockConcurrencyWhile(async () => {
      const u = await state.storage.get('maintenanceUntil');
      const m = await state.storage.get('maintenanceMessage');
      const w = await state.storage.get('lastWeather');
      const aq = await state.storage.get('lastAirQuality');
      const dm = await state.storage.get('deadmanAlertSent');
      const lba = await state.storage.get('lastBackupAt');
      const lbd = await state.storage.get('lastBackupDate');
      const lact = await state.storage.get('lastActivity');
      const fseen = await state.storage.get('firstSeenAt');
      const lbMissed = await state.storage.get('lastBackupMissedEmailAt');
      const lbFail = await state.storage.get('lastBackupFailureEmailAt');
      if (typeof u === 'number') this.maintenanceUntil = u;
      if (typeof m === 'string') this.maintenanceMessage = m;
      if (w && typeof w === 'object') this.lastWeather = w;
      if (aq && typeof aq === 'object') this.lastAirQuality = aq;
      if (typeof dm === 'boolean') this.deadmanAlertSent = dm;
      if (typeof lba === 'number') this.lastBackupAt = lba;
      if (typeof lbd === 'string') this.lastBackupDate = lbd;
      // Restoring lastActivity after isolate eviction: without this, a truly-
      // dead device's deadman alert can't fire because the `lastActivity > 0`
      // guard rejects the default-zero value, AND a recovery email could fire
      // spuriously when the ESP reconnects to a fresh isolate.
      if (typeof lact === 'number') this.lastActivity = lact;
      // First-seen tracking: stamped once on initial DO creation, used as a
      // fallback timestamp for the missed-backup alert when no successful
      // backup has ever happened. Without this, a fresh deploy that never
      // gets a successful backup would never alert.
      if (typeof fseen === 'number') {
        this.firstSeenAt = fseen;
      } else {
        this.firstSeenAt = Date.now();
        await state.storage.put('firstSeenAt', this.firstSeenAt);
      }
      // Persisted throttle markers so a DO isolate eviction doesn't reset
      // the 1/hr + 1/day email caps and re-spam the inbox after restart.
      if (typeof lbMissed === 'number') this.lastBackupMissedEmailAt = lbMissed;
      if (typeof lbFail === 'number') this.lastBackupFailureEmailAt = lbFail;
    });
    this._ensureAlarm(30000);
  }

  // Only schedule a new alarm if none is set or the existing one is further out
  // than the requested window. Otherwise we keep pushing the alarm into the
  // future on every SSE connect / constructor call, which starves the deadman.
  async _ensureAlarm(msFromNow) {
    const target = Date.now() + msFromNow;
    const existing = await this.state.storage.getAlarm();
    if (existing == null || existing > target) {
      await this.state.storage.setAlarm(target);
    }
  }

  // SMTP2GO send wrapper. Returns the Response when SMTP2GO is configured
  // (so callers can check .ok), or null when keys are missing.
  async _sendEmail({ subject, text_body, attachments }) {
    const env = this.env;
    if (!env.SMTP2GO_KEY || !env.NOTIFY_EMAIL) return null;
    const payload = {
      sender: env.NOTIFY_FROM || 'HelloESP <noreply@ecobbina.work>',
      to: [env.NOTIFY_EMAIL],
      subject,
      text_body
    };
    if (attachments) payload.attachments = attachments;
    return fetch('https://api.smtp2go.com/v3/email/send', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Smtp2go-Api-Key': env.SMTP2GO_KEY },
      body: JSON.stringify(payload)
    });
  }

  // Per-IP rate limit check. Returns null when allowed, or a 429 Response
  // when exceeded. Non-relay endpoints call this directly so they aren't a
  // back door around the relay-path limit at the bottom of fetch().
  // Handlers opt into this explicitly; the relay path is capped separately.
  _enforceRateLimit(clientIP) {
    const now = Date.now();
    let rl = this.rateLimits.get(clientIP);
    if (!rl || now > rl.resetAt) {
      rl = { count: 0, resetAt: now + RATE_LIMIT_WINDOW };
      this.rateLimits.set(clientIP, rl);
    }
    rl.count++;
    if (rl.count > RATE_LIMIT_MAX) {
      return new Response('Rate limit exceeded', {
        status: 429,
        headers: { 'Content-Type': 'text/plain', 'Retry-After': '60', ...SEC_HEADERS }
      });
    }
    if (this.rateLimits.size > 500) {
      for (const [k, v] of this.rateLimits) {
        if (v.resetAt < now) this.rateLimits.delete(k);
      }
    }
    return null;
  }

  async maybeSendDeadmanAlert() {
    const env = this.env;
    if (!env.SMTP2GO_KEY || !env.NOTIFY_EMAIL) return;
    const now = Date.now();
    // Planned downtime: maintenance window suppresses the deadman alert so an
    // intentionally-offline device doesn't spam the inbox.
    if (now < this.maintenanceUntil) return;
    // DEADMAN_HOURS env var overrides default; typical home has near-zero ISP outages >6h
    const hoursCfg = parseFloat(env.DEADMAN_HOURS);
    const DEAD_HOURS = (hoursCfg > 0 && hoursCfg < 720) ? hoursCfg : 6;
    const DEAD_MS = DEAD_HOURS * 3600000;
    const BACK_MS = 300000;   // 5 minutes of fresh activity = considered back
    const elapsed = now - this.lastActivity;

    // device has been silent for >DEAD_HOURS and we haven't alerted yet
    if (this.lastActivity > 0 && elapsed > DEAD_MS && !this.deadmanAlertSent) {
      this.deadmanAlertSent = true;
      await this.state.storage.put('deadmanAlertSent', true);
      const hours = Math.floor(elapsed / 3600000);
      const lastSeen = new Date(this.lastActivity).toISOString();
      const body = `HelloESP has been unreachable for ${hours} hours.\n\nLast heartbeat: ${lastSeen}\n\nThe device may be offline, rebooting into a loop, or has lost WiFi.\nThis is an automated dead-man's-switch alert; you won't get another until it recovers and goes silent again.`;
      try {
        await this._sendEmail({
          subject: `HelloESP unreachable (${hours}h)`,
          text_body: body
        });
      } catch (e) { console.error('deadman email failed:', e && e.message); }
      return;
    }

    // device came back; clear the flag and send a recovery notification
    if (this.deadmanAlertSent && elapsed < BACK_MS) {
      this.deadmanAlertSent = false;
      await this.state.storage.put('deadmanAlertSent', false);
      const body = `HelloESP is back online.\n\nFirst fresh heartbeat: ${new Date(this.lastActivity).toISOString()}\n\nThis is a dead-man's-switch recovery notification.`;
      try {
        await this._sendEmail({ subject: 'HelloESP recovered', text_body: body });
      } catch (e) { console.error('deadman-recovered email failed:', e && e.message); }
    }
  }

  async refreshAirQuality() {
    try {
      const url = `https://air-quality-api.open-meteo.com/v1/air-quality?latitude=${WEATHER_LAT}&longitude=${WEATHER_LON}&current=us_aqi,pm2_5,carbon_dioxide,uv_index`;
      const res = await fetch(url, { cf: { cacheTtl: 3600 } });
      if (!res.ok) { console.error('air quality http', res.status); return; }
      const data = await res.json();
      if (!data || !data.current) return;
      const c = data.current;
      this.lastAirQuality = {
        us_aqi:        c.us_aqi,
        pm2_5:         c.pm2_5,
        co2_ppm:       c.carbon_dioxide,
        uv_index:      c.uv_index,
        fetched_at:    Date.now()
      };
      await this.state.storage.put('lastAirQuality', this.lastAirQuality);
    } catch (e) {
      console.error('air quality fetch failed:', e && e.message);
    }
  }

  async refreshWeather() {
    try {
      const url = `https://api.open-meteo.com/v1/forecast?latitude=${WEATHER_LAT}&longitude=${WEATHER_LON}&current=temperature_2m,apparent_temperature,relative_humidity_2m,dew_point_2m,weather_code,wind_speed_10m,wind_direction_10m,surface_pressure,is_day&temperature_unit=celsius&wind_speed_unit=kmh`;
      const res = await fetch(url, { cf: { cacheTtl: 3600 } });
      if (!res.ok) { console.error('weather http', res.status); return; }
      const data = await res.json();
      if (!data || !data.current) return;
      const c = data.current;
      // Capture previous pressure BEFORE overwriting lastWeather, so we can derive
      // a trend label (Rising/Falling/Steady) over the hourly refresh interval.
      const prevPressure = this.lastWeather && this.lastWeather.pressure_hpa;
      let pressureTrend = 'Steady';
      if (typeof prevPressure === 'number' && typeof c.surface_pressure === 'number') {
        const diff = c.surface_pressure - prevPressure;
        if (diff > 1.0)  pressureTrend = 'Rising';
        else if (diff < -1.0) pressureTrend = 'Falling';
      }
      this.lastWeather = {
        temp_c:         c.temperature_2m,
        feels_like_c:   c.apparent_temperature,
        humidity:       c.relative_humidity_2m,
        dewpoint_c:     c.dew_point_2m,
        temp_f:         c.temperature_2m * 9 / 5 + 32,
        feels_like_f:   c.apparent_temperature * 9 / 5 + 32,
        dewpoint_f:     c.dew_point_2m * 9 / 5 + 32,
        weather_code:   c.weather_code,
        wind_mph:       c.wind_speed_10m,
        wind_deg:       c.wind_direction_10m,
        pressure_hpa:   c.surface_pressure,
        pressure_trend: pressureTrend,
        is_day:         c.is_day === 1,
        location:       WEATHER_LOCATION,
        fetched_at:     Date.now()
      };
      await this.state.storage.put('lastWeather', this.lastWeather);
    } catch (e) {
      console.error('weather fetch failed:', e && e.message);
    }
  }

  enrichStats(rawData) {
    // returns the stats object with outdoor weather injected if we have fresh-enough data
    if (!this.lastWeather) return rawData;
    if (Date.now() - this.lastWeather.fetched_at > WEATHER_STALE_MS) return rawData;
    const outdoor = {
      temp_c:         this.lastWeather.temp_c,
      feels_like_c:   this.lastWeather.feels_like_c,
      humidity:       this.lastWeather.humidity,
      dewpoint_c:     this.lastWeather.dewpoint_c,
      // Legacy _f aliases: kept for clients flashed before the Celsius
      // switch. Pre-rename payloads stored raw Celsius in these fields,
      // so map raw (no conversion) to keep old pages consistent.
      temp_f:         typeof this.lastWeather.temp_c === 'number'
                        ? this.lastWeather.temp_c : this.lastWeather.temp_f,
      feels_like_f:   typeof this.lastWeather.feels_like_c === 'number'
                        ? this.lastWeather.feels_like_c : this.lastWeather.feels_like_f,
      dewpoint_f:     typeof this.lastWeather.dewpoint_c === 'number'
                        ? this.lastWeather.dewpoint_c : this.lastWeather.dewpoint_f,
      weather_code:   this.lastWeather.weather_code,
      wind_mph:       this.lastWeather.wind_mph,
      wind_deg:       this.lastWeather.wind_deg,
      pressure_hpa:   this.lastWeather.pressure_hpa,
      pressure_trend: this.lastWeather.pressure_trend,
      is_day:         this.lastWeather.is_day,
      location:       this.lastWeather.location,
      age_ms:         Date.now() - this.lastWeather.fetched_at
    };
    // Air quality piggybacks on the same outdoor block, gated independently
    // on its own freshness so a stale AQ fetch doesn't suppress weather and
    // vice versa. Same staleness window (2h) since both refresh hourly.
    if (this.lastAirQuality
        && Date.now() - this.lastAirQuality.fetched_at <= WEATHER_STALE_MS) {
      if (typeof this.lastAirQuality.us_aqi === 'number') {
        outdoor.us_aqi = this.lastAirQuality.us_aqi;
      }
      if (typeof this.lastAirQuality.pm2_5 === 'number') {
        outdoor.pm2_5 = this.lastAirQuality.pm2_5;
      }
      if (typeof this.lastAirQuality.co2_ppm === 'number') {
        outdoor.co2_ppm = this.lastAirQuality.co2_ppm;
      }
      if (typeof this.lastAirQuality.uv_index === 'number') {
        outdoor.uv_index = this.lastAirQuality.uv_index;
      }
    }
    return { ...rawData, outdoor };
  }

  badgeState() {
    // figure out what the badge should say + what color: live / stale / offline / maintenance
    const now = Date.now();
    if (now < this.maintenanceUntil) return { state: 'maintenance', color: '#c06b00' };
    if (!this.lastStats) return { state: 'offline', color: '#666' };
    if (now - this.lastStatsAt > 120000) return { state: 'stale', color: '#666' };
    return { state: 'live', color: '#2686e6' };
  }

  _todayUtc() { return new Date().toISOString().slice(0, 10); }


  // Full DO storage snapshot to R2. Other DO state (weather/AQ caches,
  // operational flags, first-seen markers) lives only in DO. A daily
  // catch-all snapshot to R2 means DO loss (CF outage, bug-driven mass
  // delete, accidental DO Explorer click) is recoverable from at most
  // yesterday's state. Per-key backups would be more current but more
  // invasive; daily JSON is the lowest-surface-area fix. Output:
  // state/do-snapshot/YYYY-MM-DD.json with all keys.
  async _doSnapshot() {
    if (!this.env.BACKUP) return;
    const today = this._todayUtc();
    const allKeys = {};
    let cursor;
    // Cursor-paginate the DO list since the per-call cap is 1000. With ~100
    // keys today this is one call; future-safe as the key count grows.
    while (true) {
      const opts = { limit: 1000 };
      if (cursor) opts.start = cursor;
      const page = await this.state.storage.list(opts);
      if (page.size === 0) break;
      let lastKey = null;
      for (const [k, v] of page) {
        lastKey = k;
        allKeys[k] = v;
      }
      if (page.size < 1000 || !lastKey) break;
      cursor = lastKey + '\x00';
    }
    const payload = {
      snapshot_at: Math.floor(Date.now() / 1000),
      snapshot_date: today,
      key_count: Object.keys(allKeys).length,
      data: allKeys,
    };
    try {
      await this.env.BACKUP.put('state/do-snapshot/' + today + '.json',
        JSON.stringify(payload),
        { httpMetadata: { contentType: 'application/json' } });
    } catch (e) {
      console.error('do-snapshot R2 write failed:', e && e.message);
      return;
    }
    await this.state.storage.put('lastDoSnapshotDate', today);
    console.log('do-snapshot: wrote', today, 'with', payload.key_count, 'keys');
    // Prune snapshots older than 30 days. R2 storage is cheap but unbounded
    // growth is messy; 30 days is plenty of recovery window.
    try {
      const cutoff = new Date();
      cutoff.setUTCDate(cutoff.getUTCDate() - 30);
      const cutoffStr = cutoff.toISOString().slice(0, 10);
      const list = await this.env.BACKUP.list({ prefix: 'state/do-snapshot/' });
      const toDelete = [];
      for (const obj of (list.objects || [])) {
        const m = obj.key.match(/state\/do-snapshot\/(\d{4}-\d{2}-\d{2})\.json$/);
        if (m && m[1] < cutoffStr) toDelete.push(obj.key);
      }
      if (toDelete.length) {
        await this.env.BACKUP.delete(toDelete);
        console.log('do-snapshot: pruned', toDelete.length, 'old snapshots');
      }
    } catch (e) {
      console.error('do-snapshot prune failed:', e && e.message);
      // Non-fatal: snapshot was written, prune is best-effort.
    }
  }

  // Once-per-UTC-day gate on _doSnapshot. Called from alarm(), short-circuits
  // most of the time after the first call each day. Storage check is cheap
  // (single get), much cheaper than redundantly snapshotting on every alarm.
  async _maybeDoSnapshot() {
    if (!this.env.BACKUP) return;
    const today = this._todayUtc();
    const last = await this.state.storage.get('lastDoSnapshotDate');
    if (last === today) return;
    await this._doSnapshot();
  }

  buildCurlCard() {
    let s = null;
    try { if (this.lastStats) s = JSON.parse(this.lastStats); } catch (e) {}

    const num   = (v) => (Number.isFinite(v) ? Math.round(v).toLocaleString() : '-');
    const one   = (v, unit) => (Number.isFinite(v) ? v.toFixed(1) + (unit || '') : '-');
    const dash  = '-';

    const uptime   = s && s.uptime ? String(s.uptime) : dash;
    const tempC    = s && s.temperature && Number.isFinite(s.temperature.celsius)
        ? one(s.temperature.celsius, '°C') : dash;
    const humidity = s && Number.isFinite(s.humidity_percent)
        ? one(s.humidity_percent, '%') : dash;
    const pressure = s && Number.isFinite(s.pressure_hpa)
        ? one(s.pressure_hpa, ' hPa') : dash;
    const co2      = s && Number.isFinite(s.co2_ppm)
        ? num(s.co2_ppm) + ' ppm (eCO₂)' : dash;
    const rssi     = s && Number.isFinite(s.rssi) ? s.rssi + ' dBm' : dash;
    const visitors = s ? num(s.visitors) + ' all-time · ' + num(s.daily_visitors) + ' today' : dash;
    const countries = s ? num(s.countries) : dash;
    const heapFree = s && s.memory && Number.isFinite(s.memory.used_percent)
        ? (100 - s.memory.used_percent).toFixed(0) + '% free' : dash;
    let sd = dash;
    if (s && Number.isFinite(s.sd_used_mb) && Number.isFinite(s.sd_free_mb)) {
        const totalMb = s.sd_used_mb + s.sd_free_mb;
        if (totalMb > 0) {
            sd = Math.round(s.sd_used_mb) + ' MB used / ' + Math.round(totalMb) + ' MB';
        }
    }

    const lines = [
      '',
      '   _    _      _ _       ______  _____ _____',
      '  | |  | |    | | |     |  ____|/ ____|  __ \\',
      '  | |__| | ___| | | ___ | |__  | (___ | |__) |',
      '  |  __  |/ _ \\ | |/ _ \\|  __|  \\___ \\|  ___/',
      '  | |  | |  __/ | | (_) | |____ ____) | |',
      '  |_|  |_|\\___|_|_|\\___/|______|_____/|_|',
      '',
      '  A website running on an ESP32 on a wall in Denver.',
      '',
      '  STATUS',
      '    Uptime       ' + uptime,
      '    Visitors     ' + visitors,
      '    Countries    ' + countries,
      '',
      '  INDOOR (sealed frame)',
      '    Temp         ' + tempC,
      '    Humidity     ' + humidity,
      '    Air          ' + co2,
      '    Pressure     ' + pressure,
    ];

    if (s && Number.isFinite(s.power_w)) {
      const fmtWh = (v) => {
        if (!Number.isFinite(v)) return dash;
        if (v >= 1000) return (v / 1000).toFixed(2) + ' kWh';
        return Math.round(v) + ' Wh';
      };
      lines.push(
        '',
        '  POWER (smart plug)',
        '    Now          ' + s.power_w.toFixed(1) + ' W',
        '    Today        ' + fmtWh(s.energy_today_wh),
        '    Lifetime     ' + fmtWh(s.energy_total_wh)
      );
    }

    if (s && s.outdoor && (Number.isFinite(s.outdoor.temp_c) || Number.isFinite(s.outdoor.temp_f))) {
      const o = s.outdoor;
      const ot = (typeof o.temp_c === 'number') ? o.temp_c : o.temp_f;
      const loc = (o.location && typeof o.location === 'string') ? o.location : 'Denver, CO';
      lines.push(
        '',
        '  OUTDOOR (' + loc + ')',
        '    Temp         ' + one(ot, '°C'),
        '    Humidity     ' + (Number.isFinite(o.humidity) ? one(o.humidity, '%') : dash),
        '    Wind         ' + (Number.isFinite(o.wind_mph) ? one(o.wind_mph, ' mph') : dash)
      );
      if (Number.isFinite(o.us_aqi) && o.us_aqi >= 0) {
        const aqi = Math.round(o.us_aqi);
        const label = aqi <= 50 ? 'good'
                    : aqi <= 100 ? 'moderate'
                    : aqi <= 150 ? 'unhealthy for sensitive'
                    : aqi <= 200 ? 'unhealthy'
                    : aqi <= 300 ? 'very unhealthy'
                    : 'hazardous';
        lines.push('    AQI          ' + aqi + ' (' + label + ')');
      }
    }

    lines.push(
      '',
      '  DEVICE',
      '    Heap         ' + heapFree,
      '    SD card      ' + sd,
      '    WiFi         ' + rssi,
      '',
      '  LINKS',
      '    Web          https://esp.ecobbina.work',
      '    Guestbook    https://esp.ecobbina.work/guestbook',
      '    Console      https://esp.ecobbina.work/console',
      '    History      https://esp.ecobbina.work/history',
      '    About        https://esp.ecobbina.work/about',
      '    Source       https://github.com/EnochT14/hello-esp',
      '    Guestbook RSS https://esp.ecobbina.work/guestbook.rss',
      '    Badge        https://esp.ecobbina.work/status.svg',
      '',
      '  (You asked for it with curl. Nice.)',
      ''
    );
    return lines.join('\n');
  }

  buildStatusBadge(metric) {
    const s = this.badgeState();
    let stats = null;
    try { if (this.lastStats) stats = JSON.parse(this.lastStats); } catch (e) {}

    let valueText;
    if (s.state === 'maintenance') {
      valueText = 'maintenance';
    } else if (s.state === 'offline' || s.state === 'stale' || !stats) {
      valueText = 'offline';
    } else {
      switch (metric) {
        case 'visits':
          valueText = (stats.visitors != null ? stats.visitors : 0) + ' visits';
          break;
        case 'temp':
          if (stats.temperature && typeof stats.temperature.celsius === 'number') {
            valueText = Math.round(stats.temperature.celsius) + '\u00b0C';
          } else { valueText = 'no data'; }
          break;
        case 'power':
          // Live wattage from the Shelly poll (only present when shelly_url
          // is configured AND the freshness gate in buildStatsJson is met).
          // 'no data' covers both the forker case (no Shelly) and the stale
          // case (Shelly unreachable for >3 min) using the same fallback
          // the temp/visits cases use.
          if (typeof stats.power_w === 'number') {
            valueText = stats.power_w.toFixed(1) + ' W';
          } else { valueText = 'no data'; }
          break;
        case 'online':
          valueText = '\u25CF live';
          break;
        case 'uptime':
        default:
          valueText = formatUptime(stats.uptime || '');
          if (!valueText) valueText = 'up ?';
          break;
      }
    }

    // Approximate Verdana-11 character width ~6.5px. Label "HelloESP" fixed at 78px.
    const labelW = 78;
    const charW = 7;
    const valueW = Math.max(54, Math.round(valueText.length * charW + 20));
    const totalW = labelW + valueW;

    const safeValue = escapeHtml(valueText);
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${totalW}" height="20" viewBox="0 0 ${totalW} 20" role="img" aria-label="HelloESP: ${safeValue}"><title>HelloESP: ${safeValue}</title><linearGradient id="g" x2="0" y2="100%"><stop offset="0" stop-color="#bbb" stop-opacity=".1"/><stop offset="1" stop-opacity=".1"/></linearGradient><clipPath id="r"><rect width="${totalW}" height="20" rx="3" fill="#fff"/></clipPath><g clip-path="url(#r)"><rect width="${labelW}" height="20" fill="#1a1a1a"/><rect x="${labelW}" width="${valueW}" height="20" fill="${s.color}"/><rect width="${totalW}" height="20" fill="url(#g)"/></g><g fill="#fff" text-anchor="middle" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11"><text x="${labelW/2}" y="14">HelloESP</text><text x="${labelW + valueW/2}" y="14">${safeValue}</text></g></svg>`;
  }

  buildStatusWide() {
    const s = this.badgeState();
    let stats = null;
    try { if (this.lastStats) stats = JSON.parse(this.lastStats); } catch (e) {}

    const W = 340, H = 78;
    const chipX = 14, chipY = 5;   // chip icon position
    const chipSize = 20;

    let line1Right;  // status indicator text + color
    if (s.state === 'maintenance')       line1Right = { text: 'maintenance', color: '#c06b00' };
    else if (s.state === 'live')         line1Right = { text: '\u25CF live', color: '#0a8b4a' };
    else                                 line1Right = { text: '\u25CF offline', color: '#b02030' };

    let row2 = '', row3 = '';
    if (stats && s.state !== 'offline' && s.state !== 'stale') {
      // Row A: indoor environment readings + power draw (when Shelly fresh)
      const tempC = stats.temperature && typeof stats.temperature.celsius === 'number'
        ? Math.round(stats.temperature.celsius) + '\u00b0C' : null;
      const hum = stats.humidity_percent != null ? Math.round(stats.humidity_percent) + '% RH' : null;
      const co2 = stats.co2_ppm != null ? stats.co2_ppm + ' CO\u2082 ppm' : null;
      const power = (typeof stats.power_w === 'number') ? Math.round(stats.power_w) + ' W' : null;

      const rowA = [tempC, hum, co2, power].filter(Boolean).join(' \u00b7 ');

      // Row B: ops/social (uptime, visits, countries, messages). Compact
      // uptime ("47d" / "8h" / "23m") and tight separators are needed to
      // fit all four within the 340px wide-card budget. Missing values
      // collapse the row gracefully via filter(Boolean).
      let up = null;
      const upMatch = String(stats.uptime || '').match(/(\d+)\s*days?[,\s]+(\d+)\s*hours?[,\s]+(\d+)\s*minutes?/);
      if (upMatch) {
        const d = +upMatch[1], h = +upMatch[2], mn = +upMatch[3];
        if (d > 0)       up = d + 'd';
        else if (h > 0)  up = h + 'h';
        else if (mn > 0) up = mn + 'm';
      }
      const vis = (typeof stats.visitors === 'number' && stats.visitors > 0)
        ? stats.visitors.toLocaleString() + ' visits' : null;
      const countries = (typeof stats.countries === 'number' && stats.countries > 0)
        ? stats.countries + ' countries' : null;
      const messages = (typeof stats.guestbook_approved === 'number' && stats.guestbook_approved > 0)
        ? stats.guestbook_approved.toLocaleString() + ' msgs' : null;
      const rowB = [up, vis, countries, messages].filter(Boolean).join(' \u00b7 ');

      row2 = escapeHtml(rowA);
      row3 = escapeHtml(rowB);
    } else {
      row2 = s.state === 'maintenance' ? 'Site under planned maintenance.' : 'Device is not currently reachable.';
      row3 = 'Check back in a moment.';
    }

    const statusColor = escapeHtml(line1Right.color);
    const statusText = escapeHtml(line1Right.text);

    return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" role="img" aria-label="HelloESP status"><title>HelloESP status</title><clipPath id="rw"><rect width="${W}" height="${H}" rx="6" fill="#fff"/></clipPath><g clip-path="url(#rw)"><rect width="${W}" height="${H}" fill="#f8f7f4"/><rect width="${W}" height="28" fill="#1a1a1a"/></g><g transform="translate(${chipX} ${chipY}) scale(${chipSize/32})" fill="#2686e6">${CHIP_ICON_PATHS}</g><g font-family="Verdana,Geneva,DejaVu Sans,sans-serif" fill="#fff"><text x="42" y="19" font-size="12" font-weight="bold">HelloESP</text></g><g font-family="Verdana,Geneva,DejaVu Sans,sans-serif"><text x="${W - 14}" y="19" text-anchor="end" font-size="11" fill="${statusColor}">${statusText}</text><text x="14" y="50" font-size="12" fill="#1a1a1a">${row2}</text><text x="14" y="68" font-size="11" fill="#555">${row3}</text></g></svg>`;
  }

  broadcastEvent(eventName, jsonStr) {
    // Cap per-event payload size: with SSE_MAX_CLIENTS=500 viewers, a 1 MB
    // payload would be a 500 MB instantaneous fanout against a DO with a
    // ~128 MB memory ceiling. Drop oversized events instead of crashing the
    // DO; SSE delivery to all currently-connected clients is preserved for
    // every event that fits under the cap.
    const payload = new TextEncoder().encode(`event: ${eventName}\ndata: ${jsonStr}\n\n`);
    if (payload.byteLength > SSE_MAX_PAYLOAD) return;
    const dead = [];
    for (const w of this.sseClients) {
      w.write(payload).catch(() => dead.push(w));
    }
    for (const w of dead) {
      this.sseClients.delete(w);
      // Release the writer's transform-stream state. Without abort(), the
      // writable side stays held even after we forget the reference, which
      // leaks per-flapped-client over the DO's lifetime.
      w.abort().catch(() => {});
    }
  }

  async setMaintenance(minutes, message) {
    // Cap at 30 days so an intentionally-offline device (e.g. travelling,
    // hardware down for weeks) can be silenced with one action instead of
    // re-arming a 2h window. 0 still cancels.
    const m = Math.min(43200, Math.max(0, Number(minutes) || 0));
    if (m === 0) {
      this.maintenanceUntil = 0;
      this.maintenanceMessage = '';
      await this.state.storage.delete('maintenanceUntil');
      await this.state.storage.delete('maintenanceMessage');
    } else {
      this.maintenanceUntil = Date.now() + m * 60000;
      this.maintenanceMessage = String(message || '').slice(0, 200);
      await this.state.storage.put('maintenanceUntil', this.maintenanceUntil);
      await this.state.storage.put('maintenanceMessage', this.maintenanceMessage);
    }
  }

  async verifyHmac(hexSig, nonce) {
    try {
      if (!hexSig || !/^[0-9a-f]{64}$/i.test(hexSig)) return false;
      const sig = new Uint8Array(32);
      for (let i = 0; i < 32; i++) sig[i] = parseInt(hexSig.slice(i * 2, i * 2 + 2), 16);
      const keyBytes = new TextEncoder().encode(this.env.HMAC_SECRET);
      const key = await crypto.subtle.importKey('raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['verify']);
      return await crypto.subtle.verify('HMAC', key, sig, new TextEncoder().encode(nonce));
    } catch (e) {
      return false;
    }
  }

  async handleEvent(msg) {
    try {
      if (msg.event === 'maintenance') {
        await this.setMaintenance(msg.minutes, msg.message);
        return;
      }
      if (msg.event === 'stats_update') {
        if (msg.data) {
          const enriched = this.enrichStats(msg.data);
          this.lastStats = JSON.stringify(enriched);
          this.lastStatsAt = Date.now();
          // `clients` is per-broadcast (not cached in lastStats) so the
          // homepage presence indicator reflects current connections.
          const broadcastBody = JSON.stringify({ ...enriched, clients: this.sseClients.size });
          this.broadcastEvent('stats', broadcastBody);
        }
        return;
      }
      if (msg.event === 'console_update') {
        if (msg.data) this.broadcastEvent('console', JSON.stringify(msg.data));
        return;
      }
      if (msg.event === 'adsb_update') {
        if (msg.data) {
          // Filtering, trail tracking and shaping all happen here rather than
          // on the chip. RAM only: the receiver re-sends the whole fleet each
          // cycle, so nothing needs to survive a DO restart.
          const fleet = this._processAdsb(msg.data);
          this.lastAdsb = JSON.stringify(fleet);
          this.lastAdsbAt = Date.now();
          this.broadcastEvent('adsb', this.lastAdsb);
        }
        return;
      }
      if (msg.event && msg.event.startsWith('backup_')) {
        await this.handleBackupEvent(msg);
        return;
      }
      if (msg.event === 'r2_healthcheck') {
        await this._runR2Healthcheck();
        return;
      }
      if (msg.event === 'test_email') {
        await this._sendTestEmail();
        return;
      }
      if (msg.event !== 'pending_guestbook') return;
      const env = this.env;
      if (!env.SMTP2GO_KEY || !env.NOTIFY_EMAIL) return;

      const now = Date.now();
      if (now - this.lastEmailAt < 300000) return;
      this.lastEmailAt = now;

      const count = Math.max(0, parseInt(msg.count, 10) || 0);
      if (count < 1) return;
      const noun = count === 1 ? 'entry' : 'entries';

      let body = `${count} guestbook ${noun} awaiting moderation on HelloESP.\n\n`;
      if (msg.name) {
        body += `Latest:\n`;
        body += `  ${String(msg.name).slice(0, 40)}`;
        if (msg.country && msg.country !== '??') body += ` (${String(msg.country).slice(0, 3)})`;
        body += `\n`;
        if (msg.message) body += `  "${String(msg.message).slice(0, 300)}"\n`;
        body += `\n`;
      }
      body += `To review, open /admin from your LAN.`;

      const res = await this._sendEmail({
        subject: `HelloESP: ${count} pending guestbook ${noun}`,
        text_body: body
      });
      if (res && !res.ok) console.error('SMTP2GO non-2xx:', res.status);
    } catch (e) {
      console.error('SMTP2GO send failed:', e && e.message);
    }
  }

  // --- Backup streaming ---
  //
  // The device streams a backup as chunked events. Each file is written to R2
  // as soon as it completes, so a Durable Object only ever holds manifest rows
  // rather than the whole bundle.
  //
  // Buffering the bundle in DO memory was a real bug: if the DO was evicted
  // part way through a transfer, backup_end found no session, returned early
  // with no log, and nothing was ever written. The device still saw its socket
  // open and reported success.
  //
  //   state/YYYY-MM-DD/<file>         written incrementally as files complete
  //   state/YYYY-MM-DD/_manifest.json
  //   state/latest.json               written last = the commit marker
  //
  // A snapshot only counts as committed once latest.json names it, so a
  // transfer that dies half way leaves the previous snapshot intact and
  // readable.
  //
  // Rotation (GFS): 7 daily + 4 weekly (Sun) + 12 monthly (1st) + yearly
  // (Jan 1) forever. Prefix + age guards refuse to delete anything recent or
  // outside the state/YYYY-MM-DD/ namespace.

  // Filenames from the device must match a strict allowlist: alphanumeric,
  // dot/underscore/dash/slash only. This rejects path traversal (`..`), leading
  // separators, backslashes (Windows-style traversal), all control chars
  // including `\r\n` (which would corrupt manifest.json line keys), and
  // Unicode line separators (U+2028/U+2029). The segment-must-contain-an-alnum
  // check rejects degenerate names like `.` and `..`.
  static SAFE_NAME_RE = /^[A-Za-z0-9._-]+(\/[A-Za-z0-9._-]+)*$/;
  static SEGMENT_HAS_ALNUM = /(^|\/)[A-Za-z0-9]/;

  // Guard rails so one runaway file cannot exhaust the DO's heap.
  static MAX_FILE_B64 = 2 * 1024 * 1024;
  static MAX_TOTAL_BYTES = 8 * 1024 * 1024;

  pruneBackupSessions() {
    const cutoff = Date.now() - BACKUP_SESSION_IDLE;
    for (const [seq, s] of this.backupSessions) {
      if (s.startedAt < cutoff) {
        this.backupSessions.delete(seq);
        console.warn(`backup session ${seq} expired idle after ${s.entries.length} file(s); ` +
                     `${s.bytes} byte(s) already in R2`);
      }
    }
  }

  async handleBackupEvent(msg) {
    const seq = msg.seq;
    if (typeof seq !== 'number') return;

    if (msg.event === 'backup_start') {
      this.pruneBackupSessions();
      const prev = this.backupSessions.get(seq);
      if (prev) {
        console.warn(`backup session ${seq} restarted; discarding ${prev.entries.length} pending row(s)`);
      }
      this.backupSessions.set(seq, {
        startedAt: Date.now(),
        meta: {
          generated_at: String(msg.generated_at || ''),
          firmware: String(msg.firmware || ''),
          uptime: String(msg.uptime || '')
        },
        entries: [],
        emailFiles: [],       // only populated when the R2 binding is missing
        currentFile: null,
        totalB64: 0,
        chunkCount: 0,
        bytes: 0,
        aborted: false
      });
      return;
    }

    const s = this.backupSessions.get(seq);
    if (!s) {
      // Previously a silent return, which is how a lost bundle looked like a
      // success. Make it visible.
      console.warn(`backup event ${msg.event} for unknown session ${seq}`);
      return;
    }
    if (s.aborted) return;

    if (msg.event === 'backup_file_start') {
      s.currentFile = {
        name: String(msg.name || 'unknown'),
        size: Math.max(0, parseInt(msg.size, 10) || 0),
        chunks: []
      };
      s.totalB64 = 0;
      s.chunkCount = 0;
      return;
    }

    if (msg.event === 'backup_file_chunk') {
      if (!s.currentFile) return;
      const data = String(msg.data || '');
      s.currentFile.chunks.push(data);
      s.totalB64 += data.length;
      s.chunkCount++;
      if (s.totalB64 > EspRelay.MAX_FILE_B64) {
        s.aborted = true;
        this.backupSessions.delete(seq);
        console.error(`backup session ${seq} aborted: file ${s.currentFile.name} ` +
                      `exceeded ${EspRelay.MAX_FILE_B64} base64 bytes`);
      }
      return;
    }

    if (msg.event === 'backup_file_end') {
      if (!s.currentFile) return;
      const name = String(msg.name || s.currentFile.name || 'unknown');
      const declared = s.currentFile.size;
      const b64 = s.currentFile.chunks.join('');
      s.currentFile = null;

      if (!this.env.BACKUP) {
        // No bucket: keep the old whole-bundle behaviour so the email
        // attachment path still works.
        s.emailFiles.push({ name, size: declared, content_b64: b64 });
        s.entries.push({ path: name, size: declared });
        return;
      }

      const row = await this._storeBackupFile(s, name, declared, b64);
      s.entries.push(row);
      if (s.bytes > EspRelay.MAX_TOTAL_BYTES) {
        s.aborted = true;
        this.backupSessions.delete(seq);
        console.error(`backup session ${seq} aborted: bundle exceeded ` +
                      `${EspRelay.MAX_TOTAL_BYTES} bytes`);
      }
      return;
    }

    if (msg.event === 'backup_file_skipped') {
      s.entries.push({
        path: String(msg.name || 'unknown'),
        size: Math.max(0, parseInt(msg.size, 10) || 0),
        skipped: String(msg.reason || 'unknown')
      });
      return;
    }

    if (msg.event === 'backup_end') {
      const originalSize = Math.max(0, parseInt(msg.size, 10) || 0);
      this.backupSessions.delete(seq);
      if (!this.env.BACKUP) {
        await this.emailBackupBundle(s.meta, s.emailFiles, originalSize);
        return;
      }
      await this.commitBackupBundle(s, originalSize);
    }
  }

  // Decode, hash and store one file. Never throws: a failed file becomes a
  // skipped manifest row so one bad path cannot cost the whole snapshot.
  async _storeBackupFile(session, name, declaredSize, contentB64) {
    const env = this.env;
    const date = this._bucketDate(session.meta.generated_at);
    const prefix = `state/${date}/`;

    if (typeof name !== 'string' || name.length === 0 || name.length > 256
        || name.startsWith('/') || name.includes('..')
        || !EspRelay.SAFE_NAME_RE.test(name)
        || !EspRelay.SEGMENT_HAS_ALNUM.test(name)) {
      console.warn(`backup ${date}: rejecting suspicious filename:`, JSON.stringify(name));
      return { path: String(name).slice(0, 64), size: declaredSize, skipped: 'rejected_name' };
    }

    try {
      const bytes = EspRelay._b64ToBytes(contentB64);
      const hashBuf = await crypto.subtle.digest('SHA-256', bytes);
      await env.BACKUP.put(prefix + name, bytes);
      session.bytes += bytes.length;
      return {
        path: name,
        size: bytes.length,
        sha256: EspRelay._hex(new Uint8Array(hashBuf))
      };
    } catch (e) {
      const reason = (e && e.message) || String(e);
      console.error(`backup ${date}: failed to store ${name}:`, reason);
      return { path: name, size: declaredSize, skipped: 'write_failed' };
    }
  }

  // Writes the manifest and then the commit marker. This is the only place a
  // snapshot becomes "real", and it refuses to move the marker when no file
  // was actually stored.
  async commitBackupBundle(session, originalSize) {
    const env = this.env;
    const date = this._bucketDate(session.meta.generated_at);
    const prefix = `state/${date}/`;
    const entries = session.entries;
    const included = entries.filter(e => !e.skipped);
    const skipped = entries.filter(e => e.skipped);

    if (!included.length) {
      const reason = 'no files stored';
      console.error(`backup ${date}: ${reason}; leaving the commit marker where it is`);
      await this._sendBackupFailureAlert(date, reason);
      return false;
    }

    const manifest = {
      schema: 'helloesp-backup/2',
      generated_at: session.meta.generated_at,
      firmware: session.meta.firmware,
      uptime: session.meta.uptime,
      date,
      original_size: originalSize,
      files: entries
    };

    try {
      await env.BACKUP.put(prefix + '_manifest.json', JSON.stringify(manifest, null, 2), {
        httpMetadata: { contentType: 'application/json' }
      });
      await env.BACKUP.put('state/latest.json', JSON.stringify({
        date,
        files: entries.length,
        included: included.length,
        skipped: skipped.length,
        bytes: session.bytes,
        at: Date.now(),
        firmware: session.meta.firmware,
        generated_at: session.meta.generated_at
      }, null, 2), { httpMetadata: { contentType: 'application/json' } });
    } catch (e) {
      const reason = (e && e.message) || String(e);
      console.error(`backup ${date} commit failed:`, reason);
      await this._sendBackupFailureAlert(date, reason);
      return false;
    }

    this.lastBackupAt = Date.now();
    this.lastBackupDate = date;
    await this.state.storage.put('lastBackupAt', this.lastBackupAt);
    await this.state.storage.put('lastBackupDate', date);

    // Tell the device the bundle was actually stored, not just sent. The device
    // only records a confirmed backup on this message.
    if (this.espSocket && this.espSocket.readyState === 1) {
      try {
        this.espSocket.send(JSON.stringify({
          type: 'event',
          event: 'backup_committed',
          date,
          bytes: session.bytes,
          files: entries.length,
          included: included.length,
          skipped: skipped.length,
          at: this.lastBackupAt
        }));
      } catch (e) {
        console.error('backup_committed push failed:', e && e.message);
      }
    }

    // Fire-and-forget rotation. Its failure is logged but doesn't invalidate
    // the committed backup.
    this._rotateSnapshots().catch(e => console.error('rotation failed:', e && e.message));
    return true;
  }

  // --- ADS-B fleet processing ---
  //
  // The ESP32 only forwards a compact extract from the local receiver: it has no
  // CPU budget for filtering, trail history, or shaping the JSON the map wants.
  // Everything downstream of "which aircraft exist right now" lives here, in
  // the Worker, where it is cheap and shared by every viewer.
  //
  // Pipeline: normalise -> filter -> track -> cache.
  //
  //   normalise  accept both the receiver's raw aircraft.json shape and the
  //              legacy already-shaped {now, aircraft[]} the chip used to send
  //   filter     drop anything stale, on the ground, or implausible
  //   track      remember the last few fixes per ICAO hex so the map can draw
  //              trails, and expire aircraft that have gone quiet
  //   cache      the shaped fleet is what /adsb.json serves and what SSE
  //              broadcasts, so every viewer sees identical numbers

  static ADSB_STALE_MS      = 30000;   // no position feed for 30s -> drop
  static ADSB_DROP_MS       = 120000;  // silent for 2 min -> forget the trail
  static ADSB_TRAIL_POINTS  = 12;
  static ADSB_MAX_TRAIL_AGE_MS = 300000;

  // A single feed that arrives out of order (common: the receiver's own clock
  // and ours differ) must not erase a newer trail.
  _processAdsb(raw) {
    const nowMs = Date.now();
    const list = Array.isArray(raw) ? raw
               : (raw && Array.isArray(raw.aircraft)) ? raw.aircraft
               : [];

    const nowSec = (raw && typeof raw.now === 'number' && raw.now > 1e9)
      ? Math.round(raw.now)
      : Math.round(nowMs / 1000);

    const out = [];
    for (const a of list) {
      if (!a || typeof a !== 'object') continue;

      const hex = String(a.hex || a.icao || a.id || '').toLowerCase();
      if (!/^[0-9a-f]{6}$/.test(hex)) continue;

      const lat = numOrNull(a.lat);
      const lon = numOrNull(a.lon);
      const seen = numOrNull(a.seen);
      const seenAgeMs = (seen !== null && seen >= 0) ? seen * 1000
                        : (seen !== null && seen < 0) ? nowMs - (-seen)   // dump1090 relative
                        : 0;
      if (seenAgeMs > EspRelay.ADSB_STALE_MS) continue;

      // Ground vehicles carry no position worth plotting.
      if (typeof a.ground === 'string' && a.ground.toLowerCase() === 'ground') continue;
      if (a.alt_baro === 'ground') continue;

      if (lat === null || lon === null) continue;
      if (lat < -90 || lat > 90 || lon < -180 || lon > 180) continue;

      const alt = numOrNull(a.alt_baro !== undefined ? a.alt_baro : a.alt_geom)
               ?? numOrNull(a.alt);
      const spd = numOrNull(a.gs) ?? numOrNull(a.speed);
      const trk = numOrNull(a.track);

      const callsign = cleanCallsign(
        a.flight !== undefined ? a.flight
        : a.callsign !== undefined ? a.callsign : '');

      const entry = {
        hex,
        lat,
        lon,
        seen: Math.round(nowSec - seenAgeMs / 1000)
      };
      if (alt !== null) entry.alt_baro = alt;
      if (spd !== null) entry.gs = spd;
      if (trk !== null) entry.track = trk;
      if (callsign) entry.flight = callsign;
      if (typeof a.category === 'string') entry.category = a.category.toLowerCase();
      if (typeof a.rssi === 'number') entry.rssi = a.rssi;

      // --- track ---
      const prev = this.adsbTracks.get(hex);
      const trail = (prev && prev.trail) ? prev.trail.slice() : [];
      const last = trail.length ? trail[trail.length - 1] : null;
      if (!last || (nowMs - last.t) > 1000) {
        trail.push({ lat, lon, t: nowMs });
        if (trail.length > EspRelay.ADSB_TRAIL_POINTS) trail.shift();
      }
      this.adsbTracks.set(hex, { trail, lastSeenMs: nowMs });

      // Drop stale trails so the map doesn't grow without bound.
      if (trail.length) {
        while (trail.length && (nowMs - trail[0].t) > EspRelay.ADSB_MAX_TRAIL_AGE_MS) {
          trail.shift();
        }
      }
      entry.trail = trail.map(p => [round4(p.lat), round4(p.lon)]);
      if (!entry.trail.length) delete entry.trail;

      out.push(entry);
    }

    // Forget aircraft that have gone quiet.
    for (const [hex, t] of this.adsbTracks) {
      if (nowMs - t.lastSeenMs > EspRelay.ADSB_DROP_MS) this.adsbTracks.delete(hex);
    }

    return { now: nowSec, updated: nowMs, count: out.length, aircraft: out };
  }

  static _b64ToBytes(b64) {
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  static _hex(bytes) {
    let s = '';
    for (const b of bytes) s += b.toString(16).padStart(2, '0');
    return s;
  }

  _bucketDate(generated_at) {
    const m = /^(\d{4}-\d{2}-\d{2})/.exec(generated_at || '');
    return m ? m[1] : new Date().toISOString().slice(0, 10);
  }

  _shouldKeepSnapshot(dateStr, nowMs) {
    const d = new Date(dateStr + 'T00:00:00Z');
    if (isNaN(d.getTime())) return true; // malformed, err on keep
    const ageDays = Math.floor((nowMs - d.getTime()) / 86400000);
    if (ageDays < 8) return true;                                            // daily (last week)
    if (d.getUTCDay() === 0 && ageDays < 29) return true;                    // weekly (Sundays, 4wk)
    if (d.getUTCDate() === 1 && ageDays < 366) return true;                  // monthly (1st, 12mo)
    if (d.getUTCMonth() === 0 && d.getUTCDate() === 1) return true;          // yearly (Jan 1, forever)
    return false;
  }

  async _rotateSnapshots() {
    const env = this.env;
    if (!env.BACKUP) return;
    const now = Date.now();

    // Discover dated snapshot folders via list+delimiter.
    const listing = await env.BACKUP.list({ prefix: 'state/', delimiter: '/' });
    const folders = [];
    for (const p of (listing.delimitedPrefixes || [])) {
      const m = /^state\/(\d{4}-\d{2}-\d{2})\/$/.exec(p);
      if (m) folders.push(m[1]);
    }

    const toDelete = folders.filter(d => {
      if (this._shouldKeepSnapshot(d, now)) return false;
      const ageDays = Math.floor((now - new Date(d + 'T00:00:00Z').getTime()) / 86400000);
      return ageDays >= 8; // hard floor; never prune recent even if classifier says drop
    });

    for (const date of toDelete) {
      // Belt-and-suspenders: iterate each object under the date prefix and verify the key
      // shape before deleting. Refuse anything outside state/YYYY-MM-DD/.
      let cursor;
      do {
        const sub = await env.BACKUP.list({ prefix: `state/${date}/`, cursor });
        const keys = (sub.objects || [])
          .map(o => o.key)
          .filter(k => /^state\/\d{4}-\d{2}-\d{2}\//.test(k));
        if (keys.length) await env.BACKUP.delete(keys);
        cursor = sub.truncated ? sub.cursor : undefined;
      } while (cursor);
    }
  }

  _backupAlertsOff() {
    const v = String(this.env.BACKUP_ALERTS ?? '').trim().toLowerCase();
    return v === 'off' || v === '0' || v === 'false' || v === 'no';
  }

  async _sendBackupFailureAlert(date, reason) {
    const env = this.env;
    if (!env.SMTP2GO_KEY || !env.NOTIFY_EMAIL) return;
    if (this._backupAlertsOff()) return;
    const now = Date.now();
    if (now < this.maintenanceUntil) return;
    if (now - this.lastBackupFailureEmailAt < 3600000) return; // one per hour at most
    this.lastBackupFailureEmailAt = now;
    await this.state.storage.put('lastBackupFailureEmailAt', now).catch(() => {});
    try {
      await this._sendEmail({
        subject: `HelloESP backup FAILED - ${date}`,
        text_body: `Backup for ${date} could not be written to R2.\n\nReason: ${reason}\n\nCheck the R2 bucket and Worker logs.`
      });
    } catch (e) {
      console.error('backup failure email send failed:', e && e.message);
    }
  }

  // Admin-triggered SMTP2GO test. Sends a harmless test email; reports back whether SMTP2GO
  // accepted it. Catches silent failures (wrong key, unverified sender, etc.) before a real
  // alert needs to fire.
  async _sendTestEmail() {
    const env = this.env;
    const sendResult = (pass, detail) => {
      if (this.espSocket && this.espSocket.readyState === 1) {
        try {
          // ESP uses naive indexOf-based JSON field extraction, so strip any characters it
          // can't round-trip (embedded quotes / backslashes / control chars would terminate early).
          const safe = String(detail).replace(/["\\\n\r\t\x00-\x1f]/g, ' ').slice(0, 128);
          this.espSocket.send(JSON.stringify({
            type: 'event',
            event: 'test_email_result',
            pass,
            detail: safe
          }));
        } catch (e) {
          console.error('test_email_result push failed:', e && e.message);
        }
      }
    };

    if (!env.SMTP2GO_KEY) { sendResult(false, 'SMTP2GO_KEY not set'); return; }
    if (!env.NOTIFY_EMAIL) { sendResult(false, 'NOTIFY_EMAIL not set'); return; }

    try {
      const res = await this._sendEmail({
        subject: 'HelloESP test email',
        text_body: `This is a manual test sent from the admin panel at ${new Date().toISOString()}.\n\nIf you received this, SMTP2GO is working. Dead-man, backup, and guestbook alerts will use the same path.`
      });
      if (!res || !res.ok) {
        let bodyText = '';
        try { if (res) bodyText = (await res.text()).slice(0, 100); } catch (e) {}
        sendResult(false, `SMTP2GO HTTP ${res ? res.status : 'no-response'}${bodyText ? ': ' + bodyText : ''}`);
        return;
      }
      sendResult(true, `sent to ${env.NOTIFY_EMAIL}`);
    } catch (e) {
      sendResult(false, 'fetch failed: ' + ((e && e.message) || String(e)));
    }
  }

  // Admin-triggered R2 liveness test. PUTs a small file, reads it back byte-for-byte, deletes.
  // Sends the outcome back to the ESP as an event so the admin UI can display it.
  // The test key lives outside the state/YYYY-MM-DD/ rotation namespace, so rotation won't touch it.
  async _runR2Healthcheck() {
    const env = this.env;
    const sendResult = (pass, detail) => {
      if (this.espSocket && this.espSocket.readyState === 1) {
        try {
          const safe = String(detail).replace(/["\\\n\r\t\x00-\x1f]/g, ' ').slice(0, 128);
          this.espSocket.send(JSON.stringify({
            type: 'event',
            event: 'r2_healthcheck_result',
            pass,
            detail: safe
          }));
        } catch (e) {
          console.error('r2_healthcheck_result push failed:', e && e.message);
        }
      }
    };

    if (!env.BACKUP) {
      sendResult(false, 'R2 binding not configured');
      return;
    }

    const key = 'state/_healthcheck/test.txt';
    const payload = `helloesp r2 healthcheck ${Date.now()}`;

    try {
      await env.BACKUP.put(key, payload);
    } catch (e) {
      sendResult(false, 'PUT failed: ' + ((e && e.message) || String(e)));
      return;
    }
    try {
      const obj = await env.BACKUP.get(key);
      if (!obj) { sendResult(false, 'GET returned null'); return; }
      const text = await obj.text();
      if (text !== payload) { sendResult(false, `readback mismatch (${text.length} vs ${payload.length} bytes)`); return; }
    } catch (e) {
      sendResult(false, 'GET failed: ' + ((e && e.message) || String(e)));
      return;
    }
    try {
      await env.BACKUP.delete(key);
    } catch (e) {
      // non-fatal: put/get confirmed the bucket works. Leftover test file gets overwritten next run.
      console.warn('r2 healthcheck delete failed:', e && e.message);
    }
    sendResult(true, 'put/get/delete ok');
  }

  async _claimMissedBackupAlert(referenceTime, now) {
    return this.state.storage.transaction(async txn => {
      const previous = await txn.get('missedBackupAlertState');
      const current = previous && previous.referenceTime === referenceTime ? previous : null;
      const legacyLastAt = previous ? 0 : (await txn.get('lastBackupMissedEmailAt') || 0);
      const nextAt = current ? current.nextAt : legacyLastAt + 86400000;
      if (now < nextAt) return false;
      const count = current ? current.count : (legacyLastAt ? 1 : 0);
      const delay = Math.min(30, 2 ** Math.min(count, 5)) * 86400000;
      await txn.put({
        missedBackupAlertState: { referenceTime, count: Math.min(count + 1, 6), nextAt: now + delay },
        lastBackupMissedEmailAt: now
      });
      return true;
    });
  }

  async _maybeSendMissedBackupAlert() {
    const env = this.env;
    if (!env.SMTP2GO_KEY || !env.NOTIFY_EMAIL) return;
    if (this._backupAlertsOff()) return;
    const now = Date.now();
    if (now < this.maintenanceUntil) return;
    if (!this.espSocket || this.espSocket.readyState !== 1 || !this.hmacAuthenticated) return;
    if (this.lastActivity <= 0 || now - this.lastActivity > 75000) return;
    if (this.deadmanAlertSent) return;
    // Use lastBackupAt if any successful backup has happened; otherwise use
    // the DO's first-seen time as the reference. Without this, a fresh
    // deploy that never gets a successful backup would never alert.
    const referenceTime = this.lastBackupAt || this.firstSeenAt;
    if (!referenceTime) return;
    const ageMs = now - referenceTime;
    if (ageMs < 48 * 3600000) return;                              // fresh
    if (!await this._claimMissedBackupAlert(referenceTime, now)) return;
    this.lastBackupMissedEmailAt = now;
    const ageHours = Math.floor(ageMs / 3600000);
    const neverHadOne = !this.lastBackupAt;
    try {
      await this._sendEmail({
        subject: neverHadOne
          ? `HelloESP backup never succeeded (${ageHours}h since deploy)`
          : `HelloESP backup overdue (${ageHours}h)`,
        text_body: neverHadOne
          ? `Worker has been running for ${ageHours} hours and no R2 backup has ever succeeded.\n\nCheck that the R2 binding is configured (wrangler.toml) and the device is uploading bundles.`
          : `No successful backup since ${new Date(this.lastBackupAt).toISOString()}.\n\nLast snapshot date: ${this.lastBackupDate || 'unknown'}.\n\nCheck that the device is online and the R2 binding is healthy.`
      });
    } catch (e) {
      console.error('missed-backup email send failed:', e && e.message);
    }
  }

  async emailBackupBundle(meta, files, originalSize) {
    const env = this.env;
    if (!env.SMTP2GO_KEY || !env.NOTIFY_EMAIL) {
      console.warn('backup bundle received but SMTP2GO_KEY or NOTIFY_EMAIL unset');
      return;
    }

    const bundle = {
      schema: 'helloesp-backup/1',
      generated_at: meta.generated_at,
      firmware: meta.firmware,
      uptime: meta.uptime,
      original_size: originalSize,
      files
    };
    const bundleJson = JSON.stringify(bundle);
    // BACKUP_PART_SIZE is a byte budget (SMTP attachment ceiling). Slicing
    // bundleJson.slice() slices by UTF-16 code units, so any non-ASCII content
    // (e.g. a guestbook entry with an em-dash) would make a part over-size or
    // split a code point. Encode once and slice the resulting byte buffer.
    const bundleBytes = new TextEncoder().encode(bundleJson);
    const date = (meta.generated_at || new Date().toISOString()).slice(0, 10);
    // 6-char unique session token for grouping multipart email backups.
    // Not security-critical (just a uniqueness key) but switched to
    // crypto-RNG to satisfy CodeQL's insecure-randomness check across the codebase.
    const sessionId = crypto.randomUUID().replace(/-/g, '').slice(0, 6);
    const totalParts = Math.max(1, Math.ceil(bundleBytes.length / BACKUP_PART_SIZE));
    const padWidth = String(totalParts).length;

    const skipped = files.filter(f => f.skipped);
    const included = files.filter(f => !f.skipped);

    let header = `HelloESP weekly state backup.\n\n`;
    header += `Generated:   ${meta.generated_at || 'n/a'}\n`;
    header += `Firmware:    ${meta.firmware || 'n/a'}\n`;
    header += `Uptime:      ${meta.uptime || 'n/a'}\n`;
    header += `Files:       ${included.length} included, ${skipped.length} skipped\n`;
    header += `Bundle size: ${(bundleBytes.length / 1024).toFixed(1)} KB total\n`;
    header += `Raw size:    ${(originalSize / 1024).toFixed(1)} KB (on device)\n`;
    if (totalParts > 1) header += `Parts:       ${totalParts} emails (session ${sessionId})\n`;
    header += `\n`;
    if (skipped.length) {
      header += `Skipped:\n`;
      for (const f of skipped) header += `  - ${f.name} (${f.size} bytes, ${f.skipped})\n`;
      header += `\n`;
    }

    let footer;
    if (totalParts === 1) {
      footer = `Contents are base64-encoded inside the JSON. To restore a specific file:\n`;
      footer += `  jq -r '.files[] | select(.name=="guestbook.csv") | .content_b64' backup.json | base64 -d > guestbook.csv\n`;
    } else {
      footer = `This bundle is split across ${totalParts} emails. Download every attachment,\n`;
      footer += `then reassemble and restore:\n`;
      footer += `  cat helloesp-backup-${date}-${sessionId}-part*.json > bundle.json\n`;
      footer += `  jq -r '.files[] | select(.name=="guestbook.csv") | .content_b64' bundle.json | base64 -d > guestbook.csv\n`;
      footer += `\nIf a part is missing, the device retries the full backup next week.\n`;
    }

    let sentParts = 0;
    for (let i = 0; i < totalParts; i++) {
      const bytes = bundleBytes.subarray(i * BACKUP_PART_SIZE, (i + 1) * BACKUP_PART_SIZE);
      let bin = '';
      const CHUNK = 0x8000;
      for (let j = 0; j < bytes.length; j += CHUNK) {
        bin += String.fromCharCode.apply(null, bytes.subarray(j, j + CHUNK));
      }
      const attachmentB64 = btoa(bin);

      const partNum = String(i + 1).padStart(padWidth, '0');
      const totalStr = String(totalParts).padStart(padWidth, '0');
      const filename = totalParts === 1
        ? `helloesp-backup-${date}.json`
        : `helloesp-backup-${date}-${sessionId}-part${partNum}of${totalStr}.json`;
      const subject = totalParts === 1
        ? `HelloESP backup - ${date} (${(bundleBytes.length / 1024).toFixed(0)} KB)`
        : `HelloESP backup - ${date} (part ${i + 1}/${totalParts})`;

      let body = header;
      if (totalParts > 1) {
        body += `>>> This is part ${i + 1} of ${totalParts}. Slice offset: ${i * BACKUP_PART_SIZE} of ${bundleBytes.length} bytes.\n\n`;
      }
      body += footer;

      try {
        const res = await this._sendEmail({
          subject,
          text_body: body,
          attachments: [{
            filename,
            fileblob: attachmentB64,
            mimetype: 'application/json'
          }]
        });
        if (!res || !res.ok) {
          console.error(`backup part ${i + 1}/${totalParts} SMTP2GO non-2xx:`, res ? res.status : 'no-response');
          continue;
        }
        sentParts++;
      } catch (e) {
        console.error(`backup part ${i + 1}/${totalParts} failed:`, e && e.message);
      }

      if (i < totalParts - 1) await new Promise(r => setTimeout(r, BACKUP_PART_DELAY_MS));
    }

    if (sentParts === totalParts) {
      this.lastBackupAt = Date.now();
      this.lastBackupDate = date;
      await this.state.storage.put('lastBackupAt', this.lastBackupAt);
      await this.state.storage.put('lastBackupDate', date);
    } else {
      console.error(`backup partial: ${sentParts}/${totalParts} parts emailed`);
    }
  }

  async alarm() {

    // Daily DO storage snapshot to R2. Once-per-UTC-day catch-all so any
    // DO state (weather/AQ caches, operational flags) is recoverable from
    // yesterday if DO ever gets wiped. Short-circuits cheaply after the
    // first call each day.
    this._maybeDoSnapshot().catch(e =>
      console.error('do-snapshot alarm failed:', e && e.message));

    // lazy weather refresh: fetch on first tick, then every WEATHER_REFRESH_MS (1 hour)
    if (!this.lastWeather || Date.now() - this.lastWeather.fetched_at > WEATHER_REFRESH_MS) {
      this.refreshWeather().catch(() => {});
    }
    // Same cadence for air quality (Open-Meteo updates both hourly).
    if (!this.lastAirQuality || Date.now() - this.lastAirQuality.fetched_at > WEATHER_REFRESH_MS) {
      this.refreshAirQuality().catch(() => {});
    }

    // dead-man's-switch: email if ESP has been silent for >24h
    this.maybeSendDeadmanAlert().catch(() => {});

    await this._maybeSendMissedBackupAlert().catch(e =>
      console.error('missed-backup alert failed:', e && e.message));

    // dead-client sweep: if ESP isn't pushing events, broadcasts don't prune dead SSE writers.
    // Send a zero-cost SSE comment to every client; prune any that throw.
    if (this.sseClients.size > 0) {
      const ping = new TextEncoder().encode(': ping\n\n');
      const dead = [];
      for (const w of this.sseClients) {
        w.write(ping).catch(() => dead.push(w));
      }
      for (const w of dead) {
        this.sseClients.delete(w);
        w.abort().catch(() => {});
      }
    }

    if (this.espSocket && Date.now() - this.lastActivity > 75000) {
      try { this.espSocket.close(); } catch (e) {}
      this.espSocket = null;
    }

    // Sweep orphan backup sessions whose device dropped mid-stream. Without
    // this, a half-finished session sits in memory until the next backup_start
    // (up to 24h on the daily cadence). Bounded but wasteful.
    this.pruneBackupSessions();

    // Time-based eviction of expired rate-limit and ws-auth-fail entries.
    // The opportunistic size-gated sweep inside _enforceRateLimit only fires
    // past 500 entries; on long stretches of low traffic, expired buckets
    // would otherwise linger indefinitely below that threshold.
    {
      const t = Date.now();
      for (const [k, v] of this.rateLimits) {
        if (v.resetAt < t) this.rateLimits.delete(k);
      }
      for (const [k, v] of this.wsAuthFails) {
        if (v.blockedUntil < t && t - v.firstAt > 600000) this.wsAuthFails.delete(k);
      }
    }

    // Persist lastActivity so isolate eviction doesn't blank the deadman state.
    // Throttled to ~30s (alarm cadence). One write per tick; storage cost is
    // negligible vs the cost of a missed deadman alert.
    if (this.lastActivity > 0) {
      this.state.storage.put('lastActivity', this.lastActivity).catch(() => {});
    }


    // Always rearm. Without this, an offline-ESP + no-SSE-clients state would stop the alarm
    // loop, and dead-man / overdue-backup alerts would be delayed until the next visitor woke
    // the DO. ~86k invocations/month is well under Worker free-tier limits. Direct setAlarm
    // here (not _ensureAlarm). This IS the rearm: we want a fresh 30s window, not to defer
    // to a farther-out existing alarm.
    await this.state.storage.setAlarm(Date.now() + 30000);
  }

  async fetch(request) {
    const url = new URL(request.url);

    // curl/wget/httpie/PowerShell hitting "/" get a text/plain ASCII stats
    // card built from the cached lastStats. Zero ESP load, works when the
    // device is down, no-store so it doesn't poison "/" for browser visits.
    if (request.method === 'GET' && (url.pathname === '/' || url.pathname === '')) {
      const ua = (request.headers.get('User-Agent') || '').toLowerCase();
      if (/\b(curl|wget|httpie|libwww-perl|powershell)\b/.test(ua)) {
        return new Response(this.buildCurlCard(), {
          status: 200,
          headers: {
            'Content-Type': 'text/plain; charset=utf-8',
            'Cache-Control': 'no-store',
            ...SEC_HEADERS
          }
        });
      }
    }

    // Worker-side load-shedding endpoints.
    if (url.pathname === '/ping' && request.method === 'GET') {
      const espUp = !!(this.espSocket && this.espSocket.readyState === 1);
      return new Response(espUp ? 'pong' : 'offline', {
        status: espUp ? 200 : 503,
        headers: {
          'Content-Type': 'text/plain',
          'Cache-Control': 'no-store',
          ...SEC_HEADERS
        }
      });
    }

    // /stats: serve from Worker's cached lastStats (pushed by ESP via SSE events).
    if (url.pathname === '/stats' && request.method === 'GET') {
      if (this.lastStats) {
        const age = Date.now() - this.lastStatsAt;
        // Inject fresh sseClients.size for the live-presence indicator.
        // Stays out of lastStats itself so cache hits / cold-relay paths
        // also pick up the current count rather than a stale snapshot.
        let body = this.lastStats;
        try {
          const parsed = JSON.parse(this.lastStats);
          parsed.clients = this.sseClients.size;
          body = JSON.stringify(parsed);
        } catch (e) { /* fall through to raw */ }
        return new Response(body, {
          status: 200,
          headers: {
            'Content-Type': 'application/json',
            'Cache-Control': 'public, max-age=5, stale-while-revalidate=30',
            'X-Worker-Cache-Age': String(Math.floor(age / 1000)),
            ...SEC_HEADERS
          }
        });
      }
      // No cached stats yet; fall through to the ESP relay below.
    }

    // Embeddable live status badges. Uses cached lastStats; zero ESP load.
    if (url.pathname === '/status.svg') {
      const metric = url.searchParams.get('metric') || 'uptime';
      const svg = this.buildStatusBadge(metric);
      return new Response(svg, {
        status: 200,
        headers: {
          'Content-Type': 'image/svg+xml; charset=utf-8',
          'Cache-Control': 'public, max-age=60',
          ...SEC_HEADERS
        }
      });
    }

    // DO Storage Explorer: read-only inspection plus delete-by-key for the
    // worker's Durable Object storage. Used by the admin panel's DO Explorer
    // section. Auth via Authorization: Bearer WORKER_SECRET header so the
    // secret never appears in URLs (history, referrer, server logs). Same
    // trust model as other admin tooling: LAN admin page reaches across
    // origin to the worker, operator pastes secret once per browser session.
    // Per-IP rate limit prevents a leaked secret from being used for bulk
    // scraping. No write/put endpoint, only read + delete: editing arbitrary
    // DO state from a UI is too easy to misuse and not needed for debugging.
    if (url.pathname.startsWith('/admin/do/')) {
      const corsHdrs = {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'GET, DELETE, OPTIONS',
        'Access-Control-Allow-Headers': 'Authorization',
        'Access-Control-Max-Age': '600',
      };
      if (request.method === 'OPTIONS') {
        return new Response(null, { status: 204, headers: corsHdrs });
      }
      const authHeader = request.headers.get('Authorization') || '';
      const bm = authHeader.match(/^Bearer\s+(.+)$/);
      const providedKey = bm ? bm[1] : '';
      if (!providedKey || !timingSafeEqualStr(providedKey, this.env.WORKER_SECRET || '')) {
        return new Response('Unauthorized', { status: 401, headers: corsHdrs });
      }
      // Per-IP rate limit. 30/min for DO ops.
      // pattern. Bulk scraping a 1000-key DO would still take ~30 minutes
      // even with a leaked secret, giving time to rotate.
      const dIP = request.headers.get('CF-Connecting-IP') || 'unknown';
      const dNow = Date.now();
      let drl = this.rateLimits.get('doadmin:' + dIP);
      if (!drl || dNow > drl.resetAt) {
        drl = { count: 0, resetAt: dNow + 60000 };
        this.rateLimits.set('doadmin:' + dIP, drl);
      }
      drl.count++;
      if (drl.count > 30) {
        return new Response('Rate limit exceeded; try again in a minute',
          { status: 429, headers: corsHdrs });
      }
      const jsonHdrs = { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...corsHdrs };
      if (url.pathname === '/admin/do/list' && request.method === 'GET') {
        const prefix = url.searchParams.get('prefix') || '';
        const limit = Math.min(1000, Math.max(1, parseInt(url.searchParams.get('limit') || '500', 10)));
        if (prefix.length > 100) return new Response('Bad prefix', { status: 400, headers: corsHdrs });
        const opts = { limit };
        if (prefix) opts.prefix = prefix;
        const list = await this.state.storage.list(opts);
        const items = [];
        for (const [k, v] of list) {
          let size = 0;
          try { size = JSON.stringify(v).length; } catch (e) {}
          items.push({ key: k, size });
        }
        return new Response(JSON.stringify({ items, count: items.length }), { status: 200, headers: jsonHdrs });
      }
      if (url.pathname === '/admin/do/get' && request.method === 'GET') {
        const k = url.searchParams.get('k') || '';
        if (!k || k.length > 200) return new Response('Bad key', { status: 400, headers: corsHdrs });
        const val = await this.state.storage.get(k);
        if (val === undefined) return new Response('Key not found', { status: 404, headers: corsHdrs });
        return new Response(JSON.stringify({ key: k, value: val }), { status: 200, headers: jsonHdrs });
      }
      if (url.pathname === '/admin/do/delete' && request.method === 'DELETE') {
        const k = url.searchParams.get('k') || '';
        if (!k || k.length > 200) return new Response('Bad key', { status: 400, headers: corsHdrs });
        const existed = await this.state.storage.delete(k);
        console.log('do/delete:', k, 'existed=', existed);
        return new Response(JSON.stringify({ deleted: k, existed: !!existed }), { status: 200, headers: jsonHdrs });
      }
      return new Response('Not found', { status: 404, headers: corsHdrs });
    }

    // The fleet is processed and cached here, so serve it locally rather than
    // relaying to the chip. Falls back to the device when the cache is cold.
    if (url.pathname === '/adsb.json') {
      if (this.lastAdsb && (Date.now() - this.lastAdsbAt) < 60000) {
        return new Response(this.lastAdsb, {
          status: 200,
          headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }
        });
      }
    }

    if (url.pathname === '/status-wide.svg') {
      const svg = this.buildStatusWide();
      return new Response(svg, {
        status: 200,
        headers: {
          'Content-Type': 'image/svg+xml; charset=utf-8',
          'Cache-Control': 'public, max-age=60',
          ...SEC_HEADERS
        }
      });
    }

    // SSE fanout: browsers connect here and receive push updates the ESP sends via WS
    if (url.pathname === '/_stream') {
      // Cap concurrent SSE writers. Without this a flood could hoard DO memory (each writer
      // holds stream buffer state). 503 tells well-behaved clients to back off and retry.
      if (this.sseClients.size >= SSE_MAX_CLIENTS) {
        return new Response('Too many live connections, try again shortly', {
          status: 503,
          headers: { 'Content-Type': 'text/plain', 'Retry-After': '30', ...SEC_HEADERS }
        });
      }
      const { readable, writable } = new TransformStream();
      const writer = writable.getWriter();
      this.sseClients.add(writer);
      // make sure the alarm is ticking so dead-client sweeps run
      this._ensureAlarm(30000);
      // immediately send the most recent cached stats so new clients don't wait 5s.
      // Inject fresh clients count (same pattern as the broadcast/cache paths)
      // so the very first stats event a new viewer receives populates the
      // live-presence indicator. Without this, the indicator stayed hidden
      // until the next 15s ESP push.
      if (this.lastStats) {
        let body = this.lastStats;
        try {
          const parsed = JSON.parse(this.lastStats);
          parsed.clients = this.sseClients.size;
          body = JSON.stringify(parsed);
        } catch (e) { /* fall through to raw */ }
        const payload = new TextEncoder().encode(`event: stats\ndata: ${body}\n\n`);
        writer.write(payload).catch(() => {
          this.sseClients.delete(writer);
          writer.abort().catch(() => {});
        });
      }
      // Replay the last ADS-B fleet so the /adsb page and homepage strip
      // render instantly instead of waiting up to 5s for the next push.
      if (this.lastAdsb) {
        const payload = new TextEncoder().encode(`event: adsb\ndata: ${this.lastAdsb}\n\n`);
        writer.write(payload).catch(() => {
          this.sseClients.delete(writer);
          writer.abort().catch(() => {});
        });
      } else {
        // send a zero-length comment so the connection is established promptly
        writer.write(new TextEncoder().encode(': connected\n\n')).catch(() => {
          this.sseClients.delete(writer);
          writer.abort().catch(() => {});
        });
      }
      return new Response(readable, {
        status: 200,
        headers: {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-store',
          'X-Accel-Buffering': 'no',
          ...SEC_HEADERS
        }
      });
    }

    if (url.pathname === '/_ws' && request.headers.get('Upgrade') === 'websocket') {
      const wsIP = request.headers.get('CF-Connecting-IP') || 'unknown';
      const now = Date.now();
      let wf = this.wsAuthFails.get(wsIP);
      if (wf && now < wf.blockedUntil) {
        return new Response('Too many failed attempts', { status: 429 });
      }

      const key = url.searchParams.get('key');
      if (!key || !timingSafeEqualStr(key, this.env.WORKER_SECRET || '')) {
        if (!wf) wf = { count: 0, firstAt: now, blockedUntil: 0 };
        if (now - wf.firstAt > 60000) { wf.count = 0; wf.firstAt = now; }
        wf.count++;
        if (wf.count >= 5) wf.blockedUntil = now + 600000;
        this.wsAuthFails.set(wsIP, wf);
        return new Response('Unauthorized', { status: 403 });
      }
      this.wsAuthFails.delete(wsIP);

      // opportunistic cleanup of stale auth-fail entries (mirrors rateLimits cleanup)
      if (this.wsAuthFails.size > 500) {
        for (const [k, v] of this.wsAuthFails) {
          if (now - v.firstAt > 600000) this.wsAuthFails.delete(k);
        }
      }

      // Valid auth; take over any existing socket (covers stale sockets from
      // unclean reboots). We must explicitly fail any in-flight requests
      // here: the old socket's close-listener (line ~1925) short-circuits
      // when `this.espSocket !== oldServer`, which becomes true the moment
      // we null/replace this.espSocket below. So the listener won't drain
      // pendingRequests on its own and they'd hang for the 30s timeout.
      if (this.espSocket) {
        if (this.pendingRequests && this.pendingRequests.size) {
          for (const p of this.pendingRequests.values()) {
            try { p.resolve(offlineResponse()); } catch (e) {}
          }
          this.pendingRequests.clear();
        }
        try { this.espSocket.close(); } catch (e) {}
        this.espSocket = null;
      }
      // reset auth flag; each new connection must pass HMAC (or fall back if no HMAC_SECRET)
      this.hmacAuthenticated = false;
      if (this.pendingAuth) {
        clearTimeout(this.pendingAuth.timer);
        this.pendingAuth = null;
      }
      const [client, server] = Object.values(new WebSocketPair());
      server.accept();
      this.espSocket = server;
      this.lastActivity = Date.now();

      // Optional HMAC handshake. If HMAC_SECRET is set, require the ESP to
      // respond to an auth_challenge before trusting the connection.
      if (this.env.HMAC_SECRET) {
        const nonce = crypto.randomUUID();
        this.pendingAuth = {
          nonce,
          socket: server,
          timer: setTimeout(() => {
            if (this.pendingAuth && this.pendingAuth.socket === server) {
              console.error('HMAC auth timeout');
              try { server.close(); } catch (e) {}
              this.pendingAuth = null;
              if (this.espSocket === server) this.espSocket = null;
            }
          }, 5000)
        };
        server.send(JSON.stringify({ type: 'auth_challenge', nonce }));
      } else {
        console.warn('HMAC_SECRET unset; ESP WS auth is WORKER_SECRET-only (development mode).');
        this.hmacAuthenticated = true;
      }

      server.addEventListener('message', (event) => {
        // Only count activity once HMAC is verified. Pre-auth, an attacker
        // hammering the WS upgrade could indefinitely refresh this timestamp
        // and silently mask the deadman alert on a truly-dead device.
        if (this.hmacAuthenticated) this.lastActivity = Date.now();
        const data = event.data;
        if (typeof data !== 'string') return;

        // HMAC auth response. Parse first (cheap, bounded by WS frame size), then
        // type-check; avoids relying on a substring match to decide whether to parse.
        if (this.pendingAuth && this.pendingAuth.socket === server && data.charAt(0) === '{') {
          let authMsg = null;
          try { authMsg = JSON.parse(data); } catch (e) { /* not JSON; fall through */ }
          if (authMsg && authMsg.type === 'auth_response') {
            const nonce = this.pendingAuth.nonce;
            if (!authMsg.hmac) {
              try { server.send(JSON.stringify({ type: 'auth_result', ok: false, reason: 'missing hmac' })); } catch (e) {}
              try { server.close(); } catch (e) {}
              this.espSocket = null;
              clearTimeout(this.pendingAuth.timer);
              this.pendingAuth = null;
              return;
            }
            this.verifyHmac(authMsg.hmac, nonce).then((valid) => {
              if (this.espSocket !== server) return;
              if (!valid) {
                console.error('HMAC mismatch');
                try { server.send(JSON.stringify({ type: 'auth_result', ok: false, reason: 'hmac mismatch' })); } catch (e) {}
                try { server.close(); } catch (e) {}
                this.espSocket = null;
              } else {
                this.hmacAuthenticated = true;
              }
              if (this.pendingAuth && this.pendingAuth.socket === server) {
                clearTimeout(this.pendingAuth.timer);
                this.pendingAuth = null;
              }
            }).catch((e) => {
              console.error('verifyHmac error:', e && e.message);
              if (this.espSocket === server) {
                try { server.close(); } catch (e2) {}
                this.espSocket = null;
              }
              if (this.pendingAuth && this.pendingAuth.socket === server) {
                clearTimeout(this.pendingAuth.timer);
                this.pendingAuth = null;
              }
            });
            return;
          }
        }
        // Reject all other messages until HMAC verified
        if (!this.hmacAuthenticated) return;

        if (data.length === 0) {
          if (this.currentStreamId === null) return;
          const ar = this.activeResponses.get(this.currentStreamId);
          if (!ar) { this.currentStreamId = null; return; }

          const totalLen = ar.chunks.reduce((s, c) => s + c.length, 0);
          const assembled = new Uint8Array(totalLen);
          let offset = 0;
          for (const chunk of ar.chunks) {
            assembled.set(chunk, offset);
            offset += chunk.length;
          }

          const headers = { 'Content-Type': ar.ct };
          if (ar.cc) headers['Cache-Control'] = ar.cc;
          if (ar.ce) headers['Content-Encoding'] = ar.ce;

          const pending = this.pendingRequests.get(ar.id);
          if (pending) {
            // encodeBody: 'manual' is required when ar.ce is set (gzipped
            // body) so the Workers runtime ships the bytes as-is without
            // re-encoding/decompressing on outer-worker arrayBuffer() reads.
            // For plain bodies the default ('auto') is fine and stays in
            // the streaming code path.
            const respInit = ar.ce
              ? { status: ar.status, headers, encodeBody: 'manual' }
              : { status: ar.status, headers };
            pending.resolve(new Response(assembled.buffer, respInit));
            this.pendingRequests.delete(ar.id);
          }
          this.activeResponses.delete(this.currentStreamId);
          this.currentStreamId = null;
          return;
        }

        if (data.charAt(0) === '{') {
          try {
            const msg = JSON.parse(data);
            if (msg.type === 'event') {
              this.handleEvent(msg).catch((e) => console.error('handleEvent error:', e && e.message));
              return;
            }
            // Drop metadata for IDs we never asked for. Without this guard, a
            // buggy or replayed frame could accumulate orphan entries in
            // activeResponses that only get cleared on full WS close.
            if (typeof msg.id !== 'number' || !this.pendingRequests.has(msg.id)) {
              console.warn('dropping stream metadata for unknown id:', msg.id);
              return;
            }
            this.activeResponses.set(msg.id, {
              id: msg.id,
              status: msg.status || 200,
              ct: msg.ct || 'text/html',
              cc: msg.cc || '',
              ce: msg.ce || '',
              chunks: []
            });
            this.currentStreamId = msg.id;
            return;
          } catch (e) {
            console.warn('malformed stream metadata frame:', e && e.message);
          }
        }

        if (this.currentStreamId !== null) {
          const ar = this.activeResponses.get(this.currentStreamId);
          if (ar) {
            try {
              ar.chunks.push(b64ToBytes(data));
            } catch (e) {
              console.error('b64 decode error:', e && e.message);
            }
          }
        }
      });

      server.addEventListener('close', () => {
        // Only clear state if THIS socket is still the active one.
        // A takeover reconnect may have already replaced us with a new socket.
        if (this.espSocket !== server) return;
        this.espSocket = null;
        this.hmacAuthenticated = false;
        if (this.pendingAuth && this.pendingAuth.socket === server) {
          clearTimeout(this.pendingAuth.timer);
          this.pendingAuth = null;
        }
        for (const p of this.pendingRequests.values()) {
          p.resolve(offlineResponse());
        }
        this.pendingRequests.clear();
        this.activeResponses.clear();
        this.currentStreamId = null;
      });

      return new Response(null, { status: 101, webSocket: client });
    }

    // Remote maintenance control (works even when ESP is offline).
    // Lets the owner silence dead-man / overdue-backup alerts while the
    // device is intentionally offline (travel, hardware down, etc.) without
    // needing the ESP to be up to relay the WS `maintenance` event.
    // Auth: Bearer WORKER_SECRET (same trust model as /admin/do/*).
    if (url.pathname === '/admin/maintenance') {
      const corsM = {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
        'Access-Control-Allow-Headers': 'Authorization, Content-Type',
        'Access-Control-Max-Age': '600',
      };
      if (request.method === 'OPTIONS') {
        return new Response(null, { status: 204, headers: corsM });
      }
      const authH = request.headers.get('Authorization') || '';
      const mm = authH.match(/^Bearer\s+(.+)$/);
      const provided = mm ? mm[1] : '';
      if (!provided || !timingSafeEqualStr(provided, this.env.WORKER_SECRET || '')) {
        return new Response('Unauthorized', { status: 401, headers: corsM });
      }
      if (request.method === 'GET') {
        const remaining = Math.max(0, this.maintenanceUntil - Date.now());
        return new Response(JSON.stringify({
          maintenanceUntil: this.maintenanceUntil,
          maintenanceMessage: this.maintenanceMessage,
          remainingMs: remaining,
          active: remaining > 0
        }), { status: 200, headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...corsM } });
      }
      if (request.method === 'POST') {
        let body;
        try { body = await request.json(); } catch (e) {
          return new Response('Invalid JSON', { status: 400, headers: corsM });
        }
        const minutes = Math.max(0, parseInt(body.minutes, 10) || 0);
        const message = String(body.message || '').slice(0, 200);
        await this.setMaintenance(minutes, message);
        const remaining = Math.max(0, this.maintenanceUntil - Date.now());
        return new Response(JSON.stringify({
          ok: true,
          maintenanceUntil: this.maintenanceUntil,
          remainingMs: remaining
        }), { status: 200, headers: { 'Content-Type': 'application/json', ...corsM } });
      }
      return new Response('Method not allowed', { status: 405, headers: corsM });
    }

    // maintenance window takes precedence over offline, so planned work shows the right page
    if (Date.now() < this.maintenanceUntil) {
      return maintenanceResponse(this.maintenanceUntil, this.maintenanceMessage);
    }

    if (!this.espSocket || !this.hmacAuthenticated) return offlineResponse();

    const clientIP = request.headers.get('CF-Connecting-IP') || 'unknown';
    const limited = this._enforceRateLimit(clientIP);
    if (limited) return limited;

    const id = ++this.requestId;

    // request.cf.country is the canonical source inside a Worker; the CF-IPCountry HTTP header
    // isn't forwarded to Workers by default, so reading it via headers.get always returned '' and
    // every relayed request showed up as ?? in the console.
    const headers = {
      'CF-Connecting-IP': clientIP,
      'CF-IPCountry': (request.cf && request.cf.country) || request.headers.get('CF-IPCountry') || '',
      'Accept-Encoding': request.headers.get('Accept-Encoding') || ''
    };

    let body = '';
    if (request.method === 'POST') {
      const cl = request.headers.get('Content-Length');
      if (cl && parseInt(cl, 10) > MAX_BODY) {
        return new Response('Payload too large', { status: 413, headers: { 'Content-Type': 'text/plain', ...SEC_HEADERS } });
      }
      // Stream the body so a missing or lying Content-Length can't slip a huge
      // payload through and then get fully buffered by request.text(). Cancel
      // the stream the moment we exceed MAX_BODY. fatal:true on the decoder
      // throws on non-UTF-8 input rather than silently substituting U+FFFD,
      // since the WS relay frame embeds body as a JSON string and any binary
      // POST would arrive at the ESP corrupted.
      if (request.body) {
        const reader = request.body.getReader();
        const decoder = new TextDecoder('utf-8', { fatal: true });
        let received = 0;
        let parts = '';
        let tooLarge = false;
        let decodeFailed = false;
        try {
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            received += value.byteLength;
            if (received > MAX_BODY) {
              tooLarge = true;
              try { await reader.cancel(); } catch (e) {}
              break;
            }
            parts += decoder.decode(value, { stream: true });
          }
          if (!tooLarge) parts += decoder.decode();
        } catch (e) {
          decodeFailed = true;
          try { await reader.cancel(); } catch (err) {}
        }
        if (tooLarge) {
          return new Response('Payload too large', { status: 413, headers: { 'Content-Type': 'text/plain', ...SEC_HEADERS } });
        }
        if (decodeFailed) {
          return new Response('Binary payloads not supported via Worker relay', { status: 415, headers: { 'Content-Type': 'text/plain', ...SEC_HEADERS } });
        }
        body = parts;
      }
    }

    try {
      this.espSocket.send(JSON.stringify({
        id,
        method: request.method,
        path: url.pathname + url.search,
        headers,
        body
      }));
    } catch (e) {
      this.espSocket = null;
      return offlineResponse();
    }

    // Tag /stats relay responses so we can enrich + cache them on the way
    // back. Without this, a cold-worker /stats falls through to the raw ESP
    // body (no `outdoor` weather block), and the homepage flickers the
    // outdoor section on/off until lastStats warms back up. Catching the
    // first relayed /stats here populates lastStats so all subsequent polls
    // are consistent.
    const isStatsPath = (url.pathname === '/stats' && request.method === 'GET');

    return new Promise((resolve) => {
      const timeout = setTimeout(() => {
        this.pendingRequests.delete(id);
        this.activeResponses.delete(id);
        if (this.currentStreamId === id) this.currentStreamId = null;
        resolve(timeoutResponse());
      }, 30000);

      this.pendingRequests.set(id, {
        resolve: async (resp) => {
          clearTimeout(timeout);
          if (isStatsPath && resp && resp.status === 200) {
            try {
              const cloned = resp.clone();
              const raw = await cloned.json();
              const enriched = this.enrichStats(raw);
              this.lastStats = JSON.stringify(enriched);
              this.lastStatsAt = Date.now();
              // Inject live presence count for this response (kept out of
              // lastStats itself so subsequent serves get the current count).
              const responseBody = JSON.stringify({ ...enriched, clients: this.sseClients.size });
              resolve(new Response(responseBody, {
                status: 200,
                headers: {
                  'Content-Type': 'application/json',
                  'Cache-Control': 'public, max-age=5, stale-while-revalidate=30',
                  ...SEC_HEADERS
                }
              }));
              return;
            } catch (e) {
              // Fall through to raw response on parse error
            }
          }
          resolve(resp);
        }
      });
    });
  }
}

// Prefix matches: covers path families like /admin, /admin/files, etc.
const NO_CACHE_PREFIX = ['/logs', '/admin', '/_ws', '/_stream',
  '/guestbook/entries', '/guestbook/submit', '/guestbook/pending', '/guestbook/moderate',
  '/guestbook/replies', '/guestbook/locate'];
// Exact matches: a prefix rule would over-match sibling paths.
const NO_CACHE_EXACT = new Set(['/console.json', '/adsb.json']);

// Static page shells that the chip serves with a short Cache-Control. Bumping
// these to longer CF edge TTL collapses 90%+ of relay-bound traffic, which is
// what was wedging async_tcp on the chip (handleWsRelay self-deadlock under
// load). Browsers still see the chip's original max-age for freshness; CF
// edge holds longer via s-maxage. Trade: stale content at the edge for up to
// SHELL_EDGE_TTL after a chip-side change; these shells only change on
// firmware/data deploys, so an hour of edge staleness is an acceptable
// trade for the 90%+ reduction in relay-bound traffic.
const STATIC_SHELL_PATHS = new Set([
  '/about', '/history', '/console', '/guestbook', '/adsb',
  '/404.html', '/offline.html', '/timeout.html',
]);
const SHELL_EDGE_TTL = 3600;  // 1 hour CF edge cache for shells

function shouldCache(pathname) {
  if (pathname === '/stats') return false;
  if (NO_CACHE_EXACT.has(pathname)) return false;
  return !NO_CACHE_PREFIX.some(p => pathname.startsWith(p));
}

function isStaticShell(pathname) {
  return STATIC_SHELL_PATHS.has(pathname);
}

// CLI clients on "/" bypass the edge cache so browsers don't get the ASCII
// card and CLIs don't get the cached HTML. The DO sends Cache-Control:
// no-store on the card response so the inverse poison can't happen either.
function isCliRoot(request, url) {
  if (request.method !== 'GET') return false;
  if (url.pathname !== '/' && url.pathname !== '') return false;
  const ua = (request.headers.get('User-Agent') || '').toLowerCase();
  return /\b(curl|wget|httpie|libwww-perl|powershell)\b/.test(ua);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    const cliRoot = isCliRoot(request, url);
    const cacheable = !cliRoot && request.method === 'GET' && shouldCache(url.pathname);

    if (cacheable) {
      const cached = await caches.default.match(request);
      if (cached) return cached;
    }

    const id = env.ESP_RELAY.idFromName('main');
    const relay = env.ESP_RELAY.get(id);
    const response = await relay.fetch(request);

    // 101 WebSocket upgrades must be returned as-is (can't reconstruct or add headers)
    if (response.status === 101) return response;

    if (cacheable && (response.status === 200 || response.status === 404)) {
      const cc = response.headers.get('Cache-Control');
      if (cc && cc.includes('max-age')) {
        // encodeBody: 'manual' when Content-Encoding is set (gzip) so the
        // runtime doesn't auto-decompress on arrayBuffer(). The cached body
        // stays in its original gzip form and the Content-Encoding header
        // on the cached Response correctly describes those bytes.
        const ce = response.headers.get('Content-Encoding');
        const body = await response.arrayBuffer();
        const headers = applySecHeaders(new Headers(response.headers));
        // Override Cache-Control for static page shells: keep the chip's
        // short max-age for browsers (so updates feel responsive) but bump
        // s-maxage so CF edge holds the response much longer. Cuts chip
        // relay frequency ~12x on these paths, which is the primary load
        // pattern that was wedging async_tcp via handleWsRelay deadlocks.
        const finalCc = isStaticShell(url.pathname)
          ? `${cc}, s-maxage=${SHELL_EDGE_TTL}`
          : cc;
        headers.set('Cache-Control', finalCc);

        const cacheInit = ce
          ? { status: response.status, headers, encodeBody: 'manual' }
          : { status: response.status, headers };
        const cacheResp = new Response(body, cacheInit);
        await caches.default.put(request, cacheResp.clone());
        return cacheResp;
      }
    }

    const passHeaders = applySecHeaders(new Headers(response.headers));
    const passCe = response.headers.get('Content-Encoding');
    const passInit = passCe
      ? { status: response.status, headers: passHeaders, encodeBody: 'manual' }
      : { status: response.status, headers: passHeaders };
    return new Response(response.body, passInit);
  }
};
