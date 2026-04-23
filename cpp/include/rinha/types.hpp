#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rinha {

inline constexpr std::size_t kDimensions = 14;

struct Payload {
    float transaction_amount = 0.0f;
    std::uint32_t transaction_installments = 0;
    std::string transaction_requested_at;
    float customer_avg_amount = 0.0f;
    std::uint32_t customer_tx_count_24h = 0;
    bool known_merchant = false;
    std::string merchant_mcc;
    float merchant_avg_amount = 0.0f;
    bool terminal_is_online = false;
    bool terminal_card_present = false;
    float terminal_km_from_home = 0.0f;
    bool has_last_transaction = false;
    std::string last_transaction_timestamp;
    float last_transaction_km_from_current = 0.0f;
};

struct Classification {
    bool approved = true;
    float fraud_score = 0.0f;
};

using QueryVector = std::array<float, kDimensions>;

}  // namespace rinha
