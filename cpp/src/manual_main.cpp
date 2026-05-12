#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "rinha/ivf.hpp"
#include "rinha/request.hpp"
#include "rinha/types.hpp"
#include "rinha/vectorize.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kReadChunk = 4096;
constexpr std::size_t kMaxPending = 16 * 1024;
constexpr int kMaxEvents = 1024;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kSecondsPerDay = 86'400;

constexpr std::string_view kReadyResponse =
    "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
constexpr std::string_view kNotFoundResponse =
    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
constexpr std::string_view kBadRequestResponse =
    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";

constexpr std::array<std::string_view, 6> kFraudResponses{{
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}",
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}",
    "HTTP/1.1 200 OK\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}",
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}",
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}",
    "HTTP/1.1 200 OK\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}",
}};

struct Connection {
    int fd = -1;
    std::array<char, kMaxPending> in{};
    std::size_t in_len = 0;
    std::string out;
    std::size_t out_pos = 0;
};

std::string env_or_default(const char* key, std::string default_value) {
    if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    return default_value;
}

std::optional<std::string> optional_env(const char* key) {
    if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    return std::nullopt;
}

std::uint32_t uint_env_or_default(const char* key, std::uint32_t default_value) {
    const std::optional<std::string> value = optional_env(key);
    if (!value) {
        return default_value;
    }
    try {
        return static_cast<std::uint32_t>(std::stoul(*value));
    } catch (...) {
        return default_value;
    }
}

bool bool_env_or_default(const char* key, bool default_value) {
    const std::optional<std::string> value = optional_env(key);
    if (!value) {
        return default_value;
    }
    return *value == "1" || *value == "true" || *value == "TRUE";
}

rinha::IvfSearchConfig ivf_config_from_env() {
    rinha::IvfSearchConfig config{};
    config.fast_nprobe = uint_env_or_default("IVF_FAST_NPROBE", 1);
    config.full_nprobe = uint_env_or_default("IVF_FULL_NPROBE", config.fast_nprobe);
    config.boundary_full = bool_env_or_default("IVF_BOUNDARY_FULL", config.full_nprobe > config.fast_nprobe);
    config.bbox_repair = bool_env_or_default("IVF_BBOX_REPAIR", true);
    config.repair_min_frauds = static_cast<std::uint8_t>(
        std::min<std::uint32_t>(uint_env_or_default("IVF_REPAIR_MIN_FRAUDS", 2), 5)
    );
    config.repair_max_frauds = static_cast<std::uint8_t>(
        std::min<std::uint32_t>(uint_env_or_default("IVF_REPAIR_MAX_FRAUDS", 3), 5)
    );
    return config;
}

bool prepare_unix_socket(const std::string& path, std::string& error) {
    const fs::path socket_path(path);
    if (socket_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(socket_path.parent_path(), ec);
        if (ec) {
            error = "falha ao criar diretório do socket " + path + ": " + ec.message();
            return false;
        }
    }

    std::error_code ec;
    if (fs::exists(socket_path, ec) && !ec) {
        fs::remove(socket_path, ec);
        if (ec) {
            error = "falha ao remover socket antigo " + path + ": " + ec.message();
            return false;
        }
    }
    return true;
}

int listen_unix_socket(const std::string& path, std::string& error) {
    if (!prepare_unix_socket(path, error)) {
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = "falha ao criar socket unix";
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        error = "caminho do socket unix longo demais";
        close(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1U);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "falha no bind unix socket";
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0777);
    if (listen(fd, 4096) != 0) {
        error = "falha no listen unix socket";
        close(fd);
        return -1;
    }
    return fd;
}

