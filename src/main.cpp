// Astrochronist Calendar for the ESP32-2432S028R "Cheap Yellow Display".
// The website's four tabs (Now, Calendar, Science, About) on a 320x240 touch screen,
// with flat colours instead of photos and blur. Time comes from Wi-Fi NTP or a USB sync.
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <Preferences.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "astro.h"
#include "systems.h"
#include "logo.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
// No secrets.h (e.g. a release build): no Wi-Fi, set the clock over USB with tools/cyd.py sync.
#define NO_WIFI 1
static const char* const WIFI_NETS[][2] = { { "", "" } };
#endif

using namespace astro;

/* ======================= hardware ======================= */
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 p_ili;
  lgfx::Panel_ST7789 p_st;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
 public:
  explicit LGFX(bool st7789, bool invert) {
    auto b = bus.config();
    b.spi_host = HSPI_HOST; b.spi_mode = 0; b.freq_write = 27000000; b.freq_read = 16000000;
    b.spi_3wire = false; b.use_lock = true; b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = 14; b.pin_mosi = 13; b.pin_miso = 12; b.pin_dc = 2;
    bus.config(b);
    lgfx::Panel_LCD* p = st7789 ? (lgfx::Panel_LCD*)&p_st : (lgfx::Panel_LCD*)&p_ili;
    p->setBus(&bus);
    auto c = p->config();
    c.pin_cs = 15; c.pin_rst = -1; c.pin_busy = -1;
    c.memory_width = 240; c.memory_height = 320; c.panel_width = 240; c.panel_height = 320;
    c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0; c.readable = true;
    c.invert = invert; c.rgb_order = false; c.dlen_16bit = false; c.bus_shared = false;
    p->config(c);
    auto l = light.config();
    l.pin_bl = 21; l.invert = false; l.freq = 44100; l.pwm_channel = 7;
    light.config(l);
    p->setLight(&light);
    setPanel(p);
  }
};

static LGFX* lcd;
static LGFX_Sprite spr;
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen ts(33, 36);
static Preferences prefs;

static const int W = 320, H = 240, BH = 80, HEAD = 26;   // screen drawn in three 320x80 bands
static int OY = 0;                                       // top of the band being drawn

/* ======================= colours (from css/style.css) ======================= */
static inline uint16_t C(uint32_t rgb) { return lgfx::color565(rgb >> 16, (rgb >> 8) & 255, rgb & 255); }
static uint16_t cBg, cSurface, cLine, cFg, cMuted, cWarn;
struct Accent { uint32_t accent, soft, ev; };
static const Accent ACC[NSYS] = {
  { 0x2C55C9, 0xDCE5FA, 0xC9DCE6 },   // Gregorian
  { 0xB4472A, 0xF3E2D6, 0xC9DCE6 },   // Mars
  { 0x5B5F7A, 0xE3E4EE, 0xF0E6C4 },   // Lunar
  { 0x7240B0, 0xECE2F8, 0xF0E6C4 },   // BJD Sgr A*
  { 0x9A6A00, 0xF6EAD0, 0xDCE5FA },   // BJD α Cen
};

/* ======================= text ======================= */
struct Font { const lgfx::IFont* f; int8_t asc; int8_t lh; bool bold; };
static const Font F_TINY = { &fonts::DejaVu9, 7, 11, false };
static const Font F_SMALL = { &fonts::DejaVu12, 9, 14, false };
static const Font F_BODY = { &fonts::FreeSans9pt7b, 13, 17, false };
static const Font F_BOLD = { &fonts::FreeSansBold9pt7b, 13, 17, true };
static const Font F_H2 = { &fonts::FreeSansBold12pt7b, 17, 23, true };
static const Font F_H1 = { &fonts::FreeSansBold18pt7b, 25, 32, true };
static const Font F_CLOCK = { &fonts::FreeSansBold24pt7b, 34, 44, true };

static uint32_t nextCp(const char*& s) {
  uint8_t c = *s++;
  if (c < 0x80) return c;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
  uint32_t cp = c & (0x3F >> n);
  while (n-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
  return cp;
}

// Draws (or measures) a UTF-8 string with its top-left at (x, yTop). ASCII goes through the
// font; the few symbols the site uses (° · – − — ∇ α ±) are drawn as shapes. Returns width.
static int textRun(const char* s, int x, int yTop, const Font& F, uint16_t col, bool draw, float scale = 1) {
  spr.setFont(F.f); spr.setTextSize(scale); spr.setTextColor(col); spr.setTextDatum(lgfx::baseline_left);
  int asc = (int)(F.asc * scale), base = yTop + asc - OY, x0 = x;
  int th = (F.bold && asc > 10) ? 2 : 1;
  char run[96]; int rn = 0;
  auto flush = [&]() {
    if (!rn) return;
    run[rn] = 0;
    if (draw) spr.drawString(run, x, base);
    x += spr.textWidth(run); rn = 0;
  };
  while (*s) {
    uint32_t cp = nextCp(s);
    if (cp < 0x80) { if (rn < 95) run[rn++] = (char)cp; else { flush(); run[rn++] = (char)cp; } continue; }
    flush();
    int adv;
    switch (cp) {
      case 0xB0: { int r = asc / 6 < 1 ? 1 : asc / 6; adv = 2 * r + 3; if (draw) { spr.drawCircle(x + r + 1, base - asc + r, r, col); if (th > 1) spr.drawCircle(x + r + 1, base - asc + r, r - 1, col); } break; }
      case 0xB7: { adv = asc / 2 + 2; int r = asc > 14 ? 2 : 1; if (draw) spr.fillCircle(x + adv / 2, base - asc * 2 / 5, r, col); break; }
      case 0x2013: case 0x2212: case 0x2014: {
        adv = cp == 0x2014 ? asc : cp == 0x2212 ? asc * 7 / 10 : asc * 6 / 10;
        if (adv < 4) adv = 4;
        if (draw) spr.fillRect(x + 1, base - asc * 2 / 5, adv - 2, th, col);
        break;
      }
      case 0x2207: { int w = asc * 9 / 10; adv = w + 3; if (draw) { spr.drawTriangle(x + 1, base - asc, x + w, base - asc, x + (w + 1) / 2, base, col); if (th > 1) spr.drawTriangle(x + 2, base - asc + 1, x + w - 1, base - asc + 1, x + (w + 1) / 2, base - 2, col); } break; }
      case 0x3B1: {
        int xh = asc * 7 / 10, r = xh / 2; adv = xh + 4;
        if (draw) {
          spr.drawEllipse(x + r + 1, base - r - 1, r, r, col);
          if (th > 1) spr.drawEllipse(x + r + 1, base - r - 1, r - 1, r - 1, col);
          spr.drawLine(x + 2 * r, base - xh, x + 2 * r + 2, base, col);
        }
        break;
      }
      case 0xB1: { adv = asc * 7 / 10 + 2; if (draw) { int cx = x + adv / 2, cy = base - asc / 2 - 1, k = adv / 2 - 1; spr.drawFastHLine(cx - k, cy, 2 * k + 1, col); spr.drawFastVLine(cx, cy - k, 2 * k + 1, col); spr.drawFastHLine(cx - k, base, 2 * k + 1, col); } break; }
      case 0x2039: run[rn++] = '<'; continue;
      case 0x203A: run[rn++] = '>'; continue;
      case 0xD7: run[rn++] = 'x'; continue;
      default: run[rn++] = '?'; continue;
    }
    x += adv;
  }
  flush();
  spr.setTextSize(1);
  return x - x0;
}
static int tw(const char* s, const Font& F, float sc = 1) { return textRun(s, 0, 0, F, 0, false, sc); }
static void tx(int x, int y, const char* s, const Font& F, uint16_t c) { if (y + F.lh > OY && y < OY + BH) textRun(s, x, y, F, c, true); }
static void txC(int cx, int y, const char* s, const Font& F, uint16_t c, float sc = 1) {
  if (y + F.lh * sc > OY && y < OY + BH) textRun(s, cx - tw(s, F, sc) / 2, y, F, c, true, sc);
}
static void txR(int rx, int y, const char* s, const Font& F, uint16_t c) { if (y + F.lh > OY && y < OY + BH) textRun(s, rx - tw(s, F), y, F, c, true); }

// Word-wraps text into width w. Returns the height used.
static int wrap(int x, int y, int w, const char* s, const Font& F, uint16_t col, bool draw = true, int lh = 0) {
  if (!lh) lh = F.lh;
  char line[200] = "", trial[220];
  int y0 = y;
  const char* p = s;
  while (*p) {
    const char* e = p; while (*e && *e != ' ') e++;
    char word[120]; int wl = e - p < 119 ? e - p : 119; memcpy(word, p, wl); word[wl] = 0;
    snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
    if (line[0] && tw(trial, F) > w) {
      if (draw) tx(x, y, line, F, col);
      y += lh; snprintf(line, sizeof line, "%s", word);
    } else snprintf(line, sizeof line, "%s", trial);
    p = *e ? e + 1 : e;
  }
  if (line[0]) { if (draw) tx(x, y, line, F, col); y += lh; }
  return y - y0;
}
static void upper(const char* s, char* o, size_t n) { size_t i = 0; for (; s[i] && i < n - 1; i++) o[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i]; o[i] = 0; }

/* ======================= shapes ======================= */
static bool dry = false;   // measuring pass: lay out but draw nothing
static inline bool vis(int y, int h) { return !dry && y + h > OY && y < OY + BH; }
static inline bool onScreen(int y, int h) { return y + h > HEAD && y < H; }
static void fill(int x, int y, int w, int h, uint16_t c) { if (vis(y, h)) spr.fillRect(x, y - OY, w, h, c); }
static void rfill(int x, int y, int w, int h, int r, uint16_t c) { if (vis(y, h)) spr.fillRoundRect(x, y - OY, w, h, r, c); }
static void rframe(int x, int y, int w, int h, int r, uint16_t c) { if (vis(y, h)) spr.drawRoundRect(x, y - OY, w, h, r, c); }

/* ======================= hit targets ======================= */
enum Act : uint8_t { A_TAB, A_NOWSYS, A_CALSYS, A_PREV, A_NEXT, A_TODAY, A_CELL, A_BAND, A_SCIBASE, A_MY, A_PSEC, A_PCAL };
struct Hit { int16_t x, y, w, h; uint8_t act; int16_t arg; };
static Hit hits[400];
static int nHits = 0;
static bool recHits = false;
static void hit(int x, int y, int w, int h, Act a, int arg = 0) {
  if (dry || !recHits || nHits >= 400 || !(y + h > 0 && y < H)) return;
  hits[nHits++] = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, a, (int16_t)arg };
}

