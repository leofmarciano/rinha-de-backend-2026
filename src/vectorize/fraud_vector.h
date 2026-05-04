#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "ann/index.h"

namespace rinha {

/**
 * Parsed fraud-scoring request.
 *
 * The parser stores most string fields as `std::string_view` slices into the original JSON body.
 * Callers must keep the JSON buffer alive until the request has been vectorized.
 */
struct FraudRequest {
  /// Transaction amount in the input currency unit.
  double amount = 0.0;

  /// Number of installments requested for the transaction.
  int installments = 0;

  /// ISO-8601 UTC timestamp for the requested transaction.
  std::string_view requested_at;

  /// Customer historical average transaction amount.
  double customer_avg_amount = 0.0;

  /// Customer transaction count over the last 24 hours.
  int tx_count_24h = 0;

  /// Bounded list of merchant ids already known by this customer.
  std::array<std::string_view, 32> known_merchants{};

  /// Number of valid entries currently stored in `known_merchants`.
  size_t known_merchant_count = 0;

  /// Merchant identifier for the current transaction.
  std::string_view merchant_id;

  /// Merchant category code used by the MCC risk lookup.
  std::string_view merchant_mcc;

  /// Merchant historical average transaction amount.
  double merchant_avg_amount = 0.0;

  /// Whether the transaction was initiated online.
  bool is_online = false;

  /// Whether the card was physically present.
  bool card_present = false;

  /// Distance from the customer home location, in kilometers.
  double km_from_home = 0.0;

  /// Whether `last_timestamp` and `km_from_current` are available.
  bool has_last_transaction = false;

  /// ISO-8601 UTC timestamp for the previous transaction when present.
  std::string_view last_timestamp;

  /// Distance from the previous transaction location, in kilometers.
  double km_from_current = 0.0;
};

/**
 * Parses a fraud request JSON payload into a lightweight `FraudRequest`.
 *
 * @param json Request body. It must outlive `out`.
 * @param out Destination request struct, reset on successful top-level parsing.
 * @return `true` when all required fields were found and parsed.
 */
bool parse_fraud_request(std::string_view json, FraudRequest& out);

bool parse_and_vectorize_request(std::string_view json, std::array<float, kPaddedDim>& out);

/**
 * Converts a parsed request into the normalized ANN query vector.
 *
 * The output vector has `kPaddedDim` dimensions. The first `kLogicalDim` carry model features and
 * the remaining dimensions are padding for alignment/vectorization.
 *
 * @param req Parsed request.
 * @param out Destination normalized vector.
 * @return `false` if a timestamp or required derived feature cannot be parsed.
 */
bool vectorize_request(const FraudRequest& req, std::array<float, kPaddedDim>& out);

}  // namespace rinha