std::optional<std::size_t> find_header_end(const char* buffer, std::size_t len) {
    for (std::size_t index = 0; index + 3U < len; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' &&
            buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_byte(std::string_view value, char needle) {
    const std::size_t pos = value.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    return pos;
}

std::string_view trim_cr(std::string_view line) noexcept {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

std::string_view trim_space(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

bool iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        const unsigned char lower_a = static_cast<unsigned char>(a >= 'A' && a <= 'Z' ? a + 32U : a);
        const unsigned char lower_b = static_cast<unsigned char>(b >= 'A' && b <= 'Z' ? b + 32U : b);
        if (lower_a != lower_b) {
            return false;
        }
    }
    return true;
}

std::optional<std::size_t> parse_content_length(std::string_view headers) {
    std::size_t cursor = 0;
    while (cursor < headers.size()) {
        const std::size_t next = headers.find('\n', cursor);
        const std::size_t end = next == std::string_view::npos ? headers.size() : next;
        const std::string_view line = trim_cr(headers.substr(cursor, end - cursor));
        if (const auto colon = find_byte(line, ':')) {
            const std::string_view name = line.substr(0, *colon);
            if (iequals(name, "content-length")) {
                std::string_view number = trim_space(line.substr(*colon + 1U));
                std::size_t value = 0;
                bool seen = false;
                for (const char ch : number) {
                    if (ch < '0' || ch > '9') {
                        break;
                    }
                    seen = true;
                    value = (value * 10U) + static_cast<std::size_t>(ch - '0');
                }
                if (seen) {
                    return value;
                }
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        cursor = next + 1U;
    }
    return std::nullopt;
}

std::string_view response_for_score(std::uint8_t fraud_count) noexcept {
    return kFraudResponses[std::min<std::uint8_t>(fraud_count, 5)];
}

float clamp01(float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

bool to_next_value(std::size_t& cursor, std::string_view body) noexcept {
    while (cursor < body.size()) {
        const void* colon_ptr = std::memchr(body.data() + cursor, ':', body.size() - cursor);
        const void* quote_ptr = std::memchr(body.data() + cursor, '"', body.size() - cursor);
        if (colon_ptr == nullptr && quote_ptr == nullptr) {
            return false;
        }
        const auto colon_pos = colon_ptr == nullptr
            ? body.size()
            : static_cast<std::size_t>(static_cast<const char*>(colon_ptr) - body.data());
        const auto quote_pos = quote_ptr == nullptr
            ? body.size()
            : static_cast<std::size_t>(static_cast<const char*>(quote_ptr) - body.data());
        if (colon_pos < quote_pos) {
            cursor = colon_pos + 1U;
            while (cursor < body.size() &&
                   (body[cursor] == ' ' || body[cursor] == '\t' || body[cursor] == '\n' || body[cursor] == '\r')) {
                ++cursor;
            }
            return true;
        }
        cursor = quote_pos + 1U;
        const void* end_quote = std::memchr(body.data() + cursor, '"', body.size() - cursor);
        if (end_quote == nullptr) {
            return false;
        }
        cursor = static_cast<std::size_t>(static_cast<const char*>(end_quote) - body.data()) + 1U;
    }
    return false;
}

bool skip_json_string(std::size_t& cursor, std::string_view body) noexcept {
    if (cursor < body.size() && body[cursor] == '"') {
        ++cursor;
    }
    const void* end_quote = std::memchr(body.data() + cursor, '"', body.size() - cursor);
    if (end_quote == nullptr) {
        return false;
    }
    cursor = static_cast<std::size_t>(static_cast<const char*>(end_quote) - body.data()) + 1U;
    return true;
}

bool scan_json_string(std::size_t& cursor, std::string_view body, std::string_view& value) noexcept {
    if (cursor >= body.size() || body[cursor] != '"') {
        return false;
    }
    ++cursor;
    const std::size_t start = cursor;
    const void* end_quote = std::memchr(body.data() + cursor, '"', body.size() - cursor);
    if (end_quote == nullptr) {
        return false;
    }
    const auto end = static_cast<std::size_t>(static_cast<const char*>(end_quote) - body.data());
    value = body.substr(start, end - start);
    cursor = end + 1U;
    return true;
}

bool scan_u32(std::size_t& cursor, std::string_view body, std::uint32_t& value) noexcept {
    std::uint32_t parsed = 0;
    bool seen = false;
    while (cursor < body.size() && body[cursor] >= '0' && body[cursor] <= '9') {
        parsed = (parsed * 10U) + static_cast<std::uint32_t>(body[cursor] - '0');
        ++cursor;
        seen = true;
    }
    value = parsed;
    return seen;
}

bool scan_f32(std::size_t& cursor, std::string_view body, float& value) noexcept {
    const char* begin = body.data() + cursor;
    const char* end = body.data() + body.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc()) {
        return false;
    }
    cursor = static_cast<std::size_t>(ptr - body.data());
    return true;
}

std::int64_t days_from_civil_fast(std::int32_t year, std::uint8_t month, std::uint8_t day) noexcept {
    const std::int32_t adjusted_year = year - static_cast<std::int32_t>(month <= 2);
    const std::int32_t era = adjusted_year >= 0 ? adjusted_year / 400 : (adjusted_year - 399) / 400;
    const std::int32_t year_of_era = adjusted_year - (era * 400);
    const std::int32_t shifted_month = static_cast<std::int32_t>(month) + (month > 2 ? -3 : 9);
    const std::int32_t day_of_year = ((153 * shifted_month) + 2) / 5 + static_cast<std::int32_t>(day) - 1;
    const std::int32_t day_of_era =
        (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
    return static_cast<std::int64_t>((era * 146'097) + day_of_era - 719'468);
}

struct FastTimestamp {
    std::int64_t total_seconds = 0;
    std::uint8_t hour = 0;
    std::uint8_t weekday_monday0 = 0;
};

bool scan_iso_timestamp(std::size_t& cursor, std::string_view body, FastTimestamp& parsed) noexcept {
    if (cursor < body.size() && body[cursor] == '"') {
        ++cursor;
    }
    if (cursor + 20U > body.size()) {
        return false;
    }
    const char* s = body.data() + cursor;
    const auto digit = [](char ch) noexcept -> std::uint8_t {
        return static_cast<std::uint8_t>(ch - '0');
    };
    const std::int32_t year = (digit(s[0]) * 1000) + (digit(s[1]) * 100) + (digit(s[2]) * 10) + digit(s[3]);
    const std::uint8_t month = static_cast<std::uint8_t>((digit(s[5]) * 10) + digit(s[6]));
    const std::uint8_t day = static_cast<std::uint8_t>((digit(s[8]) * 10) + digit(s[9]));
    const std::uint8_t hour = static_cast<std::uint8_t>((digit(s[11]) * 10) + digit(s[12]));
    const std::uint8_t minute = static_cast<std::uint8_t>((digit(s[14]) * 10) + digit(s[15]));
    const std::uint8_t second = static_cast<std::uint8_t>((digit(s[17]) * 10) + digit(s[18]));
    const std::int64_t days = days_from_civil_fast(year, month, day);
    parsed.total_seconds =
        (days * kSecondsPerDay) + (static_cast<std::int64_t>(hour) * 3600) +
        (static_cast<std::int64_t>(minute) * kSecondsPerMinute) + static_cast<std::int64_t>(second);
    parsed.hour = hour;
    parsed.weekday_monday0 = static_cast<std::uint8_t>((days + 3) % 7);
    cursor += 20U;
    if (cursor < body.size() && body[cursor] == '"') {
        ++cursor;
    }
    return true;
}

bool scan_bool(std::size_t& cursor, std::string_view body, bool& value) noexcept {
    if (cursor < body.size() && body[cursor] == 't') {
        value = true;
        cursor += 4U;
        return true;
    }
    if (cursor < body.size() && body[cursor] == 'f') {
        value = false;
        cursor += 5U;
        return true;
    }
    return false;
}

bool scan_mcc(std::size_t& cursor, std::string_view body, std::uint32_t& value) noexcept {
    if (cursor < body.size() && body[cursor] == '"') {
        ++cursor;
    }
    if (!scan_u32(cursor, body, value)) {
        return false;
    }
    if (cursor < body.size() && body[cursor] == '"') {
        ++cursor;
    }
    return true;
}

float mcc_risk_fast(std::uint32_t mcc) noexcept {
    switch (mcc) {
        case 5411: return 0.15f;
        case 5812: return 0.30f;
        case 5912: return 0.20f;
        case 5944: return 0.45f;
        case 7801: return 0.80f;
        case 7802: return 0.75f;
        case 7995: return 0.85f;
        case 4511: return 0.35f;
        case 5311: return 0.25f;
        case 5999: return 0.50f;
        default: return 0.50f;
    }
}

bool fast_vectorize_payload(std::string_view body, rinha::QueryVector& vector) noexcept {
    std::size_t cursor = 0;
    if (!to_next_value(cursor, body) || !skip_json_string(cursor, body)) return false;

    float amount = 0.0f;
    std::uint32_t installments = 0;
    FastTimestamp requested_at{};
    float customer_avg_amount = 0.0f;
    std::uint32_t tx_count_24h = 0;
    std::array<std::string_view, 16> known_merchants{};
    std::size_t known_count = 0;
    std::string_view merchant_id;
    std::uint32_t mcc = 0;
    float merchant_avg_amount = 0.0f;
    bool is_online = false;
    bool card_present = false;
    float km_from_home = 0.0f;
    bool has_last_tx = false;
    FastTimestamp last_timestamp{};
    float km_from_current = 0.0f;

    if (!to_next_value(cursor, body) || !to_next_value(cursor, body) || !scan_f32(cursor, body, amount)) return false;
    if (!to_next_value(cursor, body) || !scan_u32(cursor, body, installments)) return false;
    if (!to_next_value(cursor, body) || !scan_iso_timestamp(cursor, body, requested_at)) return false;
    if (!to_next_value(cursor, body) || !to_next_value(cursor, body) || !scan_f32(cursor, body, customer_avg_amount)) return false;
    if (!to_next_value(cursor, body) || !scan_u32(cursor, body, tx_count_24h)) return false;
    if (!to_next_value(cursor, body)) return false;
    while (cursor < body.size() && body[cursor] != ']') {
        if (body[cursor] == '"') {
            std::string_view merchant;
            if (!scan_json_string(cursor, body, merchant)) return false;
            if (known_count < known_merchants.size()) {
                known_merchants[known_count++] = merchant;
            }
        } else {
            ++cursor;
        }
    }
    if (cursor < body.size()) ++cursor;

    if (!to_next_value(cursor, body) || !to_next_value(cursor, body) || !scan_json_string(cursor, body, merchant_id)) return false;
    if (!to_next_value(cursor, body) || !scan_mcc(cursor, body, mcc)) return false;
    if (!to_next_value(cursor, body) || !scan_f32(cursor, body, merchant_avg_amount)) return false;
    if (!to_next_value(cursor, body) || !to_next_value(cursor, body) || !scan_bool(cursor, body, is_online)) return false;
    if (!to_next_value(cursor, body) || !scan_bool(cursor, body, card_present)) return false;
    if (!to_next_value(cursor, body) || !scan_f32(cursor, body, km_from_home)) return false;
    if (!to_next_value(cursor, body)) return false;
    has_last_tx = cursor < body.size() && body[cursor] != 'n';
    if (has_last_tx) {
        if (!to_next_value(cursor, body) || !scan_iso_timestamp(cursor, body, last_timestamp)) return false;
        if (!to_next_value(cursor, body) || !scan_f32(cursor, body, km_from_current)) return false;
    }

    bool known_merchant = false;
    for (std::size_t index = 0; index < known_count; ++index) {
        known_merchant = known_merchant || known_merchants[index] == merchant_id;
    }

    float minutes_since_last_tx = -1.0f;
    float km_from_last_tx = -1.0f;
    if (has_last_tx) {
        const std::int64_t elapsed_minutes =
            std::max<std::int64_t>(0, (requested_at.total_seconds - last_timestamp.total_seconds) / kSecondsPerMinute);
        minutes_since_last_tx = clamp01(static_cast<float>(elapsed_minutes) / 1440.0f);
        km_from_last_tx = clamp01(km_from_current / 1000.0f);
    }

    const float amount_vs_avg = customer_avg_amount <= 0.0f
        ? (amount <= 0.0f ? 0.0f : 1.0f)
        : (amount / customer_avg_amount) / 10.0f;

    vector = {
        clamp01(amount / 10000.0f),
        clamp01(static_cast<float>(installments) / 12.0f),
        clamp01(amount_vs_avg),
        static_cast<float>(requested_at.hour) / 23.0f,
        static_cast<float>(requested_at.weekday_monday0) / 6.0f,
        minutes_since_last_tx,
        km_from_last_tx,
        clamp01(km_from_home / 1000.0f),
        clamp01(static_cast<float>(tx_count_24h) / 20.0f),
        is_online ? 1.0f : 0.0f,
        card_present ? 1.0f : 0.0f,
        known_merchant ? 0.0f : 1.0f,
        mcc_risk_fast(mcc),
        clamp01(merchant_avg_amount / 10000.0f),
    };
    return true;
}

std::uint8_t classify_body(
    std::string_view body,
    const rinha::IvfIndex& index,
    const rinha::IvfSearchConfig& config
) noexcept {
    rinha::QueryVector query{};
    if (fast_vectorize_payload(body, query)) [[likely]] {
        return index.fraud_count(query, config);
    }

    rinha::Payload payload;
    std::string error;
    if (!rinha::parse_payload(body, payload, error)) {
        return 0;
    }
    if (!rinha::vectorize(payload, query, error)) {
        return 0;
    }
    return index.fraud_count(query, config);
}

void append_response(
    Connection& conn,
    std::string_view request,
    std::string_view body,
    const rinha::IvfIndex& index,
    const rinha::IvfSearchConfig& config
) {
    if (request.starts_with("GET /ready ")) {
        conn.out.append(kReadyResponse);
        return;
    }
    if (request.starts_with("POST /fraud-score ")) [[likely]] {
        conn.out.append(response_for_score(classify_body(body, index, config)));
        return;
    }
    conn.out.append(kNotFoundResponse);
}

bool process_requests(Connection& conn, const rinha::IvfIndex& index, const rinha::IvfSearchConfig& config) {
    while (true) {
        const std::optional<std::size_t> header_end = find_header_end(conn.in.data(), conn.in_len);
        if (!header_end) {
            return conn.in_len <= kMaxPending;
        }

        const std::size_t header_len = *header_end + 4U;
        const std::string_view header(conn.in.data(), *header_end);
        const std::size_t first_line_end = header.find('\n');
        if (first_line_end == std::string_view::npos) {
            conn.out.append(kBadRequestResponse);
            const std::size_t remaining = conn.in_len - header_len;
            std::memmove(conn.in.data(), conn.in.data() + header_len, remaining);
            conn.in_len = remaining;
            continue;
        }

        const std::string_view first_line = trim_cr(header.substr(0, first_line_end));
        const std::size_t content_length = parse_content_length(header.substr(first_line_end + 1U)).value_or(0);
        const std::size_t total_len = header_len + content_length;
        if (conn.in_len < total_len) {
            return total_len <= kMaxPending;
        }

        const std::string_view body(conn.in.data() + header_len, content_length);
        append_response(conn, first_line, body, index, config);
        const std::size_t remaining = conn.in_len - total_len;
        std::memmove(conn.in.data(), conn.in.data() + total_len, remaining);
        conn.in_len = remaining;
    }
}

bool flush_output(Connection& conn) {
    while (conn.out_pos < conn.out.size()) {
        const ssize_t written = send(
            conn.fd,
            conn.out.data() + conn.out_pos,
            conn.out.size() - conn.out_pos,
            MSG_NOSIGNAL
        );
        if (written > 0) {
            conn.out_pos += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
    conn.out.clear();
    conn.out_pos = 0;
    return true;
}

bool update_events(int epoll_fd, Connection& conn) {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    if (!conn.out.empty()) {
        event.events |= EPOLLOUT;
    }
    event.data.ptr = &conn;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn.fd, &event) == 0;
}

void close_connection(int epoll_fd, std::unordered_map<int, std::unique_ptr<Connection>>& connections, Connection* conn) {
    if (conn == nullptr) {
        return;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr);
    close(conn->fd);
    connections.erase(conn->fd);
}

bool add_connection(int epoll_fd, std::unordered_map<int, std::unique_ptr<Connection>>& connections, int fd) {
    auto conn = std::make_unique<Connection>();
    conn->fd = fd;
    conn->out.reserve(512);

    epoll_event conn_event{};
    conn_event.events = EPOLLIN | EPOLLRDHUP;
    conn_event.data.ptr = conn.get();
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &conn_event) != 0) {
        close(fd);
        return false;
    }
    connections.emplace(fd, std::move(conn));
    return true;
}

std::optional<int> receive_fd(int socket_fd) noexcept {
    char byte = 0;
    iovec iov{};
    iov.iov_base = &byte;
    iov.iov_len = sizeof(byte);

    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    const ssize_t received = recvmsg(socket_fd, &message, 0);
    if (received <= 0) {
        return std::nullopt;
    }

    cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        return std::nullopt;
    }

    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd >= 0 ? std::optional<int>{fd} : std::nullopt;
}

void run_control_socket(const std::string& ctrl_path, int notify_fd) {
    std::string error;
    const int ctrl_fd = listen_unix_socket(ctrl_path, error);
    if (ctrl_fd < 0) {
        std::cerr << error << '\n';
        return;
    }

    while (true) {
        const int conn_fd = accept4(ctrl_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            break;
        }

        while (true) {
            std::optional<int> fd = receive_fd(conn_fd);
            if (!fd) {
                break;
            }
            const int flags = fcntl(*fd, F_GETFL, 0);
            if (flags >= 0) {
                (void)fcntl(*fd, F_SETFL, flags | O_NONBLOCK);
            }
            (void)fcntl(*fd, F_SETFD, FD_CLOEXEC);
            if (write(notify_fd, &*fd, sizeof(*fd)) != static_cast<ssize_t>(sizeof(*fd))) {
                close(*fd);
            }
        }
        close(conn_fd);
    }

    close(ctrl_fd);
}

void drain_transferred_fds(
    int notify_fd,
    int epoll_fd,
    std::unordered_map<int, std::unique_ptr<Connection>>& connections
) {
    while (true) {
        int fd = -1;
        const ssize_t read_bytes = read(notify_fd, &fd, sizeof(fd));
        if (read_bytes == static_cast<ssize_t>(sizeof(fd))) {
            add_connection(epoll_fd, connections, fd);
            continue;
        }
        if (read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        return;
    }
}

int run_server(const std::string& socket_path, const rinha::IvfIndex& index, const rinha::IvfSearchConfig& config) {
    std::string error;
    const int listen_fd = listen_unix_socket(socket_path, error);
    if (listen_fd < 0) {
        std::cerr << error << '\n';
        return 1;
    }

    std::array<int, 2> notify_pipe{-1, -1};
    if (pipe2(notify_pipe.data(), O_NONBLOCK | O_CLOEXEC) != 0) {
        std::cerr << "falha ao criar pipe de controle\n";
        close(listen_fd);
        return 1;
    }
    std::thread(run_control_socket, socket_path + ".ctrl", notify_pipe[1]).detach();

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::cerr << "falha ao criar epoll\n";
        close(notify_pipe[0]);
        close(notify_pipe[1]);
        close(listen_fd);
        return 1;
    }

    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) != 0) {
        std::cerr << "falha ao registrar listen fd\n";
        close(epoll_fd);
        close(notify_pipe[0]);
        close(notify_pipe[1]);
        close(listen_fd);
        return 1;
    }

    epoll_event notify_event{};
    notify_event.events = EPOLLIN;
    notify_event.data.fd = notify_pipe[0];
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notify_pipe[0], &notify_event) != 0) {
        std::cerr << "falha ao registrar pipe de controle\n";
        close(epoll_fd);
        close(notify_pipe[0]);
        close(notify_pipe[1]);
        close(listen_fd);
        return 1;
    }

    std::unordered_map<int, std::unique_ptr<Connection>> connections;
    std::array<epoll_event, kMaxEvents> events{};
    while (true) {
        const int count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int index_event = 0; index_event < count; ++index_event) {
            epoll_event& event = events[static_cast<std::size_t>(index_event)];
            if (event.data.fd == listen_fd) {
                while (true) {
                    const int fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        continue;
                    }
                    add_connection(epoll_fd, connections, fd);
                }
                continue;
            }
            if (event.data.fd == notify_pipe[0]) {
                drain_transferred_fds(notify_pipe[0], epoll_fd, connections);
                continue;
            }

            auto* conn = static_cast<Connection*>(event.data.ptr);
            bool alive = true;
            if ((event.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
                alive = false;
            }

            if (alive && (event.events & EPOLLIN) != 0U) {
                while (true) {
                    const std::size_t available = kMaxPending - conn->in_len;
                    if (available == 0) {
                        alive = false;
                        break;
                    }
                    const std::size_t read_size = std::min<std::size_t>(kReadChunk, available);
                    const ssize_t read_bytes = recv(conn->fd, conn->in.data() + conn->in_len, read_size, 0);
                    if (read_bytes > 0) {
                        conn->in_len += static_cast<std::size_t>(read_bytes);
                        if (!process_requests(*conn, index, config)) {
                            alive = false;
                            break;
                        }
                        continue;
                    }
                    if (read_bytes == 0) {
                        alive = false;
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    alive = false;
                    break;
                }
            }

            if (alive && (event.events & EPOLLOUT) != 0U) {
                alive = flush_output(*conn);
            }
            if (alive && !conn->out.empty()) {
                alive = flush_output(*conn);
            }

            if (!alive) {
                close_connection(epoll_fd, connections, conn);
            } else {
                update_events(epoll_fd, *conn);
            }
        }
    }

    close(epoll_fd);
    close(notify_pipe[0]);
    close(notify_pipe[1]);
    close(listen_fd);
    return 1;
}

}  // namespace

int main() {
    std::string error;
    rinha::IvfIndex index;
    if (!index.load_binary(env_or_default("IVF_INDEX_PATH", "/app/data/index.bin"), error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::optional<std::string> socket_path = optional_env("UNIX_SOCKET_PATH");
    if (!socket_path) {
        std::cerr << "UNIX_SOCKET_PATH obrigatório no servidor manual experimental\n";
        return 1;
    }

    return run_server(*socket_path, index, ivf_config_from_env());
}