/* ======================= state ======================= */
enum Tab { T_NOW, T_CAL, T_SCI, T_ABOUT };
static Tab tab = T_NOW;
static int scrollY[4], contentH[4];
static SysId nowSys = GREG, calSys = MARS;
static bool hasGeo = false; static double geoLat = 0, geoLon = 0;
static volatile bool timeSet = false;
static char clockSource[64] = "not set yet";
static char wifiNote[64] = "Wi-Fi: starting";

double astro::nowMs() { struct timeval tv; gettimeofday(&tv, nullptr); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }

/* ---- calendar ---- */
static YearLayout* L;
static YearCells* YC;
static SysId lSys = NSYS; static int64_t lYear = INT64_MIN;
static int64_t calYear = INT64_MIN;
static double selMs = NAN, atMs = NAN;
static int detailTop = 0, selCellY = -1, cardTop[12];
static int pendingScroll = 0;   // 1 = bring detail into view, 2 = centre the selected cell, 3 = to card bandMonth
static int bandMonth = 0;

static void ensureLayout() {
  if (calYear == INT64_MIN) calYear = yearOf(calSys, isnan(selMs) ? nowMs() : selMs);
  calYear = clampYear(calSys, calYear);
  if (lSys == calSys && lYear == calYear) return;
  layout(calSys, calYear, *L);
  yearCells(calSys, calYear, *L, *YC);
  lSys = calSys; lYear = calYear;
}

/* ---- science planner (sample target from js/ui/science.js) ---- */
struct Planner { const char* name; double ra, dec, t0, per, dur; bool sec, cal; };
static Planner PL = { "Sample target", 220.0, -45.0, 2461300.43210, 2.718281, 3.2, true, false };
static Base plBase() { return { "pl", PL.ra, PL.dec, PL.name, PL.name, "" }; }
struct Ev { int64_t n; double ph, t, ms; };
static int eventsBjd(double b0, double b1, Ev* out, int cap) {
  int k = 0;
  if (!(PL.per > 0)) return 0;
  Base B = plBase();
  for (int pi = 0; pi < (PL.sec ? 2 : 1); pi++) {
    double ph = pi * 0.5, t0 = PL.t0 + ph * PL.per;
    for (int64_t n = (int64_t)ceil((b0 - t0) / PL.per), c = 0; c < cap && k < cap; n++, c++) {
      double t = t0 + n * PL.per;
      if (t >= b1) break;
      out[k++] = { n, ph, t, msFromBjd(t, B) };
    }
  }
  for (int i = 1; i < k; i++) for (int j = i; j > 0 && out[j].t < out[j - 1].t; j--) { Ev e = out[j]; out[j] = out[j - 1]; out[j - 1] = e; }
  return k;
}
static int calendarEvents(double a, double b, Ev* out, int cap) {
  if (!PL.cal) return 0;
  Base B = plBase();
  return eventsBjd(bjdFromMs(a, B), bjdFromMs(b, B), out, cap);
}
static SysId sciBase = SGRA;
static int exactMY = 39;

/* ======================= chrome ======================= */
static void pill(int x, int y, int w, int h, const char* s, bool on, uint16_t onBg, uint16_t onFg, uint16_t offBg, uint16_t offFg, uint16_t border, const Font& F) {
  rfill(x, y, w, h, h / 2, on ? onBg : offBg);
  rframe(x, y, w, h, h / 2, on ? onBg : border);
  txC(x + w / 2, y + (h - F.asc) / 2 - 1, s, F, on ? onFg : offFg);
}

static void drawHeader() {
  // Always cream, like the site: the logo's black clock ticks need a light ground.
  fill(0, 0, W, HEAD, C(0xF7F1E3));
  fill(0, HEAD - 1, W, 1, C(0xE2D4AE));
  if (vis(2, LOGO_H)) spr.pushImage(4, 2 - OY, LOGO_W, LOGO_H, (const lgfx::rgb565_t*)LOGO);
  static const char* names[4] = { "Now", "Calendar", "Science", "About" };
  int widths[4], total = 0;
  for (int i = 0; i < 4; i++) { widths[i] = tw(names[i], F_SMALL) + 14; total += widths[i] + 2; }
  int x = W - total - 2;
  if (x > 32 + tw("Astrochronist", F_BOLD)) tx(31, 5, "Astrochronist", F_BOLD, C(0x241D10));
  for (int i = 0; i < 4; i++) {
    bool on = tab == i;
    if (on) rfill(x, 4, widths[i], 19, 6, C(0xEBDDB4));
    tx(x + 7, 8, names[i], F_SMALL, on ? C(0x241D10) : C(0x6E6248));
    hit(x, 0, widths[i], HEAD + 8, A_TAB, i);   // taller than the bar: fingers land low on this strip
    x += widths[i] + 2;
  }
}

/* ======================= NOW ======================= */
static void drawNow() {
  const uint16_t ink = C(0xF4F6FA), ink78 = C(0xC5C8CD), ink65 = C(0xA3A7AE), chip = C(0x141922), chipLine = C(0x343941);
  fill(0, HEAD, W, H - HEAD, C(0x0A0E16));
  double now = nowMs();
  NowInfo p; nowInfo(nowSys, now, p, hasGeo, geoLon);
  // system menu
  int ws[NSYS], tot = 0;
  for (int i = 0; i < NSYS; i++) { ws[i] = tw(SYS[i].pill, F_TINY) + 14; tot += ws[i] + 3; }
  int x = (W - tot + 3) / 2, y = HEAD + 10;
  for (int i = 0; i < NSYS; i++) {
    pill(x, y, ws[i], 20, SYS[i].pill, nowSys == i, ink, C(0x0B0F16), C(0x1A1E26), ink, C(0x4A4E55), F_TINY);
    hit(x, y, ws[i], 26, A_NOWSYS, i);
    x += ws[i] + 3;
  }
  // big clock, digits on fixed cells so it doesn't jitter (tabular numbers)
  float sc = 1.0f;
  int cw = tw("0", F_CLOCK, sc), colw = tw(":", F_CLOCK, sc) + 6;
  int cy = HEAD + 36;
  {
    const char* t = p.time; int width = 0;
    for (const char* q = t; *q;) { const char* q0 = q; uint32_t cp = nextCp(q); char one[5] = {0}; memcpy(one, q0, q - q0);
      width += (cp >= '0' && cp <= '9') ? cw : cp == ':' ? colw : tw(one, F_CLOCK, sc); }
    int cx = (W - width) / 2;
    for (const char* q = t; *q;) {
      const char* q0 = q; uint32_t cp = nextCp(q); char one[5] = {0}; memcpy(one, q0, q - q0);
      int w = (cp >= '0' && cp <= '9') ? cw : cp == ':' ? colw : tw(one, F_CLOCK, sc);
      if (vis(cy, 44)) textRun(one, cx + (w - tw(one, F_CLOCK, sc)) / 2, cy, F_CLOCK, ink, true, sc);
      cx += w;
    }
  }
  txC(W / 2, cy + 40, p.timeNote, F_SMALL, ink78);
  // day / month / (kiloday) / year chips
  Part* parts[4]; const char* units[4]; int n = 0;
  parts[n] = &p.day; units[n++] = SYS[nowSys].uDay;
  parts[n] = &p.month; units[n++] = SYS[nowSys].uMonth;
  if (p.hasKilo) { parts[n] = &p.kilo; units[n++] = SYS[nowSys].uKilo; }
  parts[n] = &p.year; units[n++] = SYS[nowSys].uYear;
  int gap = 6, cwid = (W - 16 - gap * (n - 1)) / n, top = cy + 58, ch = 54;
  for (int i = 0; i < n; i++) {
    int x0 = 8 + i * (cwid + gap);
    rfill(x0, top, cwid, ch, 8, chip); rframe(x0, top, cwid, ch, 8, chipLine);
    char u[24]; upper(units[i], u, sizeof u);
    txC(x0 + cwid / 2, top + 5, u, F_TINY, ink65);
    const Font* vf = &F_H2;
    if (tw(parts[i]->v, F_H2) > cwid - 6) vf = &F_BOLD;
    txC(x0 + cwid / 2, top + 16, parts[i]->v, *vf, ink);
    const char* sub = parts[i]->sub; char sb[48];
    snprintf(sb, sizeof sb, "%s", sub);
    while (tw(sb, F_TINY) > cwid - 4 && strlen(sb) > 3) { sb[strlen(sb) - 4] = 0; strcat(sb, "..."); }
    txC(x0 + cwid / 2, top + 40, sb, F_TINY, ink78);
  }
  // extras
  y = top + ch + 6;
  for (int i = 0; i < p.nEx; i++) {
    int a = tw(p.exK[i], F_SMALL), b = tw(p.exV[i], F_SMALL);
    int x0 = (W - a - 7 - b) / 2;
    tx(x0, y, p.exK[i], F_SMALL, ink65);
    tx(x0 + a + 7, y, p.exV[i], F_SMALL, ink);
    y += 13;
  }
  // "here": local time / solar time
  char buf[96];
  if (!timeSet) {
#ifdef NO_WIFI
    txC(W / 2, H - 13, "Clock not set: run tools/cyd.py sync over USB", F_TINY, C(0xE57A5A));
#else
    txC(W / 2, H - 13, "Setting the clock: waiting for Wi-Fi time...", F_TINY, C(0xE57A5A));
#endif
  } else if (nowSys != GREG) {
    char lt[48]; localDayHMS(now, lt, sizeof lt);
    snprintf(buf, sizeof buf, "Your local time: %s", lt);
    txC(W / 2, H - 13, buf, F_TINY, C(0xD9DBDF));
  }
  if (hasGeo && timeSet) {
    SunTimes s = sunTimes(now, geoLat, geoLon);
    char ap[12], mn[12], r[8], st[8];
    hms(meanSolarFrac(now, geoLon) + s.eotMin / 1440, ap); ap[5] = 0;
    hms(meanSolarFrac(now, geoLon), mn); mn[5] = 0;
    if (s.polar) snprintf(buf, sizeof buf, "solar time %s (mean %s) \xC2\xB7 %s", ap, mn, s.polar);
    else { localHMonly(s.rise, r, 8); localHMonly(s.set, st, 8); snprintf(buf, sizeof buf, "solar time %s (mean %s) \xC2\xB7 sunrise %s \xC2\xB7 sunset %s", ap, mn, r, st); }
    txC(W / 2, H - 25, buf, F_TINY, C(0xD9DBDF));
  }
}

