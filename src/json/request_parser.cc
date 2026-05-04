#include <cstdlib>

#include "vectorize/fraud_vector.h"

namespace rinha {
namespace {

/** Returns whether a byte is JSON whitespace for the subset parser. */
bool is_ws(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

/** Advances over whitespace and returns the first non-whitespace position. */
size_t skip_ws(std::string_view s, size_t pos) {
  while (pos < s.size() && is_ws(s[pos])) ++pos;
  return pos;
}

/**
 * Skips a JSON string starting at `pos`.
 *
 * Escaped bytes are skipped as pairs; the parser does not unescape because callers only need spans.
 */
size_t skip_string(std::string_view s, size_t pos) {
  ++pos;
  while (pos < s.size()) {
    if (s[pos] == '\\') {
      pos += 2;
      continue;
    }
    if (s[pos] == '"') return pos + 1;
    ++pos;
  }
  return std::string_view::npos;
}

/** Compares the raw quoted string span with an object key. */
bool string_equals(std::string_view s, size_t begin, size_t end, std::string_view key) {
  if (end <= begin + 1) return false;
  return s.substr(begin + 1, end - begin - 2) == key;
}

/**
 * Finds the value position for a key inside a JSON object.
 *
 * This is intentionally order-insensitive and only implements the subset required by the payload.
 */
size_t find_value(std::string_view object, std::string_view key) {
  for (size_t pos = 0; pos < object.size();) {
    if (object[pos] != '"') {
      ++pos;
      continue;
    }
    const size_t end = skip_string(object, pos);
    if (end == std::string_view::npos) return std::string_view::npos;
    if (string_equals(object, pos, end, key)) {
      size_t colon = skip_ws(object, end);
      if (colon < object.size() && object[colon] == ':') return skip_ws(object, colon + 1);
    }
    pos = end;
  }
  return std::string_view::npos;
}

/** Captures a balanced object or array span while ignoring delimiters inside strings. */
bool span_balanced(std::string_view s, size_t pos, char open, char close, std::string_view& out) {
  if (pos >= s.size() || s[pos] != open) return false;
  int depth = 0;
  for (size_t i = pos; i < s.size();) {
    if (s[i] == '"') {
      i = skip_string(s, i);
      if (i == std::string_view::npos) return false;
      continue;
    }
    if (s[i] == open) ++depth;
    if (s[i] == close && --depth == 0) {
      out = s.substr(pos, i - pos + 1);
      return true;
    }
    ++i;
  }
  return false;
}

/** Reads an object-valued field as a borrowed span. */
bool get_object(std::string_view object, std::string_view key, std::string_view& out) {
  const size_t pos = find_value(object, key);
  return pos != std::string_view::npos && span_balanced(object, pos, '{', '}', out);
}

/** Reads an array-valued field as a borrowed span. */
bool get_array(std::string_view object, std::string_view key, std::string_view& out) {
  const size_t pos = find_value(object, key);
  return pos != std::string_view::npos && span_balanced(object, pos, '[', ']', out);
}

/** Checks whether a field value is the JSON literal `null`. */
bool is_null_value(std::string_view object, std::string_view key) {
  const size_t pos = find_value(object, key);
  return pos != std::string_view::npos && object.substr(pos, 4) == "null";
}

/** Reads a string-valued field as a borrowed, still-escaped span. */
bool get_string(std::string_view object, std::string_view key, std::string_view& out) {
  const size_t pos = find_value(object, key);
  if (pos == std::string_view::npos || pos >= object.size() || object[pos] != '"') return false;
  const size_t end = skip_string(object, pos);
  if (end == std::string_view::npos) return false;
  out = object.substr(pos + 1, end - pos - 2);
  return true;
}

/** Reads a numeric field using the C locale parser. */
bool get_number(std::string_view object, std::string_view key, double& out) {
  const size_t pos = find_value(object, key);
  if (pos == std::string_view::npos) return false;
  const char* begin = object.data() + pos;
  char* end = nullptr;
  out = std::strtod(begin, &end);
  return end != begin;
}

/** Reads a numeric field and casts it to an integer. */
bool get_int(std::string_view object, std::string_view key, int& out) {
  double value = 0.0;
  if (!get_number(object, key, value)) return false;
  out = static_cast<int>(value);
  return true;
}

/** Reads a boolean field from the JSON literals `true` or `false`. */
bool get_bool(std::string_view object, std::string_view key, bool& out) {
  const size_t pos = find_value(object, key);
  if (pos == std::string_view::npos) return false;
  if (object.substr(pos, 4) == "true") {
    out = true;
    return true;
  }
  if (object.substr(pos, 5) == "false") {
    out = false;
    return true;
  }
  return false;
}

/** Parses the customer known merchant array into the bounded request storage. */
void parse_known_merchants(std::string_view array, FraudRequest& out) {
  for (size_t pos = 0;
       pos < array.size() && out.known_merchant_count < out.known_merchants.size();) {
    if (array[pos] != '"') {
      ++pos;
      continue;
    }
    const size_t end = skip_string(array, pos);
    if (end == std::string_view::npos) return;
    out.known_merchants[out.known_merchant_count++] = array.substr(pos + 1, end - pos - 2);
    pos = end;
  }
}

}  // namespace

bool parse_fraud_request(std::string_view json, FraudRequest& out) {
  std::string_view transaction, customer, merchant, terminal, known;
  if (!get_object(json, "transaction", transaction) || !get_object(json, "customer", customer) ||
      !get_object(json, "merchant", merchant) || !get_object(json, "terminal", terminal)) {
    return false;
  }

  out = FraudRequest{};
  if (!get_number(transaction, "amount", out.amount) ||
      !get_int(transaction, "installments", out.installments) ||
      !get_string(transaction, "requested_at", out.requested_at) ||
      !get_number(customer, "avg_amount", out.customer_avg_amount) ||
      !get_int(customer, "tx_count_24h", out.tx_count_24h) ||
      !get_string(merchant, "id", out.merchant_id) ||
      !get_string(merchant, "mcc", out.merchant_mcc) ||
      !get_number(merchant, "avg_amount", out.merchant_avg_amount) ||
      !get_bool(terminal, "is_online", out.is_online) ||
      !get_bool(terminal, "card_present", out.card_present) ||
      !get_number(terminal, "km_from_home", out.km_from_home)) {
    return false;
  }

  if (get_array(customer, "known_merchants", known)) parse_known_merchants(known, out);

  if (is_null_value(json, "last_transaction")) {
    out.has_last_transaction = false;
    return true;
  }

  std::string_view last;
  if (!get_object(json, "last_transaction", last)) return false;
  out.has_last_transaction = true;
  return get_string(last, "timestamp", out.last_timestamp) &&
         get_number(last, "km_from_current", out.km_from_current);
}

}  // namespace rinha
