#include "vectorize/fraud_vector.h"

#include <algorithm>

#include "util/time_parse.h"

namespace rinha {
namespace {

/**
 * Normalizes a ratio and clamps it to the model feature range.
 *
 * A zero or negative denominator is treated as "maxed out" when the numerator is positive.
 */
float normalized_ratio(double numerator, double denominator, double ratio) {
  if (denominator <= 0.0) return numerator > 0.0 ? 1.0f : 0.0f;
  return clamp01(static_cast<float>((numerator / denominator) / ratio));
}

/**
 * Returns the static risk score assigned to a merchant category code.
 */
float mcc_risk(std::string_view mcc) {
  if (mcc == "5411") return 0.15f;
  if (mcc == "5812") return 0.30f;
  if (mcc == "5912") return 0.20f;
  if (mcc == "5944") return 0.45f;
  if (mcc == "7801") return 0.80f;
  if (mcc == "7802") return 0.75f;
  if (mcc == "7995") return 0.85f;
  if (mcc == "4511") return 0.35f;
  if (mcc == "5311") return 0.25f;
  if (mcc == "5999") return 0.50f;
  return 0.50f;
}

/**
 * Checks whether the current merchant appears in the bounded known-merchant list.
 */
bool is_known_merchant(const FraudRequest& req) {
  for (size_t i = 0; i < req.known_merchant_count; ++i) {
    if (req.known_merchants[i] == req.merchant_id) return true;
  }
  return false;
}

}  // namespace

bool vectorize_request(const FraudRequest& req, std::array<float, kPaddedDim>& out) {
  int64_t requested_epoch = 0;
  int hour = 0;
  int weekday = 0;
  if (!parse_iso_utc(req.requested_at, requested_epoch, hour, weekday)) return false;

  out.fill(0.0f);
  out[0] = clamp01(static_cast<float>(req.amount / 10000.0));
  out[1] = clamp01(static_cast<float>(req.installments) / 12.0f);
  out[2] = normalized_ratio(req.amount, req.customer_avg_amount, 10.0);
  out[3] = static_cast<float>(hour) / 23.0f;
  out[4] = static_cast<float>(weekday) / 6.0f;

  if (req.has_last_transaction) {
    int64_t last_epoch = 0;
    int last_hour = 0;
    int last_weekday = 0;
    if (!parse_iso_utc(req.last_timestamp, last_epoch, last_hour, last_weekday)) return false;
    int64_t delta_seconds = requested_epoch - last_epoch;
    if (delta_seconds < 0) delta_seconds = 0;
    out[5] = clamp01(static_cast<float>(delta_seconds) / 60.0f / 1440.0f);
    out[6] = clamp01(static_cast<float>(req.km_from_current / 1000.0));
  } else {
    out[5] = -1.0f;
    out[6] = -1.0f;
  }

  out[7] = clamp01(static_cast<float>(req.km_from_home / 1000.0));
  out[8] = clamp01(static_cast<float>(req.tx_count_24h) / 20.0f);
  out[9] = req.is_online ? 1.0f : 0.0f;
  out[10] = req.card_present ? 1.0f : 0.0f;
  out[11] = is_known_merchant(req) ? 0.0f : 1.0f;
  out[12] = mcc_risk(req.merchant_mcc);
  out[13] = clamp01(static_cast<float>(req.merchant_avg_amount / 10000.0));
  out[14] = 0.0f;
  out[15] = 0.0f;
  return true;
}

}  // namespace rinha
