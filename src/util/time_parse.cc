#include "util/time_parse.h"

#include <cstdint>

namespace rinha {
namespace {

/** Converts an ASCII digit to its numeric value, or `-1` for non-digits. */
int digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : -1;
}

/** Parses two fixed-position digits. */
bool parse2(std::string_view s, size_t pos, int& out) {
  if (pos + 1 >= s.size()) return false;
  int a = digit(s[pos]);
  int b = digit(s[pos + 1]);
  if (a < 0 || b < 0) return false;
  out = a * 10 + b;
  return true;
}

/** Parses four fixed-position digits. */
bool parse4(std::string_view s, size_t pos, int& out) {
  if (pos + 3 >= s.size()) return false;
  int a = digit(s[pos]);
  int b = digit(s[pos + 1]);
  int c = digit(s[pos + 2]);
  int d = digit(s[pos + 3]);
  if (a < 0 || b < 0 || c < 0 || d < 0) return false;
  out = a * 1000 + b * 100 + c * 10 + d;
  return true;
}

/**
 * Converts a civil date to days since the Unix epoch.
 *
 * Algorithm adapted from Howard Hinnant's civil calendar routines.
 */
int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

}  // namespace

bool parse_iso_utc(std::string_view text, int64_t& epoch_seconds, int& hour, int& weekday_monday0) {
  if (text.size() < 20) return false;
  int year, month, day, minute, second;
  if (!parse4(text, 0, year) || !parse2(text, 5, month) || !parse2(text, 8, day) ||
      !parse2(text, 11, hour) || !parse2(text, 14, minute) || !parse2(text, 17, second)) {
    return false;
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return false;
  }
  const int64_t days =
      days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  epoch_seconds =
      days * 86400 + static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 + second;
  int weekday = static_cast<int>((days + 3) % 7);
  if (weekday < 0) weekday += 7;
  weekday_monday0 = weekday;
  return true;
}

}  // namespace rinha