/* ======================= CALENDAR ======================= */
static void dashedRect(int x, int y, int w, int h, uint16_t c) {
  for (int i = 0; i < w; i += 4) { fill(x + i, y, 2, 2, c); fill(x + i, y + h - 2, 2, 2, c); }
  for (int i = 0; i < h; i += 4) { fill(x, y + i, 2, 2, c); fill(x + w - 2, y + i, 2, 2, c); }
}

static int cellIndexAt(double ms) {
  if (isnan(ms) || YC->n == 0 || ms < YC->start[0] || ms >= YC->start[YC->n]) return -1;
  int lo = 0, hi = YC->n - 1;
  while (lo < hi) { int m = (lo + hi + 1) / 2; if (YC->start[m] <= ms) lo = m; else hi = m - 1; }
  return lo;
}
static int monthOfCell(int i) { int m = 0; while (m + 1 < L->nMonths && YC->monthFirst[m + 1] <= i) m++; return m; }

static void eventItem(int x, int& y, int w, const Ev& e, uint16_t surf) {
  char a[96], b[64], c[64];
  int h = 4 * F_TINY.lh + 6 + (PL.dur > 0 ? F_TINY.lh : 0);
  fill(x, y, w, h, surf);
  if (e.ph) dashedRect(x, y, 3, h, cWarn); else fill(x, y, 3, h, cWarn);
  int yy = y + 3;
  snprintf(a, sizeof a, "%s \xC2\xB7 %s \xC2\xB7 E = %lld", PL.name, e.ph ? "secondary" : "primary", (long long)e.n);
  tx(x + 8, yy, a, F_TINY, cFg); yy += F_TINY.lh;
  snprintf(a, sizeof a, "BJD %.5f", e.t); tx(x + 8, yy, a, F_TINY, cFg); yy += F_TINY.lh;
  isoUTC(e.ms, b); snprintf(a, sizeof a, "%s UTC", b); tx(x + 8, yy, a, F_TINY, cFg); yy += F_TINY.lh;
  localFull(e.ms, c, sizeof c); tx(x + 8, yy, c, F_TINY, cFg); yy += F_TINY.lh;
  if (PL.dur > 0) {
    double half = PL.dur * 1800e3; char s1[24], s2[24]; isoUTC(e.ms - half, s1); isoUTC(e.ms + half, s2);
    s1[16] = 0; s2[16] = 0; snprintf(a, sizeof a, "%s\xE2\x80\x93%s UTC", s1 + 11, s2 + 11); tx(x + 8, yy, a, F_TINY, cFg);
  }
  y += h + 6;
}

static int drawDetail(int X, int CW, int y, int si, uint16_t acc);
static void drawCards(int X, int CW, int y, int si, double now, uint16_t acc, uint16_t ev);

static void drawCalendar() {
  ensureLayout();
  const Accent& A = ACC[calSys];
  uint16_t acc = C(A.accent), soft = C(A.soft), ev = C(A.ev);
  fill(0, HEAD, W, H - HEAD, cBg);
  const int X = 10, CW = W - 20;
  int y = HEAD + 10 - scrollY[T_CAL];
  double now = nowMs();
  char a[128], b[128];

  tx(X, y, L->title, F_H1, cFg); y += F_H1.lh;
  y += wrap(X, y, CW, L->subtitle, F_SMALL, cMuted) + 8;
  // system picker
  int x = X;
  for (int i = 0; i < NSYS; i++) {
    int w = tw(SYS[i].name, F_SMALL) + 18;
    if (x + w > X + CW) { x = X; y += 26; }
    pill(x, y, w, 22, SYS[i].name, calSys == i, acc, cSurface, cSurface, cFg, cLine, F_SMALL);
    hit(x, y, w, 22, A_CALSYS, i);
    x += w + 4;
  }
  y += 30;
  // ‹ › Today
  bool canPrev = clampYear(calSys, calYear - 1) != calYear, canNext = clampYear(calSys, calYear + 1) != calYear;
  rfill(X, y, 34, 28, 14, cSurface); rframe(X, y, 34, 28, 14, cLine);
  txC(X + 17, y + 6, "\xE2\x80\xB9", F_H2, canPrev ? cFg : cLine); if (canPrev) hit(X, y, 34, 28, A_PREV);
  rfill(X + 40, y, 34, 28, 14, cSurface); rframe(X + 40, y, 34, 28, 14, cLine);
  txC(X + 57, y + 6, "\xE2\x80\xBA", F_H2, canNext ? cFg : cLine); if (canNext) hit(X + 40, y, 34, 28, A_NEXT);
  rfill(X + 80, y, 64, 28, 7, cSurface); rframe(X + 80, y, 64, 28, 7, cLine);
  txC(X + 112, y + 7, "Today", F_SMALL, cFg); hit(X + 80, y, 64, 28, A_TODAY);
  y += 38;
  // band of month widths to scale
  float tot = 0; for (int i = 0; i < L->nMonths; i++) tot += L->band[i].weight;
  int bw = CW - 2 * (L->nMonths - 1); float bx = X;
  for (int i = 0; i < L->nMonths; i++) {
    float w = bw * L->band[i].weight / tot;
    int x0 = (int)(bx + .5f), x1 = (int)(bx + w + .5f);
    fill(x0, y, x1 - x0, 30, L->band[i].accent ? soft : cSurface);
    txC((x0 + x1) / 2, y + (L->band[i].value[0] ? 4 : 10), L->band[i].label, F_TINY, cFg);
    if (L->band[i].value[0]) txC((x0 + x1) / 2, y + 16, L->band[i].value, F_TINY, cMuted);
    hit(x0, y, x1 - x0, 30, A_BAND, i);
    bx += w + 2;
  }
  y += 34;
  y += wrap(X, y, CW, L->bandNote, F_TINY, cMuted);
  if (L->bandAccent[0]) y += wrap(X, y, CW, L->bandAccent, F_TINY, acc);
  y += 8;

  // detail / conversion: measure, paint the box, then draw the text on it
  detailTop = y + scrollY[T_CAL];
  int si = cellIndexAt(selMs);
  {
    bool wasDry = dry; dry = true;
    int y1 = drawDetail(X, CW, y, si, acc);
    dry = wasDry;
    rfill(X, y, CW, y1 - y, 6, cSurface); fill(X, y, 4, y1 - y, acc);
    y = drawDetail(X, CW, y, si, acc);
  }
  y += 10;
  drawCards(X, CW, y, si, now, acc, ev);
}

