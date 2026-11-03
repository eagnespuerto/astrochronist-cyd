// Calendar adapters, ported from js/systems.js.
#include "systems.h"
#include "astro.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

using namespace astro;

const char* const MONTHS[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
const char* const WEEKDAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* const MON3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char* const WD3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

const System SYS[NSYS] = {
  { GREG, "Gregorian", "Gregorian", "Gregorian \xC2\xB7 local & UTC", "Day", "Month", nullptr, "Year" },
  { MARS, "Martian", "Martian", "Martian \xC2\xB7 Mars Year & MTC", "Sol", "Month", nullptr, "Mars Year" },
  { LUNAR, "Lunar Standard", "Lunar", "Lunar Standard Time", "Cycle", "Lunar day", nullptr, "Lunar year" },
  { SGRA, "BJD \xC2\xB7 Sol\xE2\x80\x93Sgr A*", "Sgr A*", "BJD \xC2\xB7 Sol\xE2\x80\x93Sgr A*", "Day", "Hectoday", "Kiloday", "Year" },
  { ACEN, "BJD \xC2\xB7 Sol\xE2\x80\x93\xCE\xB1 Cen", "\xCE\xB1 Cen", "BJD \xC2\xB7 Sol\xE2\x80\x93\xCE\xB1 Cen", "Day", "Hectoday", "Kiloday", "Year" },
};
static const Base& baseOf(SysId s) { return s == ACEN ? BASE_ACEN : BASE_SGRA; }

/* ---------------- formatting ---------------- */
void fmtInt(int64_t v, char* buf, size_t n) {
  char t[32]; int len = snprintf(t, sizeof t, "%lld", (long long)(v < 0 ? -v : v));
  char o[40]; int j = 0;
  if (v < 0) o[j++] = '-';
  for (int i = 0; i < len; i++) { o[j++] = t[i]; if ((len - i - 1) % 3 == 0 && i < len - 1) o[j++] = ','; }
  o[j] = 0;
  snprintf(buf, n, "%s", o);
}
void fmtFix(double v, int p, char* buf, size_t n) {
  char t[48]; snprintf(t, sizeof t, "%.*f", p, fabs(v));
  char* dot = strchr(t, '.');
  int64_t ip = (int64_t)atoll(t);
  char ib[32]; fmtInt(ip, ib, sizeof ib);
  snprintf(buf, n, "%s%s%s", v < 0 && (ip || atof(t) != 0) ? "-" : "", ib, dot ? dot : "");
}
void hms(double frac, char* buf) {
  int64_t t = (int64_t)floor(fmodp(frac, 1) * 86400 + 1e-6);
  snprintf(buf, 9, "%02d:%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60));
}
static void yearStr(int64_t y, char* b, size_t n) { snprintf(b, n, "%lld", (long long)y); }
void isoUTC(double ms, char* buf) {
  DT d = utcParts(ms);
  snprintf(buf, 24, "%04lld-%02d-%02d %02d:%02d:%02d", (long long)d.y, d.mo + 1, d.d, d.h, d.mi, d.s);
}
void utcDate(double ms, char* buf, size_t n) { DT d = utcParts(ms); char y[16]; yearStr(d.y, y, 16); snprintf(buf, n, "%d %s %s", d.d, MON3[d.mo], y); }
void utcShort(double ms, char* buf, size_t n) { DT d = utcParts(ms); snprintf(buf, n, "%d %s", d.d, MON3[d.mo]); }
void utcFull(double ms, char* buf, size_t n) {
  DT d = utcParts(ms); char y[16]; yearStr(d.y, y, 16);
  snprintf(buf, n, "%s %d %s %s, %02d:%02d", WD3[d.wd], d.d, MON3[d.mo], y, d.h, d.mi);
}
void localHM(double ms, char* buf, size_t n) {
  DT d = localParts(ms); char y[16]; yearStr(d.y, y, 16);
  snprintf(buf, n, "%s %d %s %s, %02d:%02d", WD3[d.wd], d.d, MON3[d.mo], y, d.h, d.mi);
}
void localFull(double ms, char* buf, size_t n) { char t[48]; localHM(ms, t, sizeof t); snprintf(buf, n, "%s %s", t, tzName); }
void localDate(double ms, char* buf, size_t n) { DT d = localParts(ms); char y[16]; yearStr(d.y, y, 16); snprintf(buf, n, "%s %d %s %s", WD3[d.wd], d.d, MON3[d.mo], y); }
void localDayHMS(double ms, char* buf, size_t n) {
  DT d = localParts(ms);
  snprintf(buf, n, "%s %d %s, %02d:%02d:%02d %s", WD3[d.wd], d.d, MON3[d.mo], d.h, d.mi, d.s, tzName);
}
void localHMonly(double ms, char* buf, size_t n) { DT d = localParts(ms); snprintf(buf, n, "%02d:%02d", d.h, d.mi); }

/* ---------------- Gregorian helpers ---------------- */
static int daysInMonth(int64_t y, int m) {  // m 0-based
  static const int L[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  return m == 1 && isLeapG(y) ? 29 : L[m];
}
static int isoWeek(int64_t y, int m, int d) {
  int64_t days = daysFromCivil(y, m + 1, d);
  int dow = (int)imod(days + 4, 7), iso = dow ? dow : 7;
  int64_t th = days + 4 - iso;
  Civil c = civilFromDays(th);
  return (int)((th - daysFromCivil(c.y, 1, 1)) / 7) + 1;
}

/* ---------------- Mars helpers ---------------- */
static void marsDateStr(int64_t m, char* b, size_t n) {
  MarsDate d;
  if (marsDate(m, d)) snprintf(b, n, "MY %d \xC2\xB7 M%d sol %d", d.my, d.month, d.sol);
  else { char t[24]; fmtInt(m, t, sizeof t); snprintf(b, n, "MSD %s", t); }
}
static void marsEvents(int n, int64_t& aph, int64_t& peri) {
  double a = astroStart(n), base = 360 * round(lcont(a) / 360);
  aph = (int64_t)floor(solveL(base + 71, a + 218));
  peri = (int64_t)floor(solveL(base + 251, a + 485));
}

/* ---------------- shared API ---------------- */
int64_t yearOf(SysId s, double ms) {
  switch (s) {
    case GREG: return localParts(ms).y;
    case MARS: { int y; if (yearOfSol((int64_t)floor(msdFromMs(ms)), y)) return y; return ms < 0 ? MY_MIN : MY_MAX; }
    case LUNAR: return lst(ms).year;
    default: return (int64_t)floor(floor(bjdFromMs(ms, baseOf(s))) / 1000);
  }
}
int64_t clampYear(SysId s, int64_t y) {
  if (s == MARS) return y < MY_MIN ? MY_MIN : y > MY_MAX ? MY_MAX : y;
  return y;
}

static const char* FOOT_GREG = "Gregorian days are civil days in your time zone, so they shift with daylight saving. Every other calendar here counts from a sky event instead.";
static const char* FOOT_MARS = "Months are 30\xC2\xB0 slices of solar longitude (Ls) rounded to whole sols. Odd years and years divisible by 10 have 669 sols; the leap sol ends M12. Sol 1 of MY 1 is the sol of the 1955 northern spring equinox, and weeks run on unbroken from it. Sols run from Airy-0 midnight. The Science tab has the exact Ls month boundaries.";
static const char* FOOT_LUNAR = "Lunar Standard Time (Kenneth L. Lee, 1970) starts at 1-01-01 \xE2\x88\x87 00:00:00, the moment Neil Armstrong stepped onto the Moon (1969-07-21 02:56:15 UTC). A cycle is 24 lunar hours, each 0.984 Earth hours long, so cycles drift slowly against Earth days. Small dates under each cycle are UTC.";
static char footBjd[2][900];

void layout(SysId s, int64_t y, YearLayout& L) {
  memset(&L, 0, sizeof L);
  char a[48], b[48], c[48];
  if (s == GREG) {
    if (y > 0) snprintf(L.title, sizeof L.title, "%lld", (long long)y); else snprintf(L.title, sizeof L.title, "%lld BC", (long long)(1 - y));
    L.nMonths = 12;
    for (int m = 0; m < 12; m++) {
      MonthCard& k = L.months[m];
      int nd = daysInMonth(y, m);
      snprintf(k.title, sizeof k.title, "%s", MONTHS[m]);
      snprintf(k.right, sizeof k.right, "%d days", nd);
      k.cols = 7; k.headers = 1; k.n = nd;
      k.lead = (uint8_t)imod(daysFromCivil(y, m + 1, 1) + 4, 7);
      snprintf(L.band[m].label, 6, "%.3s", MONTHS[m]);
      snprintf(L.band[m].value, 6, "%d", nd);
      L.band[m].weight = nd;
    }
    bool lp = isLeapG(y);
    snprintf(L.subtitle, sizeof L.subtitle, "%d days, %s year. Days run midnight to midnight in %s.", lp ? 366 : 365, lp ? "leap" : "common", tzName);
    snprintf(L.bandNote, sizeof L.bandNote, "Month lengths follow Roman custom, not the sky.");
    L.foot = FOOT_GREG;
  } else if (s == MARS) {
    int n = (int)y, lens[12];
    monthLengths(n, lens);
    snprintf(L.title, sizeof L.title, "Mars Year %d", n);
    int64_t m0 = wb(n);
    L.nMonths = 12;
    for (int i = 0; i < 12; i++) {
      MonthCard& k = L.months[i];
      snprintf(k.title, sizeof k.title, "M%d", i + 1);
      snprintf(k.sub, sizeof k.sub, "Ls %d\xC2\xB0\xE2\x80\x93%d\xC2\xB0", 30 * i, 30 * i + 30);
      snprintf(k.right, sizeof k.right, "%d sols", lens[i]);
      utcShort(msFromMsd(m0), a, sizeof a); utcDate(msFromMsd(m0 + lens[i] - 1), b, sizeof b);
      snprintf(k.meta, sizeof k.meta, "%s, %s to %s", SEASONS[i / 3], a, b);
      k.cols = 7; k.headers = 1; k.lead = (uint8_t)imod(m0 - wb(1), 7); k.n = lens[i]; k.accent = i >= 6;
      snprintf(L.band[i].label, 6, "M%d", i + 1);
      snprintf(L.band[i].value, 6, "%d", lens[i]);
      L.band[i].weight = lens[i]; L.band[i].accent = i >= 6;
      m0 += lens[i];
    }
    int lo = 0, hi = 0;
    for (int i = 1; i < 12; i++) { if (lens[i] > lens[hi]) hi = i; if (lens[i] < lens[lo]) lo = i; }
    utcDate(msFromMsd(wb(n)), a, sizeof a); utcDate(msFromMsd(wb(n + 1) - 1), b, sizeof b);
    snprintf(L.subtitle, sizeof L.subtitle, "%d sols, %s year. %s to %s on Earth.", marsYearLen(n), marsLeap(n) ? "leap" : "common", a, b);
    snprintf(L.bandNote, sizeof L.bandNote, "Widths to scale: Mars moves slowest near aphelion, so M%d runs %d sols longer than M%d.", hi + 1, lens[hi] - lens[lo], lo + 1);
    snprintf(L.bandAccent, sizeof L.bandAccent, "Dust storm season, Ls 180\xC2\xB0 to 360\xC2\xB0");
    L.foot = FOOT_MARS;
  } else if (s == LUNAR) {
    snprintf(L.title, sizeof L.title, "Lunar Year %lld", (long long)y);
    L.nMonths = 12;
    for (int d = 1; d <= 12; d++) {
      MonthCard& k = L.months[d - 1];
      snprintf(k.title, sizeof k.title, "Day %d", d);
      snprintf(k.right, sizeof k.right, "30 cycles");
      utcShort(msFromLst(y, d, 1), a, sizeof a); utcDate(msFromLst(y, d, 30), b, sizeof b);
      snprintf(k.meta, sizeof k.meta, "%s to %s", a, b);
      k.cols = 10; k.headers = 0; k.n = 30;
      snprintf(L.band[d - 1].label, 6, "D%d", d);
      L.band[d - 1].weight = 1;
    }
    utcFull(msFromLst(y, 1, 1), c, sizeof c);
    snprintf(L.subtitle, sizeof L.subtitle, "12 lunar days of %.3f Earth days each (%.2f days). Starts %s UTC.", SYNODIC, 12 * SYNODIC, c);
    snprintf(L.bandNote, sizeof L.bandNote, "Each lunar day is one mean synodic month, new moon to new moon, so the phases land on the same cycle every lunar day.");
    L.foot = FOOT_LUNAR;
  } else {
    const Base& B = baseOf(s);
    int64_t K = y, nowN = (int64_t)floor(bjdFromMs(nowMs(), B));
    char kb[24]; fmtInt(K, kb, sizeof kb);
    snprintf(L.title, sizeof L.title, "Kiloday %s", kb);
    L.nMonths = 10;
    for (int h = 0; h < 10; h++) {
      MonthCard& k = L.months[h];
      snprintf(k.title, sizeof k.title, "Hectoday %d", h);
      snprintf(k.sub, sizeof k.sub, "days %d00\xE2\x80\x93%d99", h, h);
      snprintf(k.right, sizeof k.right, "100 days");
      int64_t N0 = K * 1000 + h * 100;
      utcShort(msFromBjd(N0, B), a, sizeof a); utcDate(msFromBjd(N0 + 99, B), b, sizeof b);
      snprintf(k.meta, sizeof k.meta, "%s to %s", a, b);
      k.cols = 10; k.headers = 2; k.n = 100;
      k.accent = nowN >= N0 && nowN < N0 + 100;
      snprintf(L.band[h].label, 6, "H%d", h);
      L.band[h].weight = 1; L.band[h].accent = k.accent;
    }
    BjdYear ya = bjdYear(K * 1000), yb = bjdYear(K * 1000 + 999);
    char d0[24], d1[24], y0[24], y1[24];
    fmtInt(K * 1000, d0, 24); fmtInt(K * 1000 + 999, d1, 24); fmtInt(ya.year, y0, 24); fmtInt(yb.year, y1, 24);
    utcDate(msFromBjd(K * 1000, B), a, sizeof a); utcDate(msFromBjd(K * 1000 + 999, B), b, sizeof b);
    snprintf(L.subtitle, sizeof L.subtitle, "Days %s to %s, from Year %s day %lld to Year %s day %lld: %s to %s. Day numbers are BJD_TDB toward %s.",
             d0, d1, y0, (long long)ya.doy, y1, (long long)yb.doy, a, b, B.longName);
    snprintf(L.bandNote, sizeof L.bandNote, "Ten hectodays of 100 days make a kiloday. The highlighted hectoday holds today.");
    char* f = footBjd[s == ACEN];
    snprintf(f, 900, "Each day starts when the barycentric Julian Date toward %s (%s) passes a whole number, near noon UTC. That moment shifts by a few minutes over the year as Earth swings around the Sun's barycentre, but everyone on Earth shares the same day number at once. Years are 365.25 days counted from BJD 0, so Year 1 starts at second 0; a day belongs to the year it starts in, which gives 366, 365, 365 and 365 days in each four-year cycle. Highlighted days begin a new year. Small dates under each day are the UTC date it starts on.", B.name, B.coords);
    L.foot = f;
  }
}

static void addNote(YearCells& C, int i, const char* fmt, ...) {
  if (C.nNotes >= 12) return;
  C.noteIdx[C.nNotes] = i;
  va_list ap; va_start(ap, fmt); vsnprintf(C.note[C.nNotes], 96, fmt, ap); va_end(ap);
  C.nNotes++;
}

void yearCells(SysId s, int64_t y, const YearLayout& L, YearCells& C) {
  C.n = 0; C.nNotes = 0;
  int n = 0;
  for (int m = 0; m < L.nMonths; m++) {
    C.monthFirst[m] = n;
    for (int k = 0; k < L.months[m].n; k++) C.cls[n + k] = 0;
    n += L.months[m].n;
  }
  C.monthFirst[L.nMonths] = n; C.n = n;
  if (s == GREG) {
    int i = 0;
    for (int m = 0; m < 12; m++) for (int d = 1; d <= L.months[m].n; d++) C.start[i++] = localMs(y, m, d);
    C.start[i] = localMs(y + 1, 0, 1);
  } else if (s == MARS) {
    int ny = (int)y; int64_t m0 = wb(ny), aph, peri;
    marsEvents(ny, aph, peri);
    int64_t leapSol = marsLeap(ny) ? wb(ny + 1) - 1 : INT64_MIN;
    for (int i = 0; i <= n; i++) C.start[i] = msFromMsd(m0 + i);
    for (int i = 0; i < n; i++) {
      int64_t m = m0 + i;
      if (m == aph) { C.cls[i] = 1; addNote(C, i, "Aphelion (Ls 71\xC2\xB0)"); }
      if (m == peri) { C.cls[i] = 1; addNote(C, i, "Perihelion (Ls 251\xC2\xB0)"); }
      if (m == leapSol) { C.cls[i] = 2; addNote(C, i, "Leap sol"); }
    }
  } else if (s == LUNAR) {
    for (int i = 0; i <= n; i++) C.start[i] = msFromLst(y, i / 30 + 1, i % 30 + 1);
    double t0 = C.start[0], t1 = C.start[n];
    int64_t k0 = (int64_t)floor((t0 - meanPhaseMs(0)) / (SYNODIC * 864e5)) - 1;
    for (int64_t k = k0; k < k0 + 16; k++) for (int h = 0; h < 2; h++) {
      double t = meanPhaseMs(k + h * 0.5);
      if (t < t0 || t >= t1) continue;
      for (int i = 0; i < n; i++) if (t >= C.start[i] && t < C.start[i + 1]) {
        C.cls[i] = 1; addNote(C, i, h ? "Full moon (mean)" : "New moon (mean)"); break;
      }
    }
  } else {
    const Base& B = baseOf(s);
    for (int i = 0; i <= n; i++) C.start[i] = msFromBjd(y * 1000 + i, B);
    for (int i = 0; i < n; i++) {
      int64_t N = y * 1000 + i; BjdYear Y = bjdYear(N);
      if (Y.doy != 1) continue;
      double y0 = bjdYearStart(Y.year);
      char yb[24]; fmtInt(Y.year, yb, sizeof yb);
      C.cls[i] = 1;
      if (y0 == (double)N) addNote(C, i, "Year %s begins", yb);
      else {
        char fb[24]; fmtInt((int64_t)floor(y0), fb, sizeof fb);
        char fr[8]; snprintf(fr, sizeof fr, "%.2f", fmodp(y0, 1));
        addNote(C, i, "Year %s begins (the year itself starts at BJD %s%s, during the day before)", yb, fb, fr + 1);
      }
    }
  }
}

void cellLabel(SysId s, const YearCells& C, int month, int i, char* label, char* sub) {
  sub[0] = 0;
  if (s == SGRA || s == ACEN) snprintf(label, 6, "%03d", i);
  else snprintf(label, 6, "%d", i - C.monthFirst[month] + 1);
  if (s == LUNAR || s == SGRA || s == ACEN) utcShort(C.start[i], sub, 8);
}

void label(SysId s, double ms, char* buf, size_t n) {
  char a[64], t[12];
  switch (s) {
    case GREG: localHM(ms, buf, n); return;
    case MARS: { double msd = msdFromMs(ms); marsDateStr((int64_t)floor(msd), a, sizeof a); hms(msd, t); t[5] = 0; snprintf(buf, n, "%s, %s MTC", a, t); return; }
    case LUNAR: { LST p = lst(ms); hms(p.frac, t); snprintf(buf, n, "%lld-%02d-%02d \xE2\x88\x87 %s", (long long)p.year, p.day, p.cycle, t); return; }
    default: {
      double b = bjdFromMs(ms, baseOf(s)); BjdYear Y = bjdYear((int64_t)floor(b));
      char yb[24]; fmtInt(Y.year, yb, sizeof yb);
      snprintf(buf, n, "BJD %.5f \xC2\xB7 Year %s day %lld", floor(b * 1e5) / 1e5, yb, (long long)Y.doy);
    }
  }
}

void dayLabel(SysId s, double ms, char* buf, size_t n) {
  switch (s) {
    case GREG: localDate(ms, buf, n); return;
    case MARS: marsDateStr((int64_t)floor(msdFromMs(ms)), buf, n); return;
    case LUNAR: { LST p = lst(ms); snprintf(buf, n, "%lld-%02d-%02d", (long long)p.year, p.day, p.cycle); return; }
    default: {
      int64_t N = (int64_t)floor(bjdFromMs(ms, baseOf(s))); BjdYear Y = bjdYear(N);
      char nb[24], yb[24]; fmtInt(N, nb, 24); fmtInt(Y.year, yb, 24);
      snprintf(buf, n, "Day %s \xC2\xB7 Year %s, day %lld", nb, yb, (long long)Y.doy);
    }
  }
}

#define EX(k, ...) do { snprintf(o.exK[o.nEx], 32, "%s", k); snprintf(o.exV[o.nEx], 56, __VA_ARGS__); o.nEx++; } while (0)

void nowInfo(SysId s, double ms, NowInfo& o, bool hasGeo, double lon) {
  memset(&o, 0, sizeof o);
  char a[64], b[64];
  if (s == GREG) {
    DT d = localParts(ms);
    snprintf(o.time, sizeof o.time, "%02d:%02d:%02d", d.h, d.mi, d.s);
    snprintf(o.timeNote, sizeof o.timeNote, "%s", tzName);
    int doy = (int)(daysFromCivil(d.y, d.mo + 1, d.d) - daysFromCivil(d.y, 1, 1)) + 1;
    snprintf(o.day.v, 24, "%d", d.d); snprintf(o.day.sub, 48, "%s", WEEKDAYS[d.wd]);
    snprintf(o.month.v, 24, "%s", MONTHS[d.mo]); snprintf(o.month.sub, 48, "month %d", d.mo + 1);
    snprintf(o.year.v, 24, "%lld", (long long)d.y); snprintf(o.year.sub, 48, "day %d \xC2\xB7 ISO week %d", doy, isoWeek(d.y, d.mo, d.d));
    isoUTC(ms, a);
    EX("UTC", "%s", a + 11);
    a[10] = 0; EX("UTC date", "%s", a);
  } else if (s == MARS) {
    double msd = msdFromMs(ms), ls = lsOf(msd);
    MarsDate d; bool ok = marsDate((int64_t)floor(msd), d);
    hms(msd, o.time);
    snprintf(o.timeNote, sizeof o.timeNote, "Coordinated Mars Time (Airy-0)");
    if (ok) {
      snprintf(o.day.v, 24, "%d", d.sol); snprintf(o.day.sub, 48, "weekday %d \xC2\xB7 sol %d of year", d.weekday + 1, d.solOfYear);
      snprintf(o.month.v, 24, "M%d", d.month); snprintf(o.month.sub, 48, "Ls %d\xC2\xB0\xE2\x80\x93%d\xC2\xB0", 30 * (d.month - 1), 30 * d.month);
      snprintf(o.year.v, 24, "MY %d", d.my); snprintf(o.year.sub, 48, "%d sols", marsYearLen(d.my));
    } else { strcpy(o.day.v, "\xE2\x80\x94"); strcpy(o.month.v, "\xE2\x80\x94"); strcpy(o.year.v, "\xE2\x80\x94"); }
    fmtFix(msd, 4, a, sizeof a);
    EX("Mars Sol Date", "%s", a);
    EX("Solar longitude", "Ls %.2f\xC2\xB0", ls);
    EX("Season", "%s", SEASONS[(int)floor(ls / 90)]);
    if (hasGeo) { hms(msd + lon / 360, b); EX("Mars time at your longitude", "%s LMST", b); }
  } else if (s == LUNAR) {
    LST p = lst(ms); Phase ph = moonPhase(ms);
    hms(p.frac, b);
    snprintf(o.time, sizeof o.time, "\xE2\x88\x87 %s", b);
    snprintf(o.timeNote, sizeof o.timeNote, "Lunar hours, minutes, seconds");
    snprintf(o.day.v, 24, "%d", p.cycle); snprintf(o.day.sub, 48, "cycle of 30");
    snprintf(o.month.v, 24, "%d", p.day); snprintf(o.month.sub, 48, "lunar day of 12");
    snprintf(o.year.v, 24, "%lld", (long long)p.year); snprintf(o.year.sub, 48, "since Apollo 11");
    label(LUNAR, ms, a, sizeof a);
    EX("LST", "%s", a);
    EX("Phase", "%s, %d%% lit", ph.name, (int)lround(ph.illum * 100));
    fmtInt(ph.lunation, a, sizeof a); fmtInt(ph.brown, b, sizeof b);
    EX("Lunation", "%s (Brown %s)", a, b);
  } else {
    const Base& B = baseOf(s);
    double bj = bjdFromMs(ms, B); int64_t N = (int64_t)floor(bj);
    BjdYear Y = bjdYear(N);
    double start = msFromBjd(N, B);
    hms((ms - start) / 864e5, o.time);
    snprintf(o.timeNote, sizeof o.timeNote, "into BJD day \xC2\xB7 fraction %.5f", bj - N);
    fmtInt(N, a, sizeof a);
    snprintf(o.day.v, 24, "%03d", (int)imod(N, 1000)); snprintf(o.day.sub, 48, "%s \xC2\xB7 day %s", WEEKDAYS[imod(N + 1, 7)], a);
    snprintf(o.month.v, 24, "H%d", (int)(imod(N, 1000) / 100)); snprintf(o.month.sub, 48, "hectoday of 10");
    o.hasKilo = true;
    fmtInt((int64_t)floor(N / 1000.0), o.kilo.v, 24); snprintf(o.kilo.sub, 48, "kiloday");
    fmtInt(Y.year, o.year.v, 24); snprintf(o.year.sub, 48, "day %lld of %lld", (long long)Y.doy, (long long)Y.len);
    EX("BJD_TDB", "%.6f", bj);
    EX("Barycentric shift", "%.2f min", corrSec(jdFromMs(ms), B.ra, B.dec) / 60);
    EX("Toward", "%s", B.longName);
  }
}
