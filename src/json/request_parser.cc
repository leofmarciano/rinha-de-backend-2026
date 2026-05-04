#include <cstdlib>

#include "util/time_parse.h"
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

bool parse_decimal(std::string_view s, size_t pos, double& out) {
  if (pos >= s.size()) return false;
  const char* p = s.data() + pos;
  const char* end = s.data() + s.size();
  bool neg = false;
  if (*p == '-') {
    neg = true;
    if (++p == end) return false;
  }

  bool any = false;
  double value = 0.0;
  while (p < end && *p >= '0' && *p <= '9') {
    any = true;
    value = value * 10.0 + static_cast<double>(*p - '0');
    ++p;
  }
  if (p < end && *p == '.') {
    ++p;
    double scale = 0.1;
    while (p < end && *p >= '0' && *p <= '9') {
      any = true;
      value += static_cast<double>(*p - '0') * scale;
      scale *= 0.1;
      ++p;
    }
  }
  if (!any) return false;
  if (p < end && (*p == 'e' || *p == 'E')) {
    char* parsed_end = nullptr;
    out = std::strtod(s.data() + pos, &parsed_end);
    return parsed_end != s.data() + pos;
  }
  out = neg ? -value : value;
  return true;
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
  return parse_decimal(object, pos, out);
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

bool value_after(std::string_view s, std::string_view key, size_t from, size_t& pos) {
  pos = s.find(key, from);
  if (pos == std::string_view::npos) return false;
  pos = s.find(':', pos + key.size());
  if (pos == std::string_view::npos) return false;
  pos = skip_ws(s, pos + 1);
  return pos < s.size();
}

bool number_after(std::string_view s, std::string_view key, size_t from, double& out) {
  size_t pos = 0;
  if (!value_after(s, key, from, pos)) return false;
  return parse_decimal(s, pos, out);
}

bool int_after(std::string_view s, std::string_view key, size_t from, int& out) {
  double value = 0.0;
  if (!number_after(s, key, from, value)) return false;
  out = static_cast<int>(value);
  return true;
}

bool bool_after(std::string_view s, std::string_view key, size_t from, bool& out) {
  size_t pos = 0;
  if (!value_after(s, key, from, pos)) return false;
  if (s.substr(pos, 4) == "true") {
    out = true;
    return true;
  }
  if (s.substr(pos, 5) == "false") {
    out = false;
    return true;
  }
  return false;
}

bool string_after(std::string_view s, std::string_view key, size_t from, std::string_view& out) {
  size_t pos = 0;
  if (!value_after(s, key, from, pos) || s[pos] != '"') return false;
  const size_t end = skip_string(s, pos);
  if (end == std::string_view::npos) return false;
  out = s.substr(pos + 1, end - pos - 2);
  return true;
}

float normalized_ratio_fast(double numerator, double denominator, double ratio) {
  if (denominator <= 0.0) return numerator > 0.0 ? 1.0f : 0.0f;
  return clamp01(static_cast<float>((numerator / denominator) / ratio));
}

float mcc_risk_fast(std::string_view mcc) {
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

bool parse_and_vectorize_request(std::string_view json, std::array<float, kPaddedDim>& out) {
  const size_t tx = json.find("\"transaction\"");
  const size_t customer = json.find("\"customer\"");
  const size_t merchant = json.find("\"merchant\"");
  const size_t terminal = json.find("\"terminal\"");
  const size_t last = json.find("\"last_transaction\"");
  if (tx == std::string_view::npos || customer == std::string_view::npos ||
      merchant == std::string_view::npos || terminal == std::string_view::npos ||
      last == std::string_view::npos) {
    return false;
  }

  double amount = 0.0;
  int installments = 0;
  std::string_view requested_at;
  double customer_avg_amount = 0.0;
  int tx_count_24h = 0;
  std::string_view merchant_id;
  std::string_view merchant_mcc;
  double merchant_avg_amount = 0.0;
  bool is_online = false;
  bool card_present = false;
  double km_from_home = 0.0;
  if (!number_after(json, "\"amount\"", tx, amount) ||
      !int_after(json, "\"installments\"", tx, installments) ||
      !string_after(json, "\"requested_at\"", tx, requested_at) ||
      !number_after(json, "\"avg_amount\"", customer, customer_avg_amount) ||
      !int_after(json, "\"tx_count_24h\"", customer, tx_count_24h) ||
      !string_after(json, "\"id\"", merchant, merchant_id) ||
      !string_after(json, "\"mcc\"", merchant, merchant_mcc) ||
      !number_after(json, "\"avg_amount\"", merchant, merchant_avg_amount) ||
      !bool_after(json, "\"is_online\"", terminal, is_online) ||
      !bool_after(json, "\"card_present\"", terminal, card_present) ||
      !number_after(json, "\"km_from_home\"", terminal, km_from_home)) {
    return false;
  }

  int64_t requested_epoch = 0;
  int hour = 0;
  int weekday = 0;
  if (!parse_iso_utc(requested_at, requested_epoch, hour, weekday)) return false;

  const size_t known_key = json.find("\"known_merchants\"", customer);
  const size_t known_begin = known_key == std::string_view::npos ? std::string_view::npos
                                                                 : json.find('[', known_key);
  const size_t known_end = known_begin == std::string_view::npos ? std::string_view::npos
                                                                 : json.find(']', known_begin);
  const bool known_merchant =
      known_begin != std::string_view::npos && known_end != std::string_view::npos &&
      json.substr(known_begin, known_end - known_begin + 1).find(merchant_id) !=
          std::string_view::npos;

  out.fill(0.0f);
  out[0] = clamp01(static_cast<float>(amount / 10000.0));
  out[1] = clamp01(static_cast<float>(installments) / 12.0f);
  out[2] = normalized_ratio_fast(amount, customer_avg_amount, 10.0);
  out[3] = static_cast<float>(hour) / 23.0f;
  out[4] = static_cast<float>(weekday) / 6.0f;

  size_t last_value = 0;
  if (!value_after(json, "\"last_transaction\"", 0, last_value)) return false;
  if (json.substr(last_value, 4) == "null") {
    out[5] = -1.0f;
    out[6] = -1.0f;
  } else {
    std::string_view last_timestamp;
    double km_from_current = 0.0;
    if (!string_after(json, "\"timestamp\"", last, last_timestamp) ||
        !number_after(json, "\"km_from_current\"", last, km_from_current)) {
      return false;
    }
    int64_t last_epoch = 0;
    int last_hour = 0;
    int last_weekday = 0;
    if (!parse_iso_utc(last_timestamp, last_epoch, last_hour, last_weekday)) return false;
    int64_t delta_seconds = requested_epoch - last_epoch;
    if (delta_seconds < 0) delta_seconds = 0;
    out[5] = clamp01(static_cast<float>(delta_seconds) / 60.0f / 1440.0f);
    out[6] = clamp01(static_cast<float>(km_from_current / 1000.0));
  }

  out[7] = clamp01(static_cast<float>(km_from_home / 1000.0));
  out[8] = clamp01(static_cast<float>(tx_count_24h) / 20.0f);
  out[9] = is_online ? 1.0f : 0.0f;
  out[10] = card_present ? 1.0f : 0.0f;
  out[11] = known_merchant ? 0.0f : 1.0f;
  out[12] = mcc_risk_fast(merchant_mcc);
  out[13] = clamp01(static_cast<float>(merchant_avg_amount / 10000.0));
  return true;
}

}  // namespace rinha