static int drawDetail(int X, int CW, int y, int si, uint16_t acc) {
  char a[128], b[128];
  {
    y += 8;
    if (si < 0) {
      char u[16]; snprintf(u, sizeof u, "%s", SYS[calSys].uDay); for (char* q = u; *q; q++) *q = tolower(*q);
      snprintf(a, sizeof a, "Tap a %s to convert it into every other calendar.", u);
      y += wrap(X + 12, y, CW - 18, a, F_SMALL, cMuted);
    } else {
      double st = YC->start[si], en = YC->start[si + 1] - 1;
      dayLabel(calSys, st, a, sizeof a);
      y += wrap(X + 12, y, CW - 18, a, F_BOLD, cFg) + 2;
      static const SysId order[6] = { GREG, GREG, MARS, LUNAR, SGRA, ACEN };
      static const char* names[6] = { "Your local time", "UTC", nullptr, nullptr, nullptr, nullptr };
      for (int r = 0; r < 6; r++) {
        SysId s = order[r];
        const char* nm = names[r] ? names[r] : SYS[s].name;
        bool cur = r >= 2 ? s == calSys : (r == 0 && calSys == GREG);
        fill(X + 12, y, CW - 18, 1, cLine); y += 3;
        tx(X + 12, y, nm, F_TINY, cur ? acc : cMuted); y += F_TINY.lh;
        double ts[3] = { atMs, st, en }; const char* lb[3] = { "AT", "STARTS", "ENDS" };
        for (int k = isnan(atMs) ? 1 : 0; k < 3; k++) {
          if (r == 1) { isoUTC(ts[k], b); } else label(s, ts[k], b, sizeof b);
          tx(X + 12, y + 1, lb[k], F_TINY, cMuted);
          tx(X + 56, y, b, F_SMALL, cFg); y += F_SMALL.lh - 1;
        }
        y += 2;
      }
      Ev evs[8]; int ne = calendarEvents(st, en + 1, evs, 8);
      for (int i = 0; i < ne; i++) { y += 4; eventItem(X + 12, y, CW - 18, evs[i], cBg); }
    }
    y += 8;
  }
  return y;
}

static void drawCards(int X, int CW, int y, int si, double now, uint16_t acc, uint16_t ev) {
  char a[128];
  const bool wide = L->months[0].cols > 7;
  for (int m = 0; m < L->nMonths; m++) {
    const MonthCard& k = L->months[m];
    int first = YC->monthFirst[m];
    int cols = k.cols, gap = 2, iw = CW - 16;
    int cw = (iw - gap * (cols - 1)) / cols, chh = wide ? 24 : 20;
    int slots = k.lead + k.n, rows = (slots + cols - 1) / cols;
    int metaH = k.meta[0] ? wrap(0, 0, iw, k.meta, F_TINY, 0, false) : 0;
    int nNotes = 0;
    for (int i = 0; i < YC->nNotes; i++) if (YC->noteIdx[i] >= first && YC->noteIdx[i] < first + k.n) nNotes++;
    int ti = cellIndexAt(now); bool todayHere = ti >= first && ti < first + k.n; if (todayHere) nNotes++;
    int h = 8 + 24 + metaH + 6 + (k.headers ? 14 : 0) + rows * (chh + gap) + (nNotes ? 4 + nNotes * F_TINY.lh : 0) + 8;
    cardTop[m] = y + scrollY[T_CAL];
    if (si >= first && si < first + k.n) {
      int slot = k.lead + si - first;
      selCellY = y + 8 + 24 + metaH + 6 + (k.headers ? 14 : 0) + (slot / cols) * (chh + gap) + scrollY[T_CAL];
    }
    if (onScreen(y, h)) {
      rfill(X, y, CW, h, 8, cSurface); rframe(X, y, CW, h, 8, k.accent ? acc : cLine);
      int yy = y + 8;
      tx(X + 8, yy, k.title, F_H2, cFg);
      int tW = tw(k.title, F_H2);
      if (k.sub[0]) tx(X + 8 + tW + 8, yy + 6, k.sub, F_SMALL, cMuted);
      txR(X + CW - 8, yy + 6, k.right, F_SMALL, acc);
      yy += 24;
      if (metaH) yy += wrap(X + 8, yy, iw, k.meta, F_TINY, cMuted);
      yy += 6;
      if (k.headers) {
        for (int c = 0; c < cols; c++) {
          char hb[4]; if (k.headers == 1) snprintf(hb, 4, "%s", MARS_WEEK[c]); else snprintf(hb, 4, "+%d", c);
          txC(X + 8 + c * (cw + gap) + cw / 2, yy, hb, F_TINY, cMuted);
        }
        yy += 14;
      }
      for (int j = 0; j < k.n; j++) {
        int slot = k.lead + j, cx = X + 8 + (slot % cols) * (cw + gap), cy = yy + (slot / cols) * (chh + gap);
        if (!onScreen(cy, chh)) continue;
        int gi = first + j;
        hit(cx, cy, cw, chh, A_CELL, gi);
        if (!vis(cy, chh)) continue;
        bool isToday = now >= YC->start[gi] && now < YC->start[gi + 1];
        bool isSel = gi == si;
        uint16_t bgc = isToday ? acc : YC->cls[gi] == 1 ? ev : cSurface;
        if (bgc != cSurface) rfill(cx, cy, cw, chh, 5, bgc);
        if (YC->cls[gi] == 2 && !isToday) dashedRect(cx, cy, cw, chh, cWarn);
        if (isSel) { rframe(cx, cy, cw, chh, 5, cFg); rframe(cx + 1, cy + 1, cw - 2, chh - 2, 4, cFg); }
        char lb[6], sb[8]; cellLabel(calSys, *YC, m, gi, lb, sb);
        uint16_t fc = isToday ? cSurface : cFg;
        if (sb[0] && wide) {
          txC(cx + cw / 2, cy + 2, lb, F_SMALL, fc);
          txC(cx + cw / 2, cy + 14, sb, F_TINY, isToday ? cSurface : cMuted);
        } else txC(cx + cw / 2, cy + (chh - F_SMALL.asc) / 2 - 1, lb, F_SMALL, fc);
        if (PL.cal) { Ev e[1]; if (calendarEvents(YC->start[gi], YC->start[gi + 1], e, 1)) spr.fillCircle(cx + cw - 5, cy + 4 - OY, 2, cWarn); }
      }
      yy += rows * (chh + gap);
      if (nNotes) {
        yy += 4;
        for (int i = 0; i < YC->nNotes; i++) {
          int gi = YC->noteIdx[i]; if (gi < first || gi >= first + k.n) continue;
          char lb[6], sb[8]; cellLabel(calSys, *YC, m, gi, lb, sb);
          snprintf(a, sizeof a, "%s %s: %s", k.title, lb, YC->note[i]);
          // keep it on one line on this narrow card
          while (tw(a, F_TINY) > iw && strlen(a) > 4) { a[strlen(a) - 4] = 0; strcat(a, "..."); }
          tx(X + 8, yy, a, F_TINY, cFg); yy += F_TINY.lh;
        }
        if (todayHere) { char lb[6], sb[8]; cellLabel(calSys, *YC, m, ti, lb, sb); snprintf(a, sizeof a, "%s: today", lb); tx(X + 8, yy, a, F_TINY, cFg); }
      }
    }
    y += h + 12;
  }
  y += 4;
  y += wrap(X, y, CW, L->foot, F_TINY, cMuted);
  contentH[T_CAL] = y + scrollY[T_CAL] - HEAD + 12;
}


/* ======================= SCIENCE ======================= */
static int sciCard(int x, int& y, int w, const char* lbl, const char* v, const char* p, uint16_t top) {
  int ph = wrap(0, 0, w - 16, p, F_TINY, 0, false);
  int h = 8 + F_TINY.lh + F_H2.lh + ph + 6;
  if (onScreen(y, h)) {
    rfill(x, y, w, h, 8, cSurface); rframe(x, y, w, h, 8, cLine); fill(x + 2, y, w - 4, 3, top);
    char u[64]; upper(lbl, u, sizeof u);
    tx(x + 8, y + 8, u, F_TINY, cMuted);
    const Font* vf = tw(v, F_H2) > w - 16 ? &F_BOLD : &F_H2;
    tx(x + 8, y + 8 + F_TINY.lh + 2, v, *vf, cFg);
    wrap(x + 8, y + 8 + F_TINY.lh + F_H2.lh, w - 16, p, F_TINY, cMuted);
  }
  y += h + 8;
  return h;
}
static void mmss(double s, char* o) { double a = fabs(s); int m = (int)floor(a / 60); snprintf(o, 32, "%s%d min %d s", s < 0 ? "\xE2\x88\x92" : "+", m, (int)lround(a - 60 * m)); }

