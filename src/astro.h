// Time math, ported from the website's js/time/*.js (core, mars, lunar, bary, solar).
// All instants are Unix milliseconds (UTC) held in doubles, exactly as in the JS.
#pragma once
#include <math.h>
#include <stdint.h>

namespace astro {

/* ---------------- core ---------------- */
constexpr double DAY_MS = 86400000.0;
constexpr double JD_UNIX = 2440587.5;
constexpr double TT_UTC = 69.184;
constexpr double TT_DAYS = TT_UTC / 86400.0;
constexpr double D2R = M_PI / 180.0;

inline double jdFromMs(double ms) { return ms / DAY_MS + JD_UNIX; }
inline double msFromJd(double jd) { return (jd - JD_UNIX) * DAY_MS; }
inline double fmodp(double a, double b) { double r = fmod(a, b); return r < 0 ? r + b : r; }
inline int64_t imod(int64_t a, int64_t b) { int64_t r = a % b; return r < 0 ? r + b : r; }

// Proleptic Gregorian <-> day count (Howard Hinnant's algorithms), safe for any year.
inline int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
struct Civil { int64_t y; int m, d; };
inline Civil civilFromDays(int64_t z) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  int d = doy - (153 * mp + 2) / 5 + 1;
  int m = mp < 10 ? mp + 3 : mp - 9;
  return { y + (m <= 2), m, d };
}

/** Like Date.UTC / setUTCFullYear: month 0-based, overflow in month or day normalises. */
inline double utcMs(int64_t y, int m, int d, int h = 0, int mi = 0, double s = 0) {
  y += (int64_t)floor(m / 12.0); m = (int)imod(m, 12);
  double days = (double)daysFromCivil(y, m + 1, 1) + d - 1;
  return days * DAY_MS + h * 3600000.0 + mi * 60000.0 + s * 1000.0;
}

// The board has no time-zone database, so local time is a fixed UTC offset (set over USB).
extern int tzOffsetMin;
extern char tzName[12];
inline double localMs(int64_t y, int m, int d, int h = 0, int mi = 0, double s = 0) {
  return utcMs(y, m, d, h, mi, s) - tzOffsetMin * 60000.0;
}

