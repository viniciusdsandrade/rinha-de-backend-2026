#include "rinha/request.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "simdjson.h"

namespace rinha {

namespace {

bool assign_string(const simdjson::dom::element& element, std::string& out, std::string& error, std::string_view field_name) {
    std::string_view value;
    if (const auto code = element.get(value); code != simdjson::SUCCESS) {
        error = "campo inválido: " + std::string(field_name);
        return false;
    }
    out.assign(value.data(), value.size());
    return true;
}

bool assign_float(const simdjson::dom::element& element, float& out, std::string& error, std::string_view field_name) {
    double value = 0.0;
    if (const auto code = element.get(value); code != simdjson::SUCCESS) {
        error = "campo inválido: " + std::string(field_name);
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

bool assign_uint32(const simdjson::dom::element& element, std::uint32_t& out, std::string& error, std::string_view field_name) {
    std::uint64_t value = 0;
    if (const auto code = element.get(value); code != simdjson::SUCCESS) {
        error = "campo inválido: " + std::string(field_name);
        return false;
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        error = "campo fora do intervalo: " + std::string(field_name);
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool assign_bool(const simdjson::dom::element& element, bool& out, std::string& error, std::string_view field_name) {
    if (const auto code = element.get(out); code != simdjson::SUCCESS) {
        error = "campo inválido: " + std::string(field_name);
        return false;
    }
    return true;
}

bool parse_transaction(const simdjson::dom::object& object, Payload& payload, std::string& error) {
    for (const auto field : object) {
        const std::string_view key = field.key;
        if (key == "amount") {
            if (!assign_float(field.value, payload.transaction_amount, error, "transaction.amount")) {
                return false;
            }
        } else if (key == "installments") {
            if (!assign_uint32(field.value, payload.transaction_installments, error, "transaction.installments")) {
                return false;
            }
        } else if (key == "requested_at") {
            if (!assign_string(field.value, payload.transaction_requested_at, error, "transaction.requested_at")) {
                return false;
            }
        }
    }
    return !payload.transaction_requested_at.empty();
}

bool parse_customer(
    const simdjson::dom::object& object,
    Payload& payload,
    std::array<std::string_view, 8>& known_merchants,
    std::size_t& known_merchants_count,
    std::vector<std::string_view>& overflow_known_merchants,
    std::string& error
) {
    for (const auto field : object) {
        const std::string_view key = field.key;
        if (key == "avg_amount") {
            if (!assign_float(field.value, payload.customer_avg_amount, error, "customer.avg_amount")) {
                return false;
            }
        } else if (key == "tx_count_24h") {
            if (!assign_uint32(field.value, payload.customer_tx_count_24h, error, "customer.tx_count_24h")) {
                return false;
            }
        } else if (key == "known_merchants") {
            simdjson::dom::array values;
            if (const auto code = field.value.get(values); code != simdjson::SUCCESS) {
                error = "campo inválido: customer.known_merchants";
                return false;
            }
            known_merchants_count = 0;
            overflow_known_merchants.clear();
            for (const auto merchant : values) {
                std::string_view merchant_id;
                if (const auto code = merchant.get(merchant_id); code != simdjson::SUCCESS) {
                    error = "campo inválido: customer.known_merchants[]";
                    return false;
                }
                if (known_merchants_count < known_merchants.size()) {
                    known_merchants[known_merchants_count++] = merchant_id;
                } else {
                    overflow_known_merchants.emplace_back(merchant_id);
                }
            }
        }
    }
    return true;
}

bool parse_merchant(
    const simdjson::dom::object& object,
    Payload& payload,
    std::string_view& merchant_id,
    std::string& error
) {
    for (const auto field : object) {
        const std::string_view key = field.key;
        if (key == "id") {
            if (const auto code = field.value.get(merchant_id); code != simdjson::SUCCESS) {
                error = "campo inválido: merchant.id";
                return false;
            }
        } else if (key == "mcc") {
            if (!assign_string(field.value, payload.merchant_mcc, error, "merchant.mcc")) {
                return false;
            }
        } else if (key == "avg_amount") {
            if (!assign_float(field.value, payload.merchant_avg_amount, error, "merchant.avg_amount")) {
                return false;
            }
        }
    }
    return !merchant_id.empty() && !payload.merchant_mcc.empty();
}

bool parse_terminal(const simdjson::dom::object& object, Payload& payload, std::string& error) {
    for (const auto field : object) {
        const std::string_view key = field.key;
        if (key == "is_online") {
            if (!assign_bool(field.value, payload.terminal_is_online, error, "terminal.is_online")) {
                return false;
            }
        } else if (key == "card_present") {
            if (!assign_bool(field.value, payload.terminal_card_present, error, "terminal.card_present")) {
                return false;
            }
        } else if (key == "km_from_home") {
            if (!assign_float(field.value, payload.terminal_km_from_home, error, "terminal.km_from_home")) {
                return false;
            }
        }
    }
    return true;
}

bool parse_last_transaction(const simdjson::dom::element& element, Payload& payload, std::string& error) {
    if (element.is_null()) {
        payload.has_last_transaction = false;
        payload.last_transaction_timestamp.clear();
        payload.last_transaction_km_from_current = 0.0f;
        return true;
    }

    simdjson::dom::object object;
    if (const auto code = element.get(object); code != simdjson::SUCCESS) {
        error = "campo inválido: last_transaction";
        return false;
    }

    payload.has_last_transaction = true;
    payload.last_transaction_timestamp.clear();
    for (const auto field : object) {
        const std::string_view key = field.key;
        if (key == "timestamp") {
            if (!assign_string(field.value, payload.last_transaction_timestamp, error, "last_transaction.timestamp")) {
                return false;
            }
        } else if (key == "km_from_current") {
            if (!assign_float(field.value, payload.last_transaction_km_from_current, error, "last_transaction.km_from_current")) {
                return false;
            }
        }
    }

    if (payload.last_transaction_timestamp.empty()) {
        error = "campo inválido: last_transaction.timestamp";
        return false;
    }

    return true;
}

}  // namespace

bool parse_payload(std::string_view body, Payload& payload, std::string& error) {
    thread_local simdjson::dom::parser parser;

    simdjson::padded_string json(body);
    simdjson::dom::element root;
    if (const auto code = parser.parse(json).get(root); code != simdjson::SUCCESS) {
        error = "json inválido";
        return false;
    }

    simdjson::dom::object object;
    if (const auto code = root.get(object); code != simdjson::SUCCESS) {
        error = "payload inválido";
        return false;
    }

    std::array<std::string_view, 8> known_merchants{};
    std::size_t known_merchants_count = 0;
    std::vector<std::string_view> overflow_known_merchants;
    std::string_view merchant_id;

    for (const auto field : object) {
        const std::string_view key = field.key;
        if (key == "transaction") {
            simdjson::dom::object transaction;
            if (const auto code = field.value.get(transaction); code != simdjson::SUCCESS || !parse_transaction(transaction, payload, error)) {
                if (error.empty()) {
                    error = "campo inválido: transaction";
                }
                return false;
            }
        } else if (key == "customer") {
            simdjson::dom::object customer;
            if (const auto code = field.value.get(customer);
                code != simdjson::SUCCESS ||
                !parse_customer(customer, payload, known_merchants, known_merchants_count, overflow_known_merchants, error)) {
                if (error.empty()) {
                    error = "campo inválido: customer";
                }
                return false;
            }
        } else if (key == "merchant") {
            simdjson::dom::object merchant;
            if (const auto code = field.value.get(merchant); code != simdjson::SUCCESS || !parse_merchant(merchant, payload, merchant_id, error)) {
                if (error.empty()) {
                    error = "campo inválido: merchant";
                }
                return false;
            }
        } else if (key == "terminal") {
            simdjson::dom::object terminal;
            if (const auto code = field.value.get(terminal); code != simdjson::SUCCESS || !parse_terminal(terminal, payload, error)) {
                if (error.empty()) {
                    error = "campo inválido: terminal";
                }
                return false;
            }
        } else if (key == "last_transaction") {
            if (!parse_last_transaction(field.value, payload, error)) {
                return false;
            }
        }
    }

    if (payload.transaction_requested_at.empty() || payload.merchant_mcc.empty()) {
        error = "payload incompleto";
        return false;
    }

    payload.known_merchant = std::find(known_merchants.begin(), known_merchants.begin() + known_merchants_count, merchant_id) !=
        known_merchants.begin() + known_merchants_count;
    if (!payload.known_merchant && !overflow_known_merchants.empty()) {
        payload.known_merchant =
            std::find(overflow_known_merchants.begin(), overflow_known_merchants.end(), merchant_id) != overflow_known_merchants.end();
    }
    return true;
}

}  // namespace rinha
