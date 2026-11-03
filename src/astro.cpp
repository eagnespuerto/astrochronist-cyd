// Implementations for astro.h, line-for-line with the website's js/time/*.js.
#include "astro.h"

namespace astro {

int tzOffsetMin = 480;            // overwritten from NVS / USB sync
char tzName[12] = "UTC+08:00";

/* ---------------- Mars ---------------- */
const char* const SEASONS[4] = { "Northern spring", "Northern summer", "Northern autumn", "Northern winter" };
const int MONTH_LEN[12] = { 61, 65, 67, 64, 60, 54, 50, 47, 46, 47, 51, 56 };
const char* const MARS_WEEK[7] = { "S", "M", "T", "W", "T", "F", "S" };

static double MSD1955;
static int64_t K0;
int MY_MAX = 1;
static const int NY = 300;               // table slots for MY_MIN..MY_MAX+1
static double astroT[NY];
static int64_t wbT[NY];
double astroStart(int n) { return astroT[n - MY_MIN]; }
int64_t wb(int n) { return wbT[n - MY_MIN]; }

double solveL(double target, double guess) {
  double m = guess;
  for (int k = 0; k < 40; k++) {
    double f = lcont(m) - target;
    if (fabs(f) < 1e-9) break;
    m -= f / ((lcont(m + .01) - lcont(m - .01)) / .02);
  }
  return m;
}

double boundary(int n, int i) {
  return solveL(360.0 * (K0 + n - 1) + 30 * i, MSD1955 + (n - 1) * 668.5921 + i * 55.7);
}

void initMars() {
  MSD1955 = msdFromMs(utcMs(1955, 3, 11));
  K0 = (int64_t)llround(lcont(MSD1955) / 360);
  double LIMIT = msdFromMs(utcMs(2201, 0, 1));
  for (int n = MY_MIN; n - MY_MIN < NY - 1; n++) {
    astroT[n - MY_MIN] = boundary(n, 0);
    if (astroT[n - MY_MIN] >= LIMIT) { MY_MAX = n - 1; break; }
  }
  wbT[1 - MY_MIN] = (int64_t)floor(astroStart(1));
  for (int n = 2; n <= MY_MAX + 1; n++) wbT[n - MY_MIN] = wb(n - 1) + marsYearLen(n - 1);
  for (int n = 0; n >= MY_MIN; n--) wbT[n - MY_MIN] = wb(n + 1) - marsYearLen(n);
}

bool yearOfSol(int64_t m, int& out) {
  if (m < wb(MY_MIN) || m >= wb(MY_MAX + 1)) return false;
  int n = 1 + (int)floor((m - wb(1)) / 668.6);
  if (n < MY_MIN) n = MY_MIN;
  if (n > MY_MAX) n = MY_MAX;
  while (m < wb(n)) n--;
  while (m >= wb(n + 1)) n++;
  out = n;
  return true;
}

void monthLengths(int n, int out[12]) {
  for (int i = 0; i < 12; i++) out[i] = MONTH_LEN[i];
  if (marsLeap(n)) out[11]++;
}

bool marsDate(int64_t m, MarsDate& o) {
  int my;
  if (!yearOfSol(m, my)) return false;
  int64_t r = m - wb(my);
  int lens[12];
  monthLengths(my, lens);
  int month = 0;
  while (r >= lens[month]) { r -= lens[month]; month++; }
  o = { my, month + 1, (int)r + 1, (int)(m - wb(my)) + 1, (int)imod(m - wb(1), 7) };
  return true;
}

/* ---------------- Lunar ---------------- */
const double LST_EPOCH = utcMs(1969, 6, 21, 2, 56, 15);

Phase moonPhase(double ms) {
  static const char* const names[8] = { "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                                        "Full moon", "Waning gibbous", "Last quarter", "Waning crescent" };
  double x = (jdFromMs(ms) - 2451550.09766) / SYNODIC;
  int64_t lunation = (int64_t)floor(x);
  double f = x - lunation;
  return { lunation, lunation + 953, f * SYNODIC, (1 - cos(2 * M_PI * f)) / 2,
           names[(int)floor(fmodp(f + 1.0 / 16, 1) * 8)] };
}

/* ---------------- Barycentric ---------------- */
static const double AU_C = 499.004784;
const Base BASE_SGRA = { "sgra", 266.416837, -29.007811, "Sgr A*", "Sagittarius A* (galactic centre)", "RA 17:45:40.0, Dec \xE2\x88\x92" "29:00:28" };
const Base BASE_ACEN = { "acen", 219.902063, -60.833972, "\xCE\xB1 Cen", "\xCE\xB1 Centauri A (nearest star system)", "RA 14:39:36.5, Dec \xE2\x88\x92" "60:50:02" };

static void sunOffset(double T, double& x, double& y) {
  x = 0; y = 0;
  const double terms[2][3] = { { 34.39644 + 3034.74612775 * T, 5.2026, 1 / 1047.35 },
                               { 49.95424 + 1222.49362201 * T, 9.5367, 1 / 3497.9 } };
  for (auto& t : terms) { x -= t[2] * t[1] * cos(t[0] * D2R); y -= t[2] * t[1] * sin(t[0] * D2R); }
}

static double project(double x, double y, double z, double ra, double dec) {
  double a = ra * D2R, d = dec * D2R;
  return AU_C * (x * cos(d) * cos(a) + y * cos(d) * sin(a) + z * sin(d));
}

double corrSec(double jdUTC, double ra, double dec) {
  double jdTT = jdUTC + TT_DAYS, n = jdTT - 2451545.0, T = n / 36525;
  double eps = (23.439291 - 0.0130042 * T) * D2R;
  double L = (280.460 + 0.9856474 * n) * D2R, g = (357.528 + 0.9856003 * n) * D2R;
  double lam = L + (1.915 * sin(g) + 0.020 * sin(2 * g)) * D2R;
  double R = 1.00014 - 0.01671 * cos(g) - 0.00014 * cos(2 * g);
  double sx, sy; sunOffset(T, sx, sy);
  double x = -R * cos(lam) + sx, y = -R * sin(lam) + sy;
  return project(x, y * cos(eps), y * sin(eps), ra, dec);
}

double corrSecMars(double jdUTC, double ra, double dec) {
  double jdTT = jdUTC + TT_DAYS;
  MarsOrbit o = marsOrbit(jdTT);
  double Ls = o.Ls, nu = o.nu, d = o.d, T = d / 36525;
  double l = (Ls + 85.061 - 0.015 * sin((71 + 2 * Ls) * D2R) - 5.5e-6 * d) * D2R;
  double b = 1.8497 * sin((Ls - 144.5) * D2R) * D2R;
  double r = 1.523679 * (1 - 0.0934 * 0.0934) / (1 + 0.0934 * cos(nu));
  double sx, sy; sunOffset(T, sx, sy);
  double x = r * cos(b) * cos(l) + sx, y = r * cos(b) * sin(l) + sy, z = r * sin(b);
  double eps = 23.439291 * D2R;
  return project(x, y * cos(eps) - z * sin(eps), y * sin(eps) + z * cos(eps), ra, dec);
}

double utcFromBjd(double b, double ra, double dec) {
  double jd = b - TT_DAYS;
  for (int i = 0; i < 3; i++) jd = b - (TT_UTC + corrSec(jd, ra, dec)) / 86400;
  return jd;
}

double amplitude(const Base& t, bool onMars, double nowMs) {
  static double cache[2][2] = { { -1, -1 }, { -1, -1 } };
  int bi = (&t == &BASE_ACEN) ? 1 : 0;
  if (cache[bi][onMars] >= 0) return cache[bi][onMars];
  double j = jdFromMs(nowMs), m = 0;
  int span = onMars ? 687 : 366;
  for (int i = 0; i < span; i += 3) {
    double v = fabs(onMars ? corrSecMars(j + i, t.ra, t.dec) : corrSec(j + i, t.ra, t.dec));
    if (v > m) m = v;
  }
  return cache[bi][onMars] = m / 60;
}

/* ---------------- Sun ---------------- */
SunTimes sunTimes(double ms, double lat, double lon) {
  double n = round(jdFromMs(ms) - 2451545.0008 + lon / 360);
  double J = n - lon / 360;
  double M = fmodp(357.5291 + 0.98560028 * J, 360) * D2R;
  double C = 1.9148 * sin(M) + 0.02 * sin(2 * M) + 0.0003 * sin(3 * M);
  double lam = fmodp(M / D2R + C + 180 + 102.9372, 360) * D2R;
  double transit = 2451545 + J + 0.0053 * sin(M) - 0.0069 * sin(2 * lam);
  double sinD = sin(lam) * sin(23.4397 * D2R), cosD = cos(asin(sinD));
  double cosW = (sin(-0.833 * D2R) - sin(lat * D2R) * sinD) / (cos(lat * D2R) * cosD);
  SunTimes o = { msFromJd(transit), NAN, NAN, ((2451545 + J) - transit) * 1440, nullptr };
  if (cosW > 1) o.polar = "polar night";
  else if (cosW < -1) o.polar = "midnight sun";
  else {
    double w = acos(cosW) / D2R / 360;
    o.rise = msFromJd(transit - w);
    o.set = msFromJd(transit + w);
  }
  return o;
}

}  // namespace astro