struct DT { int64_t y; int mo, d, h, mi, s, wd; double ms; };  // mo 0-based, wd 0 = Sunday
inline DT breakdown(double ms, int offMin) {
  double t = ms + offMin * 60000.0;
  int64_t days = (int64_t)floor(t / DAY_MS);
  int64_t sec = (int64_t)floor((t - days * DAY_MS) / 1000.0);
  Civil c = civilFromDays(days);
  return { c.y, c.m - 1, c.d, (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60), (int)imod(days + 4, 7), t };
}
inline DT utcParts(double ms) { return breakdown(ms, 0); }
inline DT localParts(double ms) { return breakdown(ms, tzOffsetMin); }
inline bool isLeapG(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* ---------------- Mars (Allison & McEwen 2000 / Mars24) ---------------- */
constexpr double SOLD = 1.0274912517;
constexpr double MARS_JD0 = 2405522.0028779;

struct OrbitD { double lcont, nu, d; };
inline OrbitD orbitD(double d) {
  static const double PA[] = { .0071, .0057, .0039, .0037, .0021, .0020, .0018 };
  static const double PT[] = { 2.2353, 2.7543, 1.1177, 15.7866, 2.1354, 2.4694, 32.8493 };
  static const double PP[] = { 49.409, 168.173, 191.837, 21.736, 15.704, 95.528, 49.095 };
  double M = (19.3871 + 0.52402073 * d) * D2R;
  double aFMS = 270.3871 + 0.524038496 * d;
  double pbs = 0;
  for (int i = 0; i < 7; i++) pbs += PA[i] * cos((0.985626 * d / PT[i] + PP[i]) * D2R);
  double eoc = (10.691 + 3e-7 * d) * sin(M) + 0.623 * sin(2 * M) + 0.050 * sin(3 * M)
             + 0.005 * sin(4 * M) + 0.0005 * sin(5 * M) + pbs;
  return { aFMS + eoc, M + eoc * D2R, d };
}
struct MarsOrbit { double Ls, nu, d; };
inline MarsOrbit marsOrbit(double jdTT) { OrbitD o = orbitD(jdTT - 2451545.0); return { fmodp(o.lcont, 360), o.nu, o.d }; }

inline double msdFromMs(double ms) { return (ms / DAY_MS + JD_UNIX + TT_DAYS - MARS_JD0) / SOLD; }
inline double msFromMsd(double m) { return ((m * SOLD + MARS_JD0 - TT_DAYS) - JD_UNIX) * DAY_MS; }
inline double lcont(double msd) { return orbitD(msd * SOLD + MARS_JD0 - 2451545.0).lcont; }
inline double lsOf(double msd) { return fmodp(lcont(msd), 360); }

extern const char* const SEASONS[4];
extern const int MONTH_LEN[12];
extern const char* const MARS_WEEK[7];

double solveL(double target, double guess);
double boundary(int n, int i);
inline bool marsLeap(int n) { return imod(n, 2) == 1 || imod(n, 10) == 0; }
inline int marsYearLen(int n) { return marsLeap(n) ? 669 : 668; }

constexpr int MY_MIN = -60;
extern int MY_MAX;
double astroStart(int n);   // exact MSD of Ls 0 in year n
int64_t wb(int n);          // whole-sol MSD of sol 1 of year n
void initMars();
bool yearOfSol(int64_t m, int& out);
void monthLengths(int n, int out[12]);
struct MarsDate { int my, month, sol, solOfYear, weekday; };
bool marsDate(int64_t m, MarsDate& out);

/* ---------------- Lunar Standard Time (Kenneth L. Lee, 1970) ---------------- */
constexpr double SYNODIC = 29.530588853;
extern const double LST_EPOCH;
constexpr double LUNAR_DAY_MS = SYNODIC * DAY_MS;
constexpr double CYCLE_MS = LUNAR_DAY_MS / 30;
constexpr double LUNAR_SECOND = SYNODIC * 86400 / (30 * 86400);
struct LST { int64_t year; int day, cycle; double frac, t; };
inline LST lst(double ms) {
  double t = (ms - LST_EPOCH) / LUNAR_DAY_MS;
  int64_t di = (int64_t)floor(t);
  int64_t year = (int64_t)floor(di / 12.0) + 1;
  int day = (int)imod(di, 12) + 1;
  double c = (t - di) * 30;
  return { year, day, (int)floor(c) + 1, c - floor(c), t };
}
inline double msFromLst(int64_t year, int day, int cycle, double frac = 0) {
  return LST_EPOCH + ((year - 1) * 12 + (day - 1)) * LUNAR_DAY_MS + (cycle - 1 + frac) * CYCLE_MS;
}
struct Phase { int64_t lunation, brown; double age, illum; const char* name; };
Phase moonPhase(double ms);
inline double meanPhaseMs(double k) { return (2451550.09766 + k * SYNODIC - 2440587.5) * DAY_MS; }

/* ---------------- Barycentric Julian Date (about ±2 s) ---------------- */
struct Base { const char* id; double ra, dec; const char* name; const char* longName; const char* coords; };
extern const Base BASE_SGRA, BASE_ACEN;
double corrSec(double jdUTC, double ra, double dec);
double corrSecMars(double jdUTC, double ra, double dec);
inline double bjd(double jdUTC, double ra, double dec) { return jdUTC + (TT_UTC + corrSec(jdUTC, ra, dec)) / 86400; }
double utcFromBjd(double b, double ra, double dec);
inline double bjdFromMs(double ms, const Base& t) { return bjd(jdFromMs(ms), t.ra, t.dec); }
inline double msFromBjd(double b, const Base& t) { return msFromJd(utcFromBjd(b, t.ra, t.dec)); }
constexpr double BJD_YEAR = 365.25;
inline double bjdYearStart(int64_t y) { return (y - 1) * BJD_YEAR; }
struct BjdYear { int64_t year, doy, len, first; };
inline BjdYear bjdYear(int64_t N) {
  int64_t year = (int64_t)floor(N / BJD_YEAR) + 1, first = (int64_t)ceil(bjdYearStart(year));
  return { year, N - first + 1, (int64_t)ceil(bjdYearStart(year + 1)) - first, first };
}
double amplitude(const Base& t, bool onMars, double nowMs);

/* ---------------- Sun times ---------------- */
struct SunTimes { double noon, rise, set, eotMin; const char* polar; };
SunTimes sunTimes(double ms, double lat, double lon);
inline double meanSolarFrac(double ms, double lon) { return fmodp(jdFromMs(ms) + 0.5 + lon / 360, 1); }

// Current time in Unix ms (defined in main.cpp).
double nowMs();

}  // namespace astro