static void drawScience() {
  fill(0, HEAD, W, H - HEAD, cBg);
  const int X = 10, CW = W - 20;
  int y = HEAD + 10 - scrollY[T_SCI];
  const uint16_t acc = C(ACC[GREG].accent);
  double now = nowMs(), j = jdFromMs(now), jt = j + 69.184 / 86400;
  double msd = msdFromMs(now), ls = lsOf(msd);
  MarsDate md; bool mdOk = marsDate((int64_t)floor(msd), md);
  Phase ph = moonPhase(now);
  double bS = bjdFromMs(now, BASE_SGRA), bA = bjdFromMs(now, BASE_ACEN);
  char a[160], b[160], c[160], d[64];

  tx(X, y, "Science", F_H1, cFg); y += F_H1.lh;
  y += wrap(X, y, CW, "Live time scales, barycentric corrections, the exact Martian months, and a transit and eclipse planner whose events can show on the calendar.", F_SMALL, cMuted) + 10;

  // strip of time scales
  {
    char v[12][48]; const char* k[12] = { "UTC", "JD (UTC)", "MJD", "JD (TT)", "BJD TDB \xC2\xB7 Sgr A*", "BJD TDB \xC2\xB7 \xCE\xB1 Cen", "Mars Sol Date", "MTC", "Mars date", "Ls", "LST", "Lunation" };
    isoUTC(now, v[0]); snprintf(v[1], 48, "%.5f", j); snprintf(v[2], 48, "%.5f", j - 2400000.5); snprintf(v[3], 48, "%.5f", jt);
    snprintf(v[4], 48, "%.5f", bS); snprintf(v[5], 48, "%.5f", bA); fmtFix(msd, 5, v[6], 48); hms(msd, v[7]);
    if (mdOk) snprintf(v[8], 48, "MY %d M%d sol %d", md.my, md.month, md.sol); else strcpy(v[8], "\xE2\x80\x94");
    snprintf(v[9], 48, "%.3f\xC2\xB0", ls); label(LUNAR, now, v[10], 48);
    fmtInt(ph.lunation, d, sizeof d); snprintf(v[11], 48, "%s \xC2\xB7 %.2f d", d, ph.age);
    // one row per scale on this narrow screen: label left, value right
    int rh = 17, h = 12 * rh + 6;
    if (onScreen(y, h)) {
      rfill(X, y, CW, h, 8, cSurface); rframe(X, y, CW, h, 8, cLine);
      for (int i = 0; i < 12; i++) {
        int cy = y + 3 + i * rh;
        if (i) fill(X + 6, cy, CW - 12, 1, cLine);
        char u[40]; upper(k[i], u, sizeof u);
        tx(X + 8, cy + 5, u, F_TINY, cMuted);
        txR(X + CW - 8, cy + 3, v[i], F_SMALL, cFg);
      }
    }
    y += h + 18;
  }

  // Living on BJD time
  const Base& base = sciBase == ACEN ? BASE_ACEN : BASE_SGRA;
  tx(X, y, "Living on BJD time", F_H2, cFg);
  {
    int x = X + tw("Living on BJD time", F_H2) + 8;
    for (int i = 0; i < 2; i++) {
      SysId s = i ? ACEN : SGRA; const char* nm = i ? BASE_ACEN.name : BASE_SGRA.name;
      int w = tw(nm, F_TINY) + 14;
      if (x + w > X + CW) break;
      pill(x, y + 3, w, 18, nm, sciBase == s, acc, cSurface, cSurface, cFg, cLine, F_TINY);
      hit(x, y, w, 24, A_SCIBASE, s); x += w + 4;
    }
  }
  y += F_H2.lh + 6;
  {
    double B = bjdFromMs(now, base); int64_t N = (int64_t)floor(B);
    double next = msFromBjd(N + 1, base);
    int64_t jpYear = (int64_t)floor(N / 365.25) + 1, jpDay = (int64_t)floor(N - (jpYear - 1) * 365.25) + 1;
    char nb[24], n1[24]; fmtInt(N, nb, 24); fmtInt(N + 1, n1, 24);
    localHM(next, c, sizeof c);
    snprintf(a, sizeof a, "Rolls over to %s at %s %s, your time.", n1, c, tzName);
    sciCard(X, y, CW, "Today is day", nb, a, acc);
    mmss(corrSec(j, base.ra, base.dec), d);
    snprintf(b, sizeof b, "Shift from JD now (%s)", base.name);
    snprintf(a, sizeof a, "Swings \xC2\xB1%.1f min over a year toward %s, so BJD midnight wobbles.", amplitude(base, false, now), base.name);
    sciCard(X, y, CW, b, d, a, acc);
    sciCard(X, y, CW, "Weekday", WEEKDAYS[imod(N + 1, 7)], "floor(JD + 1.5) mod 7, with 0 = Sunday. Weekdays survive the switch.", acc);
    int64_t K = (int64_t)floor(N / 1000.0); char kb[24], k1[24]; fmtInt(K, kb, 24); fmtInt(K + 1, k1, 24);
    utcDate(msFromBjd((K + 1) * 1000, base), c, sizeof c);
    snprintf(a, sizeof a, "Day %d of 1,000. Kiloday %s starts %s.", (int)imod(N, 1000), k1, c);
    sciCard(X, y, CW, "Kiloday", kb, a, acc);
    fmtInt(jpYear, kb, 24);
    snprintf(a, sizeof a, "Day %lld of a 365.25-day year. Counted from 4713 BC; the period ends in AD 3268.", (long long)jpDay);
    sciCard(X, y, CW, "Julian Period year", kb, a, acc);
    mmss((bS - bA) * 86400, d);
    sciCard(X, y, CW, "Sgr A* vs \xCE\xB1 Cen", d, "How far apart the two BJD baselines are right now.", acc);
    y += wrap(X, y, CW, "BJD days start near noon UTC, so a night of observing keeps one date. Everyone on Earth shares the same day number at the same moment.", F_TINY, cMuted) + 14;

    // Mars on the same clock
    tx(X, y, "Mars on the same clock", F_H2, cFg); y += F_H2.lh + 6;
    uint16_t mc = C(0xB4472A);
    double dayStart = msFromBjd(N, base);
    double f0 = fmodp(msdFromMs(dayStart), 1), rate = 1 - 1 / SOLD, into = (1 - f0) * SOLD;
    fmtFix(msd, 4, d, sizeof d);
    snprintf(a, sizeof a, "Mars's day count, like JD. One sol = %.5f Earth days.", SOLD);
    sciCard(X, y, CW, "Mars Sol Date", d, a, mc);
    hms(msd, d); sciCard(X, y, CW, "Coordinated Mars Time", d, "Mean solar time at Airy-0, Mars's prime meridian.", mc);
    snprintf(d, sizeof d, "Ls %.1f\xC2\xB0", ls);
    char sl[24]; snprintf(sl, sizeof sl, "%s", SEASONS[(int)floor(ls / 90)]); for (char* q = sl; *q; q++) *q = tolower(*q);
    if (mdOk) snprintf(a, sizeof a, "Mars Year %d, %s. Ls is Mars's orbital angle from its spring equinox.", md.my, sl);
    else snprintf(a, sizeof a, "%s. Ls is Mars's orbital angle from its spring equinox.", sl);
    sciCard(X, y, CW, "Season", d, a, mc);
    char sb[24]; fmtInt((int64_t)floor(msdFromMs(dayStart)) + 1, sb, 24);
    if (into < 1) { snprintf(d, sizeof d, "+%d h %d m", (int)floor(into * 24), (int)imod((int64_t)floor(into * 1440), 60)); snprintf(a, sizeof a, "Sol %s begins this long after BJD day %s starts. Boundaries line up again in %d days.", sb, nb, (int)ceil(f0 / rate)); }
    else { snprintf(d, sizeof d, "no sol start"); snprintf(a, sizeof a, "No sol begins during BJD day %s. Boundaries line up again in %d days.", nb, (int)ceil(f0 / rate)); }
    sciCard(X, y, CW, "Sol vs BJD day", d, a, mc);
    mmss(corrSecMars(j, base.ra, base.dec), d);
    snprintf(a, sizeof a, "Toward %s, a Mars clock sees BJD midnight swing \xC2\xB1%.1f min over 687 days (Earth: \xC2\xB1%.1f).", base.name, amplitude(base, true, now), amplitude(base, false, now));
    sciCard(X, y, CW, "Shift from JD on Mars", d, a, mc);
  }
  y += 6;
  // The Moon
  tx(X, y, "The Moon", F_H2, cFg); y += F_H2.lh + 6;
  {
    uint16_t lc = C(0x8A8FA8);
    LST p = lst(now);
    label(LUNAR, now, d, sizeof d);
    snprintf(a, sizeof a, "Year %lld, lunar day %d, cycle %d. One lunar second = %.5f SI seconds.", (long long)p.year, p.day, p.cycle, LUNAR_SECOND);
    sciCard(X, y, CW, "Lunar Standard Time", d, a, lc);
    snprintf(d, sizeof d, "%d%% lit", (int)lround(ph.illum * 100));
    snprintf(a, sizeof a, "%s, %.2f days since mean new moon. True phases can differ by up to about 14 hours.", ph.name, ph.age);
    sciCard(X, y, CW, "Mean phase", d, a, lc);
    fmtInt(ph.lunation, d, sizeof d); fmtInt(ph.brown, c, sizeof c);
    snprintf(a, sizeof a, "Meeus numbering (0 = 6 Jan 2000). Brown lunation %s.", c);
    sciCard(X, y, CW, "Lunation", d, a, lc);
    snprintf(d, sizeof d, "%.6f d", SYNODIC);
    sciCard(X, y, CW, "Synodic month", d, "New moon to new moon. One LST lunar day, by definition.", lc);
  }
  y += 6;
  // Exact Martian months
  tx(X, y, "Exact Martian months", F_H2, cFg); y += F_H2.lh + 4;
  {
    snprintf(a, sizeof a, "MARS YEAR %d", exactMY);
    tx(X, y + 6, a, F_TINY, cMuted);
    int bx = X + tw(a, F_TINY) + 10;
    rfill(bx, y, 30, 22, 11, cSurface); rframe(bx, y, 30, 22, 11, cLine); txC(bx + 15, y + 3, "\xE2\x88\x92", F_BOLD, cFg); hit(bx, y - 3, 30, 28, A_MY, -1);
    rfill(bx + 36, y, 30, 22, 11, cSurface); rframe(bx + 36, y, 30, 22, 11, cLine); txC(bx + 51, y + 2, "+", F_BOLD, cFg); hit(bx + 36, y - 3, 30, 28, A_MY, 1);
    y += 28;
    y += wrap(X, y, CW, "The calendar rounds months to whole sols. Here each month starts at the exact moment Ls crosses its 30\xC2\xB0 mark (Allison and McEwen 2000 orbit model).", F_TINY, cMuted) + 6;
    int rh = 2 * F_TINY.lh + 6, h = 13 * rh + 4;
    if (onScreen(y, h)) {
      static int cachedMY = INT32_MIN; static double bnd[13];
      if (cachedMY != exactMY) { for (int i = 0; i <= 12; i++) bnd[i] = boundary(exactMY, i); cachedMY = exactMY; }
      rfill(X, y, CW, h, 8, cSurface); rframe(X, y, CW, h, 8, cLine);
      int yy = y + 4;
      for (int i = 0; i < 12; i++) {
        if (i) fill(X + 6, yy - 3, CW - 12, 1, cLine);
        fmtFix(bnd[i], 3, b, sizeof b); fmtFix(bnd[i + 1] - bnd[i], 3, c, sizeof c);
        snprintf(a, sizeof a, "M%d   %d\xC2\xB0\xE2\x80\x93%d\xC2\xB0   MSD %s", i + 1, 30 * i, 30 * i + 30, b);
        tx(X + 8, yy, a, F_TINY, cFg);
        snprintf(a, sizeof a, "%s sols", c); txR(X + CW - 8, yy, a, F_TINY, cFg);
        utcFull(msFromMsd(bnd[i]), b, sizeof b); snprintf(a, sizeof a, "starts %s UTC", b);
        tx(X + 8, yy + F_TINY.lh, a, F_TINY, cMuted);
        yy += rh;
      }
      fill(X + 6, yy - 3, CW - 12, 1, cLine);
      fmtFix(bnd[12] - bnd[0], 3, c, sizeof c);
      snprintf(a, sizeof a, "Mars Year %d, Ls 0\xC2\xB0 to 360\xC2\xB0", exactMY); tx(X + 8, yy + 3, a, F_TINY, cFg);
      snprintf(a, sizeof a, "%s sols", c); txR(X + CW - 8, yy + 3, a, F_TINY, cFg);
    }
    y += h + 14;
  }
  // Planner
  tx(X, y, "Transit and eclipse planner", F_H2, cFg); y += F_H2.lh + 6;
  {
    int h = 8 + 4 * 24 + 6;
    const char* keys[6] = { "Target", "RA (hh:mm:ss or deg)", "Dec (\xC2\xB1" "dd:mm:ss or deg)", "T0 (BJD TDB)", "Period (days)", "Duration (hours)" };
    const char* vals[6] = { PL.name, "14:40:00", "-45:00:00", "2461300.43210", "2.718281", "3.2" };
    if (onScreen(y, h)) {
      rfill(X, y, CW, h, 8, cSurface); rframe(X, y, CW, h, 8, cLine);
      for (int i = 0; i < 6; i++) {
        int cx = X + 8 + (i % 2) * (CW / 2), cy = y + 8 + (i / 2) * 24;
        tx(cx, cy, keys[i], F_TINY, cMuted); tx(cx, cy + 10, vals[i], F_TINY, cFg);
      }
      int cy = y + 8 + 3 * 24 + 2;
      for (int i = 0; i < 2; i++) {
        bool on = i ? PL.cal : PL.sec; int cx = X + 8 + i * (CW / 2);
        rframe(cx, cy, 14, 14, 3, on ? acc : cLine); if (on) rfill(cx + 1, cy + 1, 12, 12, 2, acc);
        if (on) { spr.drawLine(cx + 3, cy + 7 - OY, cx + 6, cy + 10 - OY, cSurface); spr.drawLine(cx + 6, cy + 10 - OY, cx + 11, cy + 3 - OY, cSurface); }
        tx(cx + 20, cy + 1, i ? "Show on calendar" : "Secondary (phase 0.5)", F_SMALL, cFg);
        hit(cx - 4, cy - 4, CW / 2 - 4, 22, i ? A_PCAL : A_PSEC);
      }
    }
    y += h + 4;
    y += wrap(X, y, CW, "Sample values shown. BJD uses TT = UTC + 69.184 s and a low-precision Earth barycentric position, good to about \xC2\xB1" "2 s. Use astropy for publication-grade timing.", F_TINY, cMuted) + 8;
    tx(X, y, "Upcoming", F_BOLD, cFg); y += F_BOLD.lh + 4;
    Base B = plBase(); double nb = bjdFromMs(now, B);
    static Ev up[8]; static int nUp = 0; static double upAt = -1; static bool upSec = true;
    if (fabs(nb - upAt) > 0.01 || upSec != PL.sec) { nUp = eventsBjd(nb, nb + fmax(PL.per * 6, 1), up, 8); if (nUp > 6) nUp = 6; upAt = nb; upSec = PL.sec; }
    for (int i = 0; i < nUp; i++) {
      int hh = 5 * F_TINY.lh + 6;
      if (onScreen(y, hh)) eventItem(X, y, CW, up[i], cSurface); else y += hh + 6;
    }
  }
  contentH[T_SCI] = y + scrollY[T_SCI] - HEAD + 12;
}

