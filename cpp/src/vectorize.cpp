#include "rinha/vectorize.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace rinha {

namespace {

constexpr float kMaxAmount = 10'000.0f;
constexpr float kMaxInstallments = 12.0f;
constexpr float kAmountVsAvgRatio = 10.0f;
constexpr float kMaxMinutes = 1'440.0f;
constexpr float kMaxKilometers = 1'000.0f;
constexpr float kMaxTransactions24h = 20.0f;
constexpr float kMaxMerchantAvgAmount = 10'000.0f;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kSecondsPerDay = 86'400;

struct ParsedTimestamp {
    std::int64_t total_seconds = 0;
    std::uint8_t hour = 0;
    std::uint8_t weekday_monday0 = 0;
};

float clamp01(float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

std::uint32_t parse_digits(const char* bytes, std::size_t length, std::string& error) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char ch = static_cast<unsigned char>(bytes[index]);
        if (ch < '0' || ch > '9') {
            error = "timestamp inválido: caractere não numérico";
            return 0;
        }
        value = (value * 10U) + static_cast<std::uint32_t>(ch - '0');
    }
    return value;
}

bool is_leap_year(std::int32_t year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

std::uint8_t days_in_month(std::int32_t year, std::uint8_t month) noexcept {
    switch (month) {
        case 1:
        case 3:
        case 5:
        case 7:
        case 8:
        case 10:
        case 12:
            return 31;
        case 4:
        case 6:
        case 9:
        case 11:
            return 30;
        case 2:
            return static_cast<std::uint8_t>(is_leap_year(year) ? 29 : 28);
        default:
            return 0;
    }
}

std::int64_t days_from_civil(std::int32_t year, std::uint8_t month, std::uint8_t day) noexcept {
    const std::int32_t adjusted_year = year - static_cast<std::int32_t>(month <= 2);
    const std::int32_t era = adjusted_year >= 0 ? adjusted_year / 400 : (adjusted_year - 399) / 400;
    const std::int32_t year_of_era = adjusted_year - (era * 400);
    const std::int32_t shifted_month = static_cast<std::int32_t>(month) + (month > 2 ? -3 : 9);
    const std::int32_t day_of_year = ((153 * shifted_month) + 2) / 5 + static_cast<std::int32_t>(day) - 1;
    const std::int32_t day_of_era =
        (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;

    return static_cast<std::int64_t>((era * 146'097) + day_of_era - 719'468);
}

bool parse_timestamp(const std::string& value, ParsedTimestamp& parsed, std::string& error) {
    const char* bytes = value.c_str();
    if (value.size() != 20 ||
        bytes[4] != '-' ||
        bytes[7] != '-' ||
        bytes[10] != 'T' ||
        bytes[13] != ':' ||
        bytes[16] != ':' ||
        bytes[19] != 'Z') {
        error = "timestamp inválido: formato RFC3339 não suportado";
        return false;
    }

    const std::int32_t year = static_cast<std::int32_t>(parse_digits(bytes, 4, error));
    if (!error.empty()) {
        return false;
    }
    const std::uint8_t month = static_cast<std::uint8_t>(parse_digits(bytes + 5, 2, error));
    const std::uint8_t day = static_cast<std::uint8_t>(parse_digits(bytes + 8, 2, error));
    const std::uint8_t hour = static_cast<std::uint8_t>(parse_digits(bytes + 11, 2, error));
    const std::uint8_t minute = static_cast<std::uint8_t>(parse_digits(bytes + 14, 2, error));
    const std::uint8_t second = static_cast<std::uint8_t>(parse_digits(bytes + 17, 2, error));
    if (!error.empty()) {
        return false;
    }

    if (month < 1 || month > 12) {
        error = "timestamp inválido: mês fora do intervalo";
        return false;
    }

    const std::uint8_t max_day = days_in_month(year, month);
    if (day == 0 || day > max_day) {
        error = "timestamp inválido: dia fora do intervalo";
        return false;
    }

    if (hour > 23 || minute > 59 || second > 59) {
        error = "timestamp inválido: horário fora do intervalo";
        return false;
    }

    const std::int64_t days_since_unix_epoch = days_from_civil(year, month, day);
    parsed.total_seconds =
        (days_since_unix_epoch * kSecondsPerDay) +
        (static_cast<std::int64_t>(hour) * 3'600) +
        (static_cast<std::int64_t>(minute) * kSecondsPerMinute) +
        static_cast<std::int64_t>(second);
    parsed.hour = hour;
    parsed.weekday_monday0 = static_cast<std::uint8_t>((days_since_unix_epoch + 3) % 7);
    return true;
}

float amount_vs_avg(float amount, float avg_amount) noexcept {
    if (avg_amount <= 0.0f) {
        return amount <= 0.0f ? 0.0f : 1.0f;
    }
    return (amount / avg_amount) / kAmountVsAvgRatio;
}

float mcc_risk(const std::string& mcc) noexcept {
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

float bool_to_f32(bool value) noexcept {
    return value ? 1.0f : 0.0f;
}

}  // namespace

bool vectorize(const Payload& payload, QueryVector& vector, std::string& error) {
    ParsedTimestamp requested_at;
    if (!parse_timestamp(payload.transaction_requested_at, requested_at, error)) {
        return false;
    }

    float minutes_since_last_tx = -1.0f;
    float km_from_last_tx = -1.0f;
    if (payload.has_last_transaction) {
        ParsedTimestamp last_timestamp;
        if (!parse_timestamp(payload.last_transaction_timestamp, last_timestamp, error)) {
            return false;
        }
        const std::int64_t elapsed_minutes =
            std::max<std::int64_t>(0, (requested_at.total_seconds - last_timestamp.total_seconds) / kSecondsPerMinute);
        minutes_since_last_tx = clamp01(static_cast<float>(elapsed_minutes) / kMaxMinutes);
        km_from_last_tx = clamp01(payload.last_transaction_km_from_current / kMaxKilometers);
    }

    vector = {
        clamp01(payload.transaction_amount / kMaxAmount),
        clamp01(static_cast<float>(payload.transaction_installments) / kMaxInstallments),
        clamp01(amount_vs_avg(payload.transaction_amount, payload.customer_avg_amount)),
        static_cast<float>(requested_at.hour) / 23.0f,
        static_cast<float>(requested_at.weekday_monday0) / 6.0f,
        minutes_since_last_tx,
        km_from_last_tx,
        clamp01(payload.terminal_km_from_home / kMaxKilometers),
        clamp01(static_cast<float>(payload.customer_tx_count_24h) / kMaxTransactions24h),
        bool_to_f32(payload.terminal_is_online),
        bool_to_f32(payload.terminal_card_present),
        bool_to_f32(!payload.known_merchant),
        mcc_risk(payload.merchant_mcc),
        clamp01(payload.merchant_avg_amount / kMaxMerchantAvgAmount),
    };

    return true;
}

}  // namespace rinha
