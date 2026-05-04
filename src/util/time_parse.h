#pragma once

#include <cstdint>
#include <string_view>

namespace rinha {

/**
 * Parses an ISO-8601 UTC timestamp into compact calendar features.
 *
 * Expected input is at least `YYYY-MM-DDTHH:MM:SS`; trailing timezone text is ignored by this
 * parser because the dataset uses UTC timestamps.
 *
 * @param text Timestamp text.
 * @param epoch_seconds Seconds since Unix epoch.
 * @param hour Hour in `[0, 23]`.
 * @param weekday_monday0 Weekday in `[0, 6]`, where Monday is zero.
 * @return `true` when the fixed-position fields were parsed.
 */
bool parse_iso_utc(std::string_view text, int64_t& epoch_seconds, int& hour, int& weekday_monday0);

}  // namespace rinha