/* ======================= ABOUT ======================= */
static void drawAbout() {
  fill(0, HEAD, W, H - HEAD, cBg);
  const int X = 12, CW = W - 24;
  int y = HEAD + 10 - scrollY[T_ABOUT];
  tx(X, y, "Astrochronism", F_H1, cFg); y += F_H1.lh + 6;
  y += wrap(X, y, CW, "Astrochronism is the idea of basing time on astronomical things and events instead of arbitrary Earth ones, especially those within our solar system, our stellar neighbourhood, and our galaxy.", F_BODY, cFg) + 8;
  y += wrap(X, y, CW, "Most of the calendar we live by is inherited custom. Month lengths come from Roman politics, the year count from a medieval estimate of a birth date, time zones from railway schedules, and leap seconds from committee decisions. None of it is wrong, but very little of it is about the sky.", F_SMALL, cFg) + 8;
  y += wrap(X, y, CW, "An astrochronist calendar counts from things anyone can measure anywhere: a planet crossing its equinox, the Moon returning to new, the light-travel geometry between the Sun's barycentre and the centre of the Milky Way. Its day numbers mean the same thing to every observer, and they keep working once people live somewhere other than Earth.", F_SMALL, cFg) + 14;
  tx(X, y, "The systems on this site", F_H2, cFg); y += F_H2.lh + 4;
  static const char* dts[5] = { "Gregorian, local and UTC", "Martian", "Lunar Standard Time", "BJD \xC2\xB7 Sol\xE2\x80\x93Sgr A*", "BJD \xC2\xB7 Sol\xE2\x80\x93\xCE\xB1 Cen" };
  static const char* dds[5] = {
    "The civil calendar, kept here as the reference point everything converts to and from.",
    "Mars Years numbered from the northern spring equinox of 1955 (MY 1), each split into twelve months of 30\xC2\xB0 of solar longitude, with sols counted from Airy-0 midnight. Months near aphelion run longer because Mars moves slowest there.",
    "Kenneth L. Lee's 1970 clock for the Moon, which starts at the moment Neil Armstrong stepped onto the surface. A lunar day is one synodic month, divided into 30 cycles; twelve lunar days make a lunar year.",
    "Whole days of Barycentric Julian Date measured toward Sagittarius A*, the black hole at the galactic centre. Days are grouped in hectodays (100) and kilodays (1,000), and into years of 365.25 days. Year 1 starts at BJD 0.0, second zero of the count; we are now in Year 6,739.",
    "The same count measured toward Alpha Centauri A, the nearest star system. The two BJD calendars differ by a few minutes at most, depending on where Earth is in its orbit.",
  };
  for (int i = 0; i < 5; i++) { y += 6; tx(X, y, dts[i], F_BOLD, cFg); y += F_BOLD.lh; y += wrap(X, y, CW, dds[i], F_SMALL, cMuted); }
  y += 14;
  tx(X, y, "Accuracy", F_H2, cFg); y += F_H2.lh + 4;
  y += wrap(X, y, CW, "Everything is computed on this board with compact formulas: the Mars24 / Allison and McEwen (2000) model for Mars, a mean synodic month for the Moon, and a low-precision Earth orbit (plus Jupiter and Saturn's pull on the Sun) for barycentric corrections, good to about two seconds. TT \xE2\x88\x92 UTC is held at 69.184 s, so dates before 1972 can be off by up to a minute. For publication-grade timing, use astropy or JPL Horizons.", F_SMALL, cFg) + 14;
  tx(X, y, "Credits", F_H2, cFg); y += F_H2.lh + 4;
  y += wrap(X, y, CW, "Mars calendar method after Clancy et al. (2000) and Piqueux et al. (2015) for Mars Year numbering; Lunar Standard Time after Kenneth L. Lee (1970).", F_SMALL, cFg) + 6;
  y += wrap(X, y, CW, "Made by Ella Agnes Puerto. Source on GitHub: eagnespuerto/astrochronist-calendar.", F_SMALL, cMuted) + 14;
  tx(X, y, "This device", F_H2, cFg); y += F_H2.lh + 4;
  char a[160];
  snprintf(a, sizeof a, "Clock: %s. %s. Local time is %s.", clockSource, wifiNote, tzName);
  y += wrap(X, y, CW, a, F_SMALL, cMuted);
  contentH[T_ABOUT] = y + scrollY[T_ABOUT] - HEAD + 14;
}

