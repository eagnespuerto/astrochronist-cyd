// Calendar adapters, ported from js/systems.js. Each system gives the Calendar page a year
// layout (a year of "month" cards full of day cells) and the Now page its day / month / year parts.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

enum SysId : uint8_t { GREG, MARS, LUNAR, SGRA, ACEN, NSYS };

struct Part { char v[24]; char sub[48]; };
struct NowInfo {
  char time[24]; char timeNote[48];
  Part day, month, kilo, year; bool hasKilo;
  int nEx; char exK[4][32]; char exV[4][56];
};

struct MonthCard {
  char title[16]; char sub[24]; char right[16]; char meta[64];
  uint8_t cols, lead, n; bool accent; uint8_t headers;   // headers: 0 none, 1 weekday, 2 "+0..+9"
};
struct BandSeg { char label[6]; char value[6]; float weight; bool accent; };
struct YearLayout {
  char title[32]; char subtitle[320];
  int nMonths; MonthCard months[12]; BandSeg band[12];
  char bandNote[128]; char bandAccent[48]; const char* foot;
};
// Every cell of one year. Cells are contiguous: cell i runs start[i] .. start[i+1]. cls 1 event, 2 leap sol.
struct YearCells {
  int n; double start[1001]; uint8_t cls[1000];
  int nNotes; int noteIdx[12]; char note[12][96]; int monthFirst[13];
};

struct System {
  SysId id; const char* name; const char* pill; const char* menu;
  const char* uDay; const char* uMonth; const char* uKilo; const char* uYear;
};
extern const System SYS[NSYS];

int64_t yearOf(SysId s, double ms);
int64_t clampYear(SysId s, int64_t y);
void layout(SysId s, int64_t y, YearLayout& L);
void yearCells(SysId s, int64_t y, const YearLayout& L, YearCells& C);
void cellLabel(SysId s, const YearCells& C, int month, int i, char* label, char* sub);
void label(SysId s, double ms, char* buf, size_t n);   // full instant label
void dayLabel(SysId s, double ms, char* buf, size_t n);
void nowInfo(SysId s, double ms, NowInfo& o, bool hasGeo, double lon);

// Formatting helpers shared with the UI
void fmtInt(int64_t v, char* buf, size_t n);               // 6,739
void fmtFix(double v, int p, char* buf, size_t n);         // 54,321.1234
void hms(double frac, char* buf);                          // hh:mm:ss (needs 9 bytes)
void isoUTC(double ms, char* buf);                         // 2026-11-03 14:05:09
void utcDate(double ms, char* buf, size_t n);              // 3 Nov 2026
void utcShort(double ms, char* buf, size_t n);             // 3 Nov
void utcFull(double ms, char* buf, size_t n);              // Tue 3 Nov 2026, 14:05
void localFull(double ms, char* buf, size_t n);            // Tue 3 Nov 2026, 14:05 UTC+08:00
void localHM(double ms, char* buf, size_t n);              // Tue 3 Nov 2026, 14:05
void localDate(double ms, char* buf, size_t n);            // Tue 3 Nov 2026
void localDayHMS(double ms, char* buf, size_t n);          // Tue 3 Nov, 14:05:09 UTC+08:00
void localHMonly(double ms, char* buf, size_t n);          // 14:05
extern const char* const MONTHS[12];
extern const char* const WEEKDAYS[7];