/* ======================= frame ======================= */
static bool capture = false;

static void drawPage() {
  switch (tab) {
    case T_NOW: drawNow(); break;
    case T_CAL: drawCalendar(); break;
    case T_SCI: drawScience(); break;
    case T_ABOUT: drawAbout(); break;
  }
}

static int maxScroll(Tab t) { int m = contentH[t] - (H - HEAD); return m > 0 ? m : 0; }

static void render() {
  for (int band = 0; band < 3; band++) {
    OY = band * BH;
    recHits = band == 0;
    if (recHits) nHits = 0;
    spr.fillScreen(tab == T_NOW ? C(0x0A0E16) : cBg);
    drawPage();
    drawHeader();
    spr.pushSprite(lcd, 0, OY);
    if (capture) Serial.write((const uint8_t*)spr.getBuffer(), W * BH * 2);
  }
}

// Lays out once to learn positions, applies any pending scroll, clamps, then renders.
static void frame() {
  if (pendingScroll && tab == T_CAL) {
    dry = true; OY = 0; drawPage(); dry = false;
    if (pendingScroll == 1) scrollY[T_CAL] = detailTop - HEAD - 6;
    else if (pendingScroll == 2 && selCellY >= 0) scrollY[T_CAL] = selCellY - HEAD - (H - HEAD) / 2 + 10;
    else if (pendingScroll == 3) scrollY[T_CAL] = cardTop[bandMonth] - HEAD - 6;
    pendingScroll = 0;
  }
  if (tab != T_NOW) {
    if (contentH[tab] == 0) { dry = true; OY = 0; drawPage(); dry = false; }
    if (scrollY[tab] > maxScroll(tab)) scrollY[tab] = maxScroll(tab);
    if (scrollY[tab] < 0) scrollY[tab] = 0;
  } else scrollY[tab] = 0;
  render();
}

/* ======================= actions ======================= */
static void saveSys() { prefs.putUChar("nowSys", nowSys); prefs.putUChar("calSys", calSys); }

static void act(const Hit& h) {
  switch (h.act) {
    case A_TAB: tab = (Tab)h.arg; break;
    case A_NOWSYS: nowSys = (SysId)h.arg; saveSys(); break;
    case A_CALSYS:
      calSys = (SysId)h.arg; saveSys();
      calYear = yearOf(calSys, isnan(selMs) ? nowMs() : selMs);
      break;
    case A_PREV: calYear = clampYear(calSys, calYear - 1); break;
    case A_NEXT: calYear = clampYear(calSys, calYear + 1); break;
    case A_TODAY: selMs = atMs = nowMs(); calYear = yearOf(calSys, selMs); pendingScroll = 2; break;
    case A_CELL: selMs = YC->start[h.arg]; atMs = NAN; pendingScroll = 1; break;
    case A_BAND: bandMonth = h.arg; pendingScroll = 3; break;
    case A_SCIBASE: sciBase = (SysId)h.arg; prefs.putUChar("sciBase", sciBase); break;
    case A_MY: exactMY += h.arg; if (exactMY < MY_MIN) exactMY = MY_MIN; if (exactMY > MY_MAX) exactMY = MY_MAX; break;
    case A_PSEC: PL.sec = !PL.sec; prefs.putBool("plSec", PL.sec); break;
    case A_PCAL: PL.cal = !PL.cal; prefs.putBool("plCal", PL.cal); break;
  }
}

static bool tapAt(int x, int y) {
  for (int i = nHits - 1; i >= 0; i--) {
    const Hit& h = hits[i];
    if (x >= h.x - 2 && x < h.x + h.w + 2 && y >= h.y - 2 && y < h.y + h.h + 2) {
      Serial.printf("tap %d,%d -> act %d arg %d\n", x, y, h.act, h.arg);
      act(h); frame(); return true;
    }
  }
  Serial.printf("tap %d,%d -> nothing\n", x, y);
  return false;
}

/* ======================= touch ======================= */
static int calX0 = 200, calX1 = 3700, calY0 = 240, calY1 = 3800;
static bool rot180 = false;
static bool touchDown = false, touchMoved = false;
static int tx0, ty0, sy0, lastTy;
static uint32_t lastMoveDraw = 0;

// Affine touch calibration: screen = A * (rawX, rawY, 1). Set by calibrateTouch(), kept in NVS.
static bool affOk = false;
static float aff[6];

static bool readTouch(int& x, int& y) {
  if (!ts.tirqTouched() || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (affOk) {
    x = (int)lroundf(aff[0] * p.x + aff[1] * p.y + aff[2]);
    y = (int)lroundf(aff[3] * p.x + aff[4] * p.y + aff[5]);
  } else {
    x = map(p.x, calX0, calX1, 0, W); y = map(p.y, calY0, calY1, 0, H);
    if (rot180) { x = W - 1 - x; y = H - 1 - y; }
  }
  x = constrain(x, 0, W - 1); y = constrain(y, 0, H - 1);
  return true;
}

// Least-squares fit of v = a*rx + b*ry + c over n points (3x3 normal equations, Cramer's rule).
static bool fitAxis(const double* rx, const double* ry, const double* v, int n, float* out) {
  double sxx = 0, sxy = 0, sx = 0, syy = 0, sy = 0, bx = 0, by = 0, b1 = 0;
  for (int i = 0; i < n; i++) {
    sxx += rx[i] * rx[i]; sxy += rx[i] * ry[i]; sx += rx[i]; syy += ry[i] * ry[i]; sy += ry[i];
    bx += rx[i] * v[i]; by += ry[i] * v[i]; b1 += v[i];
  }
  double M[3][3] = { { sxx, sxy, sx }, { sxy, syy, sy }, { sx, sy, (double)n } }, B[3] = { bx, by, b1 };
  auto det = [](double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
  double D = det(M);
  if (fabs(D) < 1e-9) return false;
  for (int k = 0; k < 3; k++) {
    double T[3][3]; memcpy(T, M, sizeof T);
    for (int r = 0; r < 3; r++) T[r][k] = B[r];
    out[k] = (float)(det(T) / D);
  }
  return true;
}

static void frame();
static void calibrateTouch() {
  const int px[4] = { 30, W - 30, W - 30, 30 }, py[4] = { 30, 30, H - 30, H - 30 };
  double rx[4], ry[4], sxv[4], syv[4];
  for (int i = 0; i < 4; i++) {
    lcd->fillScreen(TFT_BLACK);
    lcd->setFont(&fonts::DejaVu12); lcd->setTextColor(TFT_WHITE); lcd->setTextDatum(lgfx::middle_center);
    lcd->drawString("Touch calibration", W / 2, H / 2 - 10);
    lcd->drawString("Tap the centre of the red cross", W / 2, H / 2 + 8);
    lcd->drawFastHLine(px[i] - 12, py[i], 25, TFT_RED); lcd->drawFastVLine(px[i], py[i] - 12, 25, TFT_RED);
    lcd->drawCircle(px[i], py[i], 6, TFT_RED);
    while (ts.touched()) delay(10);           // wait for release from the previous point
    delay(250);
    while (!ts.touched()) delay(10);
    long ax = 0, ay = 0; int n = 0;
    while (ts.touched()) { TS_Point p = ts.getPoint(); if (n < 64) { ax += p.x; ay += p.y; n++; } delay(8); }
    rx[i] = n ? (double)ax / n : 0; ry[i] = n ? (double)ay / n : 0; sxv[i] = px[i]; syv[i] = py[i];
    Serial.printf("cal point %d screen %d,%d raw %.0f,%.0f\n", i, px[i], py[i], rx[i], ry[i]);
  }
  float a[6];
  if (fitAxis(rx, ry, sxv, 4, a) && fitAxis(rx, ry, syv, 4, a + 3)) {
    memcpy(aff, a, sizeof aff); affOk = true;
    prefs.putBytes("aff", aff, sizeof aff);
    Serial.printf("cal ok x=%.4f*rx%+.4f*ry%+.1f y=%.4f*rx%+.4f*ry%+.1f\n", a[0], a[1], a[2], a[3], a[4], a[5]);
  } else Serial.println("cal failed, keeping the old mapping");
  frame();
}

static void pollTouch() {
  int x, y;
  if (readTouch(x, y)) {
    if (!touchDown) { touchDown = true; touchMoved = false; tx0 = x; ty0 = y; sy0 = scrollY[tab]; lastTy = y; return; }
    if (abs(y - ty0) > 8 || abs(x - tx0) > 12) touchMoved = true;
    if (touchMoved && tab != T_NOW && y != lastTy && millis() - lastMoveDraw > 40) {
      scrollY[tab] = sy0 - (y - ty0);
      lastTy = y; lastMoveDraw = millis();
      frame();
    }
  } else if (touchDown) {
    touchDown = false;
    if (!touchMoved) tapAt(tx0, ty0);
  }
}

/* ======================= time sources ======================= */
static void markSet(const char* how) {
  timeSet = true;
  char t[48]; localHM(nowMs(), t, sizeof t);
  snprintf(clockSource, sizeof clockSource, "%s at %s", how, t);
}
static void onSntp(struct timeval*) { markSet("synced over Wi-Fi"); }

static void wifiTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  bool sntpOn = false;
  const int nNets = sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]);
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      for (int i = 0; i < nNets && WiFi.status() != WL_CONNECTED; i++) {
        snprintf(wifiNote, sizeof wifiNote, "Wi-Fi: trying %s", WIFI_NETS[i][0]);
        Serial.printf("%s\n", wifiNote);
        WiFi.disconnect(true); delay(100);
        WiFi.begin(WIFI_NETS[i][0], WIFI_NETS[i][1]);
        for (int k = 0; k < 40 && WiFi.status() != WL_CONNECTED; k++) delay(250);
      }
      if (WiFi.status() == WL_CONNECTED) {
        snprintf(wifiNote, sizeof wifiNote, "Wi-Fi: on %s", WiFi.SSID().c_str());
        Serial.printf("%s, IP %s\n", wifiNote, WiFi.localIP().toString().c_str());
        if (!sntpOn) {
          sntp_set_time_sync_notification_cb(onSntp);
          configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
          sntpOn = true;
        }
      } else {
        snprintf(wifiNote, sizeof wifiNote, "Wi-Fi: no known network found");
        Serial.printf("%s\n", wifiNote);
      }
    }
    delay(30000);
  }
}

/* ======================= serial commands ======================= */
static void setClock(double ms) {
  struct timeval tv; tv.tv_sec = (time_t)floor(ms / 1000); tv.tv_usec = (suseconds_t)((ms - tv.tv_sec * 1000.0) * 1000);
  settimeofday(&tv, nullptr);
}

static void handleLine(char* l) {
  switch (l[0]) {
    case 'T': setClock(atof(l + 1)); markSet("set over USB"); Serial.println("ok time"); break;
    case 'Z': {
      int off = atoi(l + 1); char* sp = strchr(l + 1, ' ');
      tzOffsetMin = off; prefs.putInt("tz", off);
      if (sp) { snprintf(tzName, sizeof tzName, "%s", sp + 1); prefs.putString("tzName", tzName); }
      lSys = NSYS; Serial.printf("ok tz %d %s\n", tzOffsetMin, tzName); break;
    }
    case 'G':
      if (l[1] == '-') { hasGeo = false; prefs.remove("lat"); }
      else { geoLat = atof(l + 1); char* sp = strchr(l + 1, ' '); geoLon = sp ? atof(sp) : 0; hasGeo = true; prefs.putDouble("lat", geoLat); prefs.putDouble("lon", geoLon); }
      Serial.println("ok geo"); break;
    case 'P': { int x = atoi(l + 1); char* sp = strchr(l + 1, ' '); int y = sp ? atoi(sp) : 0; tapAt(x, y); break; }
    case 'S': scrollY[tab] += atoi(l + 1); frame(); Serial.printf("ok scroll %d/%d\n", scrollY[tab], maxScroll(tab)); break;
    case 'N': tab = (Tab)constrain(atoi(l + 1), 0, 3); frame(); Serial.println("ok tab"); break;
    case 'C': Serial.printf("SHOT %d %d\n", W, H); Serial.flush(); capture = true; frame(); capture = false; Serial.flush(); Serial.println("\nENDSHOT"); break;
    case 'D': prefs.putUChar("panel", (uint8_t)atoi(l + 1)); Serial.println("ok panel set, restarting"); delay(100); ESP.restart(); break;
    case 'I': prefs.putBool("invert", !prefs.getBool("invert", false)); Serial.println("ok invert toggled, restarting"); delay(100); ESP.restart(); break;
    case 'R': prefs.putBool("rot180", !prefs.getBool("rot180", false)); Serial.println("ok rotation toggled, restarting"); delay(100); ESP.restart(); break;
    case 'X': calibrateTouch(); break;
    case 'K': sscanf(l + 1, "%d %d %d %d", &calX0, &calX1, &calY0, &calY1); prefs.putInt("cx0", calX0); prefs.putInt("cx1", calX1); prefs.putInt("cy0", calY0); prefs.putInt("cy1", calY1); Serial.println("ok touch cal"); break;
    case '?': {
      char t[48]; localFull(nowMs(), t, sizeof t);
      Serial.printf("time %s | set=%d (%s) | %s | heap %u | tab %d\n", t, (int)timeSet, clockSource, wifiNote, ESP.getFreeHeap(), tab);
      break;
    }
  }
}

static void pollSerial() {
  static char buf[96]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (n) { buf[n] = 0; handleLine(buf); n = 0; } }
    else if (n < 95) buf[n++] = c;
  }
}

/* ======================= setup / loop ======================= */
static double buildTimeMs() {
  static const char* mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char m[4] = { __DATE__[0], __DATE__[1], __DATE__[2], 0 };
  int mo = (strstr(mon, m) - mon) / 3, d = atoi(__DATE__ + 4), y = atoi(__DATE__ + 7);
  int hh = atoi(__TIME__), mi = atoi(__TIME__ + 3), ss = atoi(__TIME__ + 6);
  return localMs(y, mo, d, hh, mi, ss);
}

static bool hasBytes(uint32_t v, uint8_t a, uint8_t b) {
  for (int i = 0; i < 3; i++) if (((v >> (8 * i)) & 0xFF) == a && ((v >> (8 * (i + 1))) & 0xFF) == b) return true;
  return false;
}

void setup() {
  Serial.begin(921600);
  delay(50);
  prefs.begin("astro", false);
  tzOffsetMin = prefs.getInt("tz", 480);
  snprintf(tzName, sizeof tzName, "%s", prefs.getString("tzName", "UTC+08:00").c_str());
  nowSys = (SysId)prefs.getUChar("nowSys", GREG); if (nowSys >= NSYS) nowSys = GREG;
  calSys = (SysId)prefs.getUChar("calSys", MARS); if (calSys >= NSYS) calSys = MARS;
  sciBase = (SysId)prefs.getUChar("sciBase", SGRA);
  PL.sec = prefs.getBool("plSec", true); PL.cal = prefs.getBool("plCal", false);
  if (prefs.isKey("lat")) { hasGeo = true; geoLat = prefs.getDouble("lat", 0); geoLon = prefs.getDouble("lon", 0); }
  calX0 = prefs.getInt("cx0", calX0); calX1 = prefs.getInt("cx1", calX1); calY0 = prefs.getInt("cy0", calY0); calY1 = prefs.getInt("cy1", calY1);
  affOk = prefs.getBytes("aff", aff, sizeof aff) == sizeof aff;
  rot180 = prefs.getBool("rot180", false);

  cBg = C(0xEDF0F3); cSurface = C(0xFFFFFF); cLine = C(0xCBD3DC); cFg = C(0x16202B); cMuted = C(0x5A6878); cWarn = C(0xB4472A);

  // Display: the CYD ships with either an ILI9341 or an ST7789. Ask the panel which one it is.
  bool inv = prefs.getBool("invert", false);
  lcd = new LGFX(false, inv);
  lcd->init();
  uint32_t idD3 = lcd->getPanel()->readCommand(0xD3, 0, 4), id04 = lcd->getPanel()->readCommand(0x04, 0, 4);
  int forced = prefs.getUChar("panel", 0);   // 0 auto, 1 ILI9341, 2 ST7789 (serial command D)
  bool st = forced ? forced == 2 : (!hasBytes(idD3, 0x93, 0x41) && hasBytes(id04, 0x85, 0x52));
  Serial.printf("panel id D3=%08x 04=%08x forced=%d -> %s\n", idD3, id04, forced, st ? "ST7789" : "ILI9341");
  if (st) { delete lcd; lcd = new LGFX(true, !inv); lcd->init(); }
  lcd->setRotation(rot180 ? 3 : 1);
  lcd->setBrightness(255);
  lcd->fillScreen(TFT_BLACK);

  spr.setColorDepth(16);
  if (!spr.createSprite(W, BH)) Serial.println("sprite alloc failed");

  touchSpi.begin(25, 39, 32, 33);
  ts.begin(touchSpi);
  ts.setRotation(1);

  initMars();
  L = (YearLayout*)malloc(sizeof(YearLayout));
  YC = (YearCells*)malloc(sizeof(YearCells));

  setClock(buildTimeMs());   // a rough start until Wi-Fi or USB sets the real time
  { int y; if (yearOfSol((int64_t)floor(msdFromMs(nowMs())), y)) exactMY = y; }

#ifdef NO_WIFI
  snprintf(wifiNote, sizeof wifiNote, "Wi-Fi: not set up in this build");
#else
  xTaskCreatePinnedToCore(wifiTask, "wifi", 6144, nullptr, 1, nullptr, 0);
#endif
  Serial.printf("Astrochronist ready, heap %u, MY %d..%d\n", ESP.getFreeHeap(), MY_MIN, MY_MAX);
  if (!affOk) calibrateTouch(); else frame();
}

void loop() {
  pollSerial();
  pollTouch();
  static uint32_t last = 0; static bool wasSet = false;
  uint32_t ms = millis();
  if (wasSet != timeSet) {   // real time arrived: move the calendar and science pages to it
    wasSet = timeSet; calYear = INT64_MIN; selMs = atMs = NAN; lSys = NSYS;
    { int y; if (yearOfSol((int64_t)floor(msdFromMs(nowMs())), y)) exactMY = y; }
    frame(); last = ms;
  }
  if (touchDown) return;
  uint32_t period = tab == T_NOW ? 250 : tab == T_SCI ? 1000 : tab == T_CAL ? 30000 : 5000;
  if (ms - last >= period) { frame(); last = ms; }
  delay(5);
}
