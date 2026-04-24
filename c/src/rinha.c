#include "rinha.h"

#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

typedef struct {
    const char *p;
    const char *end;
} JsonCursor;

typedef struct {
    int64_t total_seconds;
    uint8_t hour;
    uint8_t weekday_monday0;
} ParsedTimestamp;

static bool parse_timestamp_fixed(const char *value, ParsedTimestamp *parsed);
static bool parse_timestamp(const char *value, ParsedTimestamp *parsed);
static float mcc_risk(const char *mcc);
static float mcc_risk_view(const char *mcc, size_t len);

static void set_error(char *error, size_t error_len, const char *message) {
    if (error != NULL && error_len > 0) {
        snprintf(error, error_len, "%s", message);
    }
}

static void skip_ws(JsonCursor *cursor) {
    while (cursor->p < cursor->end) {
        const unsigned char ch = (unsigned char)*cursor->p;
        if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
            break;
        }
        cursor->p++;
    }
}

static bool consume_char(JsonCursor *cursor, char expected) {
    skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != expected) {
        return false;
    }
    cursor->p++;
    return true;
}

static bool consume_literal(JsonCursor *cursor, const char *literal) {
    skip_ws(cursor);
    const size_t len = strlen(literal);
    if ((size_t)(cursor->end - cursor->p) < len || memcmp(cursor->p, literal, len) != 0) {
        return false;
    }
    cursor->p += len;
    return true;
}

static bool parse_string(JsonCursor *cursor, char *out, size_t out_len) {
    skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != '"') {
        return false;
    }
    cursor->p++;

    size_t written = 0;
    while (cursor->p < cursor->end) {
        unsigned char ch = (unsigned char)*cursor->p++;
        if (ch == '"') {
            if (out != NULL && out_len > 0) {
                out[written < out_len ? written : out_len - 1] = '\0';
            }
            return true;
        }

        if (ch == '\\') {
            if (cursor->p >= cursor->end) {
                return false;
            }
            ch = (unsigned char)*cursor->p++;
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'u':
                    if ((size_t)(cursor->end - cursor->p) < 4) {
                        return false;
                    }
                    cursor->p += 4;
                    ch = '?';
                    break;
                default:
                    return false;
            }
        }

        if (out != NULL && out_len > 0) {
            if (written + 1 >= out_len) {
                return false;
            }
            out[written++] = (char)ch;
        }
    }

    return false;
}

static bool parse_number_token(JsonCursor *cursor, char token[64]) {
    skip_ws(cursor);
    const char *start = cursor->p;
    while (cursor->p < cursor->end) {
        const char ch = *cursor->p;
        if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E') {
            cursor->p++;
            continue;
        }
        break;
    }

    const size_t len = (size_t)(cursor->p - start);
    if (len == 0 || len >= 64) {
        return false;
    }

    memcpy(token, start, len);
    token[len] = '\0';
    return true;
}

static bool parse_float_value(JsonCursor *cursor, float *out) {
    skip_ws(cursor);
    const char *p = cursor->p;
    if (p >= cursor->end) {
        return false;
    }

    bool negative = false;
    if (*p == '-' || *p == '+') {
        negative = *p == '-';
        p++;
    }

    bool has_digits = false;
    double value = 0.0;
    while (p < cursor->end && *p >= '0' && *p <= '9') {
        has_digits = true;
        value = (value * 10.0) + (double)(*p - '0');
        p++;
    }

    if (p < cursor->end && *p == '.') {
        p++;
        double scale = 0.1;
        while (p < cursor->end && *p >= '0' && *p <= '9') {
            has_digits = true;
            value += (double)(*p - '0') * scale;
            scale *= 0.1;
            p++;
        }
    }

    if (!has_digits) {
        return false;
    }

    if (p < cursor->end && (*p == 'e' || *p == 'E')) {
        p++;
        bool exponent_negative = false;
        if (p < cursor->end && (*p == '-' || *p == '+')) {
            exponent_negative = *p == '-';
            p++;
        }

        bool has_exponent_digits = false;
        int exponent = 0;
        while (p < cursor->end && *p >= '0' && *p <= '9') {
            has_exponent_digits = true;
            if (exponent < 64) {
                exponent = (exponent * 10) + (*p - '0');
            }
            p++;
        }
        if (!has_exponent_digits) {
            return false;
        }

        double multiplier = 1.0;
        for (int index = 0; index < exponent; ++index) {
            multiplier *= 10.0;
        }
        value = exponent_negative ? value / multiplier : value * multiplier;
    }

    cursor->p = p;
    *out = (float)(negative ? -value : value);
    return true;
}

static bool parse_uint32_value(JsonCursor *cursor, uint32_t *out) {
    skip_ws(cursor);
    const char *p = cursor->p;
    if (p >= cursor->end || *p < '0' || *p > '9') {
        return false;
    }

    uint32_t value = 0;
    do {
        const uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = (value * 10U) + digit;
        p++;
    } while (p < cursor->end && *p >= '0' && *p <= '9');

    if (p < cursor->end && (*p == '.' || *p == 'e' || *p == 'E')) {
        return false;
    }

    cursor->p = p;
    *out = value;
    return true;
}

static bool parse_bool_value(JsonCursor *cursor, bool *out) {
    if (consume_literal(cursor, "true")) {
        *out = true;
        return true;
    }
    if (consume_literal(cursor, "false")) {
        *out = false;
        return true;
    }
    return false;
}

static bool skip_value(JsonCursor *cursor);

static bool skip_array(JsonCursor *cursor) {
    if (!consume_char(cursor, '[')) {
        return false;
    }
    skip_ws(cursor);
    if (consume_char(cursor, ']')) {
        return true;
    }

    for (;;) {
        if (!skip_value(cursor)) {
            return false;
        }
        skip_ws(cursor);
        if (consume_char(cursor, ',')) {
            continue;
        }
        return consume_char(cursor, ']');
    }
}

static bool skip_object(JsonCursor *cursor) {
    if (!consume_char(cursor, '{')) {
        return false;
    }
    skip_ws(cursor);
    if (consume_char(cursor, '}')) {
        return true;
    }

    for (;;) {
        if (!parse_string(cursor, NULL, 0) || !consume_char(cursor, ':') || !skip_value(cursor)) {
            return false;
        }
        skip_ws(cursor);
        if (consume_char(cursor, ',')) {
            continue;
        }
        return consume_char(cursor, '}');
    }
}

static bool skip_value(JsonCursor *cursor) {
    skip_ws(cursor);
    if (cursor->p >= cursor->end) {
        return false;
    }

    switch (*cursor->p) {
        case '"':
            return parse_string(cursor, NULL, 0);
        case '{':
            return skip_object(cursor);
        case '[':
            return skip_array(cursor);
        case 't':
            return consume_literal(cursor, "true");
        case 'f':
            return consume_literal(cursor, "false");
        case 'n':
            return consume_literal(cursor, "null");
        default: {
            char token[64];
            return parse_number_token(cursor, token);
        }
    }
}

static bool finish_object_value(JsonCursor *cursor, bool *done) {
    skip_ws(cursor);
    if (consume_char(cursor, ',')) {
        *done = false;
        return true;
    }
    if (consume_char(cursor, '}')) {
        *done = true;
        return true;
    }
    return false;
}

static bool parse_known_merchants(JsonCursor *cursor, Payload *payload) {
    if (!consume_char(cursor, '[')) {
        return false;
    }
    payload->known_merchants_count = 0;

    skip_ws(cursor);
    if (consume_char(cursor, ']')) {
        return true;
    }

    for (;;) {
        char merchant[RINHA_MAX_MERCHANT_ID];
        if (!parse_string(cursor, merchant, sizeof(merchant))) {
            return false;
        }
        if (payload->known_merchants_count < RINHA_MAX_KNOWN_MERCHANTS) {
            memcpy(
                payload->known_merchants[payload->known_merchants_count],
                merchant,
                sizeof(merchant)
            );
            payload->known_merchants_count++;
        }

        skip_ws(cursor);
        if (consume_char(cursor, ',')) {
            continue;
        }
        return consume_char(cursor, ']');
    }
}

static bool parse_transaction(JsonCursor *cursor, Payload *payload) {
    if (!consume_char(cursor, '{')) {
        return false;
    }
    bool done = false;
    while (!done) {
        char key[64];
        if (!parse_string(cursor, key, sizeof(key)) || !consume_char(cursor, ':')) {
            return false;
        }

        if (strcmp(key, "amount") == 0) {
            if (!parse_float_value(cursor, &payload->transaction_amount)) {
                return false;
            }
        } else if (strcmp(key, "installments") == 0) {
            if (!parse_uint32_value(cursor, &payload->transaction_installments)) {
                return false;
            }
        } else if (strcmp(key, "requested_at") == 0) {
            if (!parse_string(cursor, payload->transaction_requested_at, sizeof(payload->transaction_requested_at))) {
                return false;
            }
            ParsedTimestamp parsed;
            if (!parse_timestamp(payload->transaction_requested_at, &parsed)) {
                return false;
            }
            payload->transaction_requested_seconds = parsed.total_seconds;
            payload->transaction_requested_hour = parsed.hour;
            payload->transaction_requested_weekday = parsed.weekday_monday0;
        } else if (!skip_value(cursor)) {
            return false;
        }

        if (!finish_object_value(cursor, &done)) {
            return false;
        }
    }

    return payload->transaction_requested_at[0] != '\0';
}

static bool parse_customer(JsonCursor *cursor, Payload *payload) {
    if (!consume_char(cursor, '{')) {
        return false;
    }
    bool done = false;
    while (!done) {
        char key[64];
        if (!parse_string(cursor, key, sizeof(key)) || !consume_char(cursor, ':')) {
            return false;
        }

        if (strcmp(key, "avg_amount") == 0) {
            if (!parse_float_value(cursor, &payload->customer_avg_amount)) {
                return false;
            }
        } else if (strcmp(key, "tx_count_24h") == 0) {
            if (!parse_uint32_value(cursor, &payload->customer_tx_count_24h)) {
                return false;
            }
        } else if (strcmp(key, "known_merchants") == 0) {
            if (!parse_known_merchants(cursor, payload)) {
                return false;
            }
        } else if (!skip_value(cursor)) {
            return false;
        }

        if (!finish_object_value(cursor, &done)) {
            return false;
        }
    }

    return true;
}

static bool parse_merchant(JsonCursor *cursor, Payload *payload) {
    if (!consume_char(cursor, '{')) {
        return false;
    }
    bool done = false;
    while (!done) {
        char key[64];
        if (!parse_string(cursor, key, sizeof(key)) || !consume_char(cursor, ':')) {
            return false;
        }

        if (strcmp(key, "id") == 0) {
            if (!parse_string(cursor, payload->merchant_id, sizeof(payload->merchant_id))) {
                return false;
            }
        } else if (strcmp(key, "mcc") == 0) {
            if (!parse_string(cursor, payload->merchant_mcc, sizeof(payload->merchant_mcc))) {
                return false;
            }
            payload->merchant_mcc_risk = mcc_risk(payload->merchant_mcc);
        } else if (strcmp(key, "avg_amount") == 0) {
            if (!parse_float_value(cursor, &payload->merchant_avg_amount)) {
                return false;
            }
        } else if (!skip_value(cursor)) {
            return false;
        }

        if (!finish_object_value(cursor, &done)) {
            return false;
        }
    }

    return payload->merchant_id[0] != '\0' && payload->merchant_mcc[0] != '\0';
}

static bool parse_terminal(JsonCursor *cursor, Payload *payload) {
    if (!consume_char(cursor, '{')) {
        return false;
    }
    bool done = false;
    while (!done) {
        char key[64];
        if (!parse_string(cursor, key, sizeof(key)) || !consume_char(cursor, ':')) {
            return false;
        }

        if (strcmp(key, "is_online") == 0) {
            if (!parse_bool_value(cursor, &payload->terminal_is_online)) {
                return false;
            }
        } else if (strcmp(key, "card_present") == 0) {
            if (!parse_bool_value(cursor, &payload->terminal_card_present)) {
                return false;
            }
        } else if (strcmp(key, "km_from_home") == 0) {
            if (!parse_float_value(cursor, &payload->terminal_km_from_home)) {
                return false;
            }
        } else if (!skip_value(cursor)) {
            return false;
        }

        if (!finish_object_value(cursor, &done)) {
            return false;
        }
    }

    return true;
}

static bool parse_last_transaction(JsonCursor *cursor, Payload *payload) {
    if (consume_literal(cursor, "null")) {
        payload->has_last_transaction = false;
        payload->last_transaction_timestamp[0] = '\0';
        payload->last_transaction_km_from_current = 0.0f;
        return true;
    }

    if (!consume_char(cursor, '{')) {
        return false;
    }

    payload->has_last_transaction = true;
    bool done = false;
    while (!done) {
        char key[64];
        if (!parse_string(cursor, key, sizeof(key)) || !consume_char(cursor, ':')) {
            return false;
        }

        if (strcmp(key, "timestamp") == 0) {
            if (!parse_string(cursor, payload->last_transaction_timestamp, sizeof(payload->last_transaction_timestamp))) {
                return false;
            }
            ParsedTimestamp parsed;
            if (!parse_timestamp(payload->last_transaction_timestamp, &parsed)) {
                return false;
            }
            payload->last_transaction_seconds = parsed.total_seconds;
        } else if (strcmp(key, "km_from_current") == 0) {
            if (!parse_float_value(cursor, &payload->last_transaction_km_from_current)) {
                return false;
            }
        } else if (!skip_value(cursor)) {
            return false;
        }

        if (!finish_object_value(cursor, &done)) {
            return false;
        }
    }

    return payload->last_transaction_timestamp[0] != '\0';
}

bool parse_payload(const char *body, size_t len, Payload *payload) {
    memset(payload, 0, sizeof(*payload));
    JsonCursor cursor = {.p = body, .end = body + len};

    if (!consume_char(&cursor, '{')) {
        return false;
    }

    bool done = false;
    while (!done) {
        char key[64];
        if (!parse_string(&cursor, key, sizeof(key)) || !consume_char(&cursor, ':')) {
            return false;
        }

        if (strcmp(key, "transaction") == 0) {
            if (!parse_transaction(&cursor, payload)) {
                return false;
            }
        } else if (strcmp(key, "customer") == 0) {
            if (!parse_customer(&cursor, payload)) {
                return false;
            }
        } else if (strcmp(key, "merchant") == 0) {
            if (!parse_merchant(&cursor, payload)) {
                return false;
            }
        } else if (strcmp(key, "terminal") == 0) {
            if (!parse_terminal(&cursor, payload)) {
                return false;
            }
        } else if (strcmp(key, "last_transaction") == 0) {
            if (!parse_last_transaction(&cursor, payload)) {
                return false;
            }
        } else if (!skip_value(&cursor)) {
            return false;
        }

        if (!finish_object_value(&cursor, &done)) {
            return false;
        }
    }

    skip_ws(&cursor);
    if (cursor.p != cursor.end) {
        return false;
    }

    if (payload->transaction_requested_at[0] == '\0' || payload->merchant_id[0] == '\0' || payload->merchant_mcc[0] == '\0') {
        return false;
    }

    payload->known_merchant = false;
    for (size_t index = 0; index < payload->known_merchants_count; ++index) {
        if (strcmp(payload->known_merchants[index], payload->merchant_id) == 0) {
            payload->known_merchant = true;
            break;
        }
    }

    return true;
}

static bool parse_digits_fixed(const char *bytes, size_t len, uint32_t *out) {
    uint32_t value = 0;
    for (size_t index = 0; index < len; ++index) {
        const unsigned char ch = (unsigned char)bytes[index];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = (value * 10U) + (uint32_t)(ch - '0');
    }
    *out = value;
    return true;
}

static bool is_leap_year(int32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint8_t days_in_month(int32_t year, uint8_t month) {
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
            return is_leap_year(year) ? 29 : 28;
        default:
            return 0;
    }
}

static int64_t days_from_civil(int32_t year, uint8_t month, uint8_t day) {
    const int32_t adjusted_year = year - (month <= 2);
    const int32_t era = adjusted_year >= 0 ? adjusted_year / 400 : (adjusted_year - 399) / 400;
    const int32_t year_of_era = adjusted_year - (era * 400);
    const int32_t shifted_month = (int32_t)month + (month > 2 ? -3 : 9);
    const int32_t day_of_year = ((153 * shifted_month) + 2) / 5 + (int32_t)day - 1;
    const int32_t day_of_era =
        (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
    return (int64_t)((era * 146097) + day_of_era - 719468);
}

static bool parse_timestamp_fixed(const char *value, ParsedTimestamp *parsed) {
    if (value[4] != '-' ||
        value[7] != '-' ||
        value[10] != 'T' ||
        value[13] != ':' ||
        value[16] != ':' ||
        value[19] != 'Z') {
        return false;
    }

    uint32_t year = 0;
    uint32_t month = 0;
    uint32_t day = 0;
    uint32_t hour = 0;
    uint32_t minute = 0;
    uint32_t second = 0;
    if (!parse_digits_fixed(value, 4, &year) ||
        !parse_digits_fixed(value + 5, 2, &month) ||
        !parse_digits_fixed(value + 8, 2, &day) ||
        !parse_digits_fixed(value + 11, 2, &hour) ||
        !parse_digits_fixed(value + 14, 2, &minute) ||
        !parse_digits_fixed(value + 17, 2, &second)) {
        return false;
    }

    if (month < 1 || month > 12) {
        return false;
    }
    const uint8_t max_day = days_in_month((int32_t)year, (uint8_t)month);
    if (day == 0 || day > max_day || hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    const int64_t days_since_unix_epoch = days_from_civil((int32_t)year, (uint8_t)month, (uint8_t)day);
    parsed->total_seconds =
        (days_since_unix_epoch * 86400) + ((int64_t)hour * 3600) + ((int64_t)minute * 60) + (int64_t)second;
    parsed->hour = (uint8_t)hour;
    int64_t weekday = (days_since_unix_epoch + 3) % 7;
    if (weekday < 0) {
        weekday += 7;
    }
    parsed->weekday_monday0 = (uint8_t)weekday;
    return true;
}

static bool parse_timestamp(const char *value, ParsedTimestamp *parsed) {
    return strlen(value) == 20 && parse_timestamp_fixed(value, parsed);
}

static inline float clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static inline float amount_vs_avg(float amount, float avg_amount) {
    if (avg_amount <= 0.0f) {
        return amount <= 0.0f ? 0.0f : 1.0f;
    }
    return (amount / avg_amount) / 10.0f;
}

static inline float bool_to_f32(bool value) {
    return value ? 1.0f : 0.0f;
}

static float mcc_risk_view(const char *mcc, size_t len) {
    if (len != 4 ||
        mcc[0] < '0' || mcc[0] > '9' ||
        mcc[1] < '0' || mcc[1] > '9' ||
        mcc[2] < '0' || mcc[2] > '9' ||
        mcc[3] < '0' || mcc[3] > '9') {
        return 0.50f;
    }

    const uint32_t code =
        ((uint32_t)(mcc[0] - '0') * 1000U) +
        ((uint32_t)(mcc[1] - '0') * 100U) +
        ((uint32_t)(mcc[2] - '0') * 10U) +
        (uint32_t)(mcc[3] - '0');

    switch (code) {
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

static float mcc_risk(const char *mcc) {
    return mcc_risk_view(mcc, strlen(mcc));
}

bool vectorize_payload(const Payload *payload, float out[RINHA_DIMENSIONS]) {
    float minutes_since_last_tx = -1.0f;
    float km_from_last_tx = -1.0f;
    if (payload->has_last_transaction) {
        int64_t elapsed_minutes = (payload->transaction_requested_seconds - payload->last_transaction_seconds) / 60;
        if (elapsed_minutes < 0) {
            elapsed_minutes = 0;
        }
        minutes_since_last_tx = clamp01((float)elapsed_minutes / 1440.0f);
        km_from_last_tx = clamp01(payload->last_transaction_km_from_current / 1000.0f);
    }

    out[0] = clamp01(payload->transaction_amount / 10000.0f);
    out[1] = clamp01((float)payload->transaction_installments / 12.0f);
    out[2] = clamp01(amount_vs_avg(payload->transaction_amount, payload->customer_avg_amount));
    out[3] = (float)payload->transaction_requested_hour / 23.0f;
    out[4] = (float)payload->transaction_requested_weekday / 6.0f;
    out[5] = minutes_since_last_tx;
    out[6] = km_from_last_tx;
    out[7] = clamp01(payload->terminal_km_from_home / 1000.0f);
    out[8] = clamp01((float)payload->customer_tx_count_24h / 20.0f);
    out[9] = bool_to_f32(payload->terminal_is_online);
    out[10] = bool_to_f32(payload->terminal_card_present);
    out[11] = bool_to_f32(!payload->known_merchant);
    out[12] = payload->merchant_mcc_risk;
    out[13] = clamp01(payload->merchant_avg_amount / 10000.0f);
    return true;
}

static bool refs_reserve(ReferenceSet *refs, size_t capacity, char *error, size_t error_len) {
    if (capacity <= refs->capacity) {
        return true;
    }

    float *new_dims[RINHA_DIMENSIONS] = {0};
    uint8_t *new_labels = NULL;

    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        if (posix_memalign((void **)&new_dims[dim], 32, capacity * sizeof(float)) != 0) {
            set_error(error, error_len, "falha ao alocar referências");
            for (size_t free_dim = 0; free_dim < dim; ++free_dim) {
                free(new_dims[free_dim]);
            }
            return false;
        }
        if (refs->dims[dim] != NULL && refs->rows > 0) {
            memcpy(new_dims[dim], refs->dims[dim], refs->rows * sizeof(float));
        }
    }

    new_labels = (uint8_t *)malloc(capacity);
    if (new_labels == NULL) {
        set_error(error, error_len, "falha ao alocar labels");
        for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
            free(new_dims[dim]);
        }
        return false;
    }
    if (refs->labels != NULL && refs->rows > 0) {
        memcpy(new_labels, refs->labels, refs->rows);
    }

    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        free(refs->dims[dim]);
        refs->dims[dim] = new_dims[dim];
    }
    free(refs->labels);
    refs->labels = new_labels;
    refs->capacity = capacity;
    return true;
}

void refs_free(ReferenceSet *refs) {
    if (refs == NULL) {
        return;
    }
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        free(refs->grouped_dims[dim]);
        refs->grouped_dims[dim] = NULL;
        refs->grouped_ordered_dims[dim] = NULL;
        free(refs->dims[dim]);
        refs->dims[dim] = NULL;
        refs->ordered_dims[dim] = NULL;
    }
    free(refs->grouped_labels);
    refs->grouped_labels = NULL;
    free(refs->grouped_original_indices);
    refs->grouped_original_indices = NULL;
    free(refs->labels);
    refs->labels = NULL;
    refs->rows = 0;
    refs->capacity = 0;
    for (size_t group = 0; group <= RINHA_GROUP_COUNT; ++group) {
        refs->group_starts[group] = 0;
    }
    refs->grouped_ready = false;
}

static bool refs_append(
    ReferenceSet *refs,
    const float vector[RINHA_DIMENSIONS],
    uint8_t label,
    char *error,
    size_t error_len
) {
    if (refs->rows == refs->capacity) {
        const size_t next_capacity = refs->capacity == 0 ? 131072 : refs->capacity * 2;
        if (!refs_reserve(refs, next_capacity, error, error_len)) {
            return false;
        }
    }

    const size_t row = refs->rows;
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        refs->dims[dim][row] = vector[dim];
    }
    refs->labels[row] = label;
    refs->rows++;
    return true;
}

static void refs_clear_grouped(ReferenceSet *refs) {
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        free(refs->grouped_dims[dim]);
        refs->grouped_dims[dim] = NULL;
        refs->grouped_ordered_dims[dim] = NULL;
    }
    free(refs->grouped_labels);
    refs->grouped_labels = NULL;
    free(refs->grouped_original_indices);
    refs->grouped_original_indices = NULL;
    for (size_t group = 0; group <= RINHA_GROUP_COUNT; ++group) {
        refs->group_starts[group] = 0;
    }
    refs->grouped_ready = false;
}

static inline uint8_t reference_group_key(const ReferenceSet *refs, size_t row) {
    const bool no_last_transaction = refs->dims[5][row] < 0.0f && refs->dims[6][row] < 0.0f;
    const bool is_online = refs->dims[9][row] >= 0.5f;
    const bool card_present = refs->dims[10][row] >= 0.5f;
    const bool unknown_merchant = refs->dims[11][row] >= 0.5f;
    return (uint8_t)(
        (no_last_transaction ? 1U : 0U) |
        (is_online ? 2U : 0U) |
        (card_present ? 4U : 0U) |
        (unknown_merchant ? 8U : 0U)
    );
}

static bool refs_build_groups(ReferenceSet *refs) {
    refs_clear_grouped(refs);
    if (refs->rows == 0 || refs->rows > UINT32_MAX) {
        return false;
    }

    size_t counts[RINHA_GROUP_COUNT] = {0};
    for (size_t row = 0; row < refs->rows; ++row) {
        counts[reference_group_key(refs, row)]++;
    }

    refs->group_starts[0] = 0;
    for (size_t group = 0; group < RINHA_GROUP_COUNT; ++group) {
        refs->group_starts[group + 1] = refs->group_starts[group] + counts[group];
    }

    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        if (posix_memalign((void **)&refs->grouped_dims[dim], 32, refs->rows * sizeof(float)) != 0) {
            refs_clear_grouped(refs);
            return false;
        }
    }

    refs->grouped_labels = (uint8_t *)malloc(refs->rows);
    refs->grouped_original_indices = (uint32_t *)malloc(refs->rows * sizeof(uint32_t));
    if (refs->grouped_labels == NULL || refs->grouped_original_indices == NULL) {
        refs_clear_grouped(refs);
        return false;
    }

    size_t positions[RINHA_GROUP_COUNT];
    memcpy(positions, refs->group_starts, sizeof(positions));
    for (size_t row = 0; row < refs->rows; ++row) {
        const uint8_t group = reference_group_key(refs, row);
        const size_t position = positions[group]++;
        for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
            refs->grouped_dims[dim][position] = refs->dims[dim][row];
        }
        refs->grouped_labels[position] = refs->labels[row];
        refs->grouped_original_indices[position] = (uint32_t)row;
    }

    for (size_t index = 0; index < RINHA_DIMENSIONS; ++index) {
        refs->grouped_ordered_dims[index] = refs->grouped_dims[refs->dimension_order[index]];
    }
    refs->grouped_ready = true;
    return true;
}

static void refs_compute_dimension_order(ReferenceSet *refs) {
    float variances[RINHA_DIMENSIONS] = {0};
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        refs->dimension_order[dim] = (uint8_t)dim;
        refs->ordered_dims[dim] = refs->dims[dim];
    }
    if (refs->rows == 0) {
        return;
    }

    const double row_count = (double)refs->rows;
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        double sum = 0.0;
        double squared_sum = 0.0;
        const float *values = refs->dims[dim];
        for (size_t row = 0; row < refs->rows; ++row) {
            const double value = (double)values[row];
            sum += value;
            squared_sum += value * value;
        }
        const double mean = sum / row_count;
        variances[dim] = (float)((squared_sum / row_count) - (mean * mean));
    }

    for (size_t left = 0; left + 1 < RINHA_DIMENSIONS; ++left) {
        size_t best = left;
        for (size_t right = left + 1; right < RINHA_DIMENSIONS; ++right) {
            if (variances[refs->dimension_order[right]] > variances[refs->dimension_order[best]]) {
                best = right;
            }
        }
        if (best != left) {
            const uint8_t tmp = refs->dimension_order[left];
            refs->dimension_order[left] = refs->dimension_order[best];
            refs->dimension_order[best] = tmp;
        }
    }

    for (size_t index = 0; index < RINHA_DIMENSIONS; ++index) {
        refs->ordered_dims[index] = refs->dims[refs->dimension_order[index]];
    }
    (void)refs_build_groups(refs);
}

static bool decompress_gzip_file(const char *path, char **out, size_t *out_len, char *error, size_t error_len) {
    gzFile file = gzopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_len, "falha ao abrir referências gzip");
        return false;
    }

    size_t capacity = 1 << 20;
    size_t len = 0;
    char *buffer = (char *)malloc(capacity + 1);
    if (buffer == NULL) {
        gzclose(file);
        set_error(error, error_len, "falha ao alocar gzip");
        return false;
    }

    for (;;) {
        if (capacity - len < (1 << 15)) {
            capacity *= 2;
            char *next = (char *)realloc(buffer, capacity + 1);
            if (next == NULL) {
                free(buffer);
                gzclose(file);
                set_error(error, error_len, "falha ao expandir gzip");
                return false;
            }
            buffer = next;
        }

        const int read_bytes = gzread(file, buffer + len, 1 << 15);
        if (read_bytes > 0) {
            len += (size_t)read_bytes;
            continue;
        }
        if (read_bytes == 0) {
            break;
        }

        int zlib_error = Z_OK;
        const char *message = gzerror(file, &zlib_error);
        (void)zlib_error;
        snprintf(error, error_len, "falha ao descompactar gzip: %s", message == NULL ? "erro desconhecido" : message);
        free(buffer);
        gzclose(file);
        return false;
    }

    gzclose(file);
    buffer[len] = '\0';
    *out = buffer;
    *out_len = len;
    return true;
}

static bool parse_reference_vector(JsonCursor *cursor, float vector[RINHA_DIMENSIONS]) {
    if (!consume_char(cursor, '[')) {
        return false;
    }
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        if (!parse_float_value(cursor, &vector[dim])) {
            return false;
        }
        if (dim + 1 < RINHA_DIMENSIONS) {
            if (!consume_char(cursor, ',')) {
                return false;
            }
        }
    }
    return consume_char(cursor, ']');
}

static bool parse_reference_entry(
    JsonCursor *cursor,
    float vector[RINHA_DIMENSIONS],
    uint8_t *label,
    char *error,
    size_t error_len
) {
    if (!consume_char(cursor, '{')) {
        return false;
    }

    bool seen_vector = false;
    bool seen_label = false;
    bool done = false;
    while (!done) {
        char key[64];
        if (!parse_string(cursor, key, sizeof(key)) || !consume_char(cursor, ':')) {
            return false;
        }

        if (strcmp(key, "vector") == 0) {
            if (!parse_reference_vector(cursor, vector)) {
                set_error(error, error_len, "vetor de referência inválido");
                return false;
            }
            seen_vector = true;
        } else if (strcmp(key, "label") == 0) {
            char label_value[16];
            if (!parse_string(cursor, label_value, sizeof(label_value))) {
                return false;
            }
            if (strcmp(label_value, "fraud") == 0) {
                *label = 1;
            } else if (strcmp(label_value, "legit") == 0) {
                *label = 0;
            } else {
                set_error(error, error_len, "label de referência inválida");
                return false;
            }
            seen_label = true;
        } else if (!skip_value(cursor)) {
            return false;
        }

        if (!finish_object_value(cursor, &done)) {
            return false;
        }
    }

    return seen_vector && seen_label;
}

bool refs_load_gzip_json(const char *path, ReferenceSet *refs, char *error, size_t error_len) {
    memset(refs, 0, sizeof(*refs));

    char *json = NULL;
    size_t json_len = 0;
    if (!decompress_gzip_file(path, &json, &json_len, error, error_len)) {
        return false;
    }

    JsonCursor cursor = {.p = json, .end = json + json_len};
    bool ok = consume_char(&cursor, '[');
    if (ok) {
        skip_ws(&cursor);
        if (consume_char(&cursor, ']')) {
            ok = false;
            set_error(error, error_len, "conjunto de referências vazio");
        }
    }

    while (ok) {
        float vector[RINHA_DIMENSIONS];
        uint8_t label = 0;
        if (!parse_reference_entry(&cursor, vector, &label, error, error_len) ||
            !refs_append(refs, vector, label, error, error_len)) {
            ok = false;
            break;
        }

        skip_ws(&cursor);
        if (consume_char(&cursor, ',')) {
            continue;
        }
        ok = consume_char(&cursor, ']');
        break;
    }

    if (ok) {
        skip_ws(&cursor);
        ok = cursor.p == cursor.end;
    }

    free(json);
    if (!ok) {
        if (error != NULL && error_len > 0 && error[0] == '\0') {
            set_error(error, error_len, "falha ao decodificar referências");
        }
        refs_free(refs);
        return false;
    }

    refs_compute_dimension_order(refs);
    return true;
}

static bool file_size(const char *path, size_t *size, char *error, size_t error_len) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) {
        set_error(error, error_len, "falha ao medir arquivo");
        return false;
    }
    *size = (size_t)st.st_size;
    return true;
}

bool refs_load_binary(const char *references_path, const char *labels_path, ReferenceSet *refs, char *error, size_t error_len) {
    memset(refs, 0, sizeof(*refs));

    size_t labels_size = 0;
    size_t refs_size = 0;
    if (!file_size(labels_path, &labels_size, error, error_len) ||
        !file_size(references_path, &refs_size, error, error_len)) {
        return false;
    }
    if (labels_size == 0) {
        set_error(error, error_len, "labels.bin está vazio");
        return false;
    }

    const size_t expected_refs_size = labels_size * RINHA_DIMENSIONS * sizeof(float);
    if (refs_size != expected_refs_size) {
        set_error(error, error_len, "references.bin possui tamanho inválido");
        return false;
    }

    if (!refs_reserve(refs, labels_size, error, error_len)) {
        return false;
    }

    FILE *refs_file = fopen(references_path, "rb");
    if (refs_file == NULL) {
        refs_free(refs);
        set_error(error, error_len, "falha ao abrir references.bin");
        return false;
    }
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        if (fread(refs->dims[dim], sizeof(float), labels_size, refs_file) != labels_size) {
            fclose(refs_file);
            refs_free(refs);
            set_error(error, error_len, "falha ao ler references.bin");
            return false;
        }
    }
    fclose(refs_file);

    FILE *labels_file = fopen(labels_path, "rb");
    if (labels_file == NULL) {
        refs_free(refs);
        set_error(error, error_len, "falha ao abrir labels.bin");
        return false;
    }
    if (fread(refs->labels, 1, labels_size, labels_file) != labels_size) {
        fclose(labels_file);
        refs_free(refs);
        set_error(error, error_len, "falha ao ler labels.bin");
        return false;
    }
    fclose(labels_file);

    refs->rows = labels_size;
    refs_compute_dimension_order(refs);
    return true;
}

bool refs_write_binary(const ReferenceSet *refs, const char *references_path, const char *labels_path, char *error, size_t error_len) {
    FILE *refs_file = fopen(references_path, "wb");
    if (refs_file == NULL) {
        set_error(error, error_len, "falha ao criar references.bin");
        return false;
    }
    for (size_t dim = 0; dim < RINHA_DIMENSIONS; ++dim) {
        if (fwrite(refs->dims[dim], sizeof(float), refs->rows, refs_file) != refs->rows) {
            fclose(refs_file);
            set_error(error, error_len, "falha ao escrever references.bin");
            return false;
        }
    }
    fclose(refs_file);

    FILE *labels_file = fopen(labels_path, "wb");
    if (labels_file == NULL) {
        set_error(error, error_len, "falha ao criar labels.bin");
        return false;
    }
    if (fwrite(refs->labels, 1, refs->rows, labels_file) != refs->rows) {
        fclose(labels_file);
        set_error(error, error_len, "falha ao escrever labels.bin");
        return false;
    }
    fclose(labels_file);
    return true;
}

static inline void insert_top5(
    float top_dist[5],
    uint8_t top_label[5],
    size_t top_index[5],
    float distance,
    uint8_t label,
    size_t row_index
) {
    if (distance > top_dist[4] || (distance == top_dist[4] && row_index >= top_index[4])) {
        return;
    }

    size_t position = 4;
    while (position > 0 &&
           (top_dist[position - 1] > distance ||
            (top_dist[position - 1] == distance && top_index[position - 1] > row_index))) {
        top_dist[position] = top_dist[position - 1];
        top_label[position] = top_label[position - 1];
        top_index[position] = top_index[position - 1];
        position--;
    }
    top_dist[position] = distance;
    top_label[position] = label;
    top_index[position] = row_index;
}

static inline uint8_t query_group_key(const float query[RINHA_DIMENSIONS]) {
    const bool no_last_transaction = query[5] < 0.0f && query[6] < 0.0f;
    const bool is_online = query[9] >= 0.5f;
    const bool card_present = query[10] >= 0.5f;
    const bool unknown_merchant = query[11] >= 0.5f;
    return (uint8_t)(
        (no_last_transaction ? 1U : 0U) |
        (is_online ? 2U : 0U) |
        (card_present ? 4U : 0U) |
        (unknown_merchant ? 8U : 0U)
    );
}

static inline float group_lower_bound(uint8_t group, const float query[RINHA_DIMENSIONS]) {
    float lower_bound = 0.0f;

    const bool query_without_last = query[5] < 0.0f && query[6] < 0.0f;
    const bool group_without_last = (group & 1U) != 0U;
    if (query_without_last != group_without_last) {
        if (query_without_last) {
            lower_bound += 2.0f;
        } else {
            const float d5 = query[5] + 1.0f;
            const float d6 = query[6] + 1.0f;
            lower_bound += (d5 * d5) + (d6 * d6);
        }
    }

    const uint8_t query_key = query_group_key(query);
    if (((query_key ^ group) & 2U) != 0U) lower_bound += 1.0f;
    if (((query_key ^ group) & 4U) != 0U) lower_bound += 1.0f;
    if (((query_key ^ group) & 8U) != 0U) lower_bound += 1.0f;
    return lower_bound;
}

static bool classify_top5_avx2_grouped(
    const ReferenceSet *refs,
    const float query[RINHA_DIMENSIONS],
    Classification *classification
) {
    float top_dist[5] = {INFINITY, INFINITY, INFINITY, INFINITY, INFINITY};
    uint8_t top_label[5] = {0, 0, 0, 0, 0};
    size_t top_index[5] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};

    __m256 query_lanes[RINHA_DIMENSIONS];
    for (size_t order_index = 0; order_index < RINHA_DIMENSIONS; ++order_index) {
        query_lanes[order_index] = _mm256_set1_ps(query[refs->dimension_order[order_index]]);
    }

    uint8_t groups[RINHA_GROUP_COUNT];
    float group_bounds[RINHA_GROUP_COUNT];
    size_t group_count = 0;
    for (uint8_t group = 0; group < RINHA_GROUP_COUNT; ++group) {
        if (refs->group_starts[group] == refs->group_starts[group + 1]) {
            continue;
        }

        const float lower_bound = group_lower_bound(group, query);
        size_t position = group_count;
        while (position > 0 && group_bounds[position - 1] > lower_bound) {
            group_bounds[position] = group_bounds[position - 1];
            groups[position] = groups[position - 1];
            position--;
        }
        group_bounds[position] = lower_bound;
        groups[position] = group;
        group_count++;
    }

    for (size_t group_index = 0; group_index < group_count; ++group_index) {
        if (top_dist[4] != INFINITY && group_bounds[group_index] > top_dist[4]) {
            break;
        }

        const uint8_t group = groups[group_index];
        const size_t end = refs->group_starts[group + 1];
        size_t offset = refs->group_starts[group];
        for (; offset + 8U <= end; offset += 8U) {
            __m256 accum = _mm256_setzero_ps();
            const float threshold = top_dist[4];
            const __m256 threshold_lanes = _mm256_set1_ps(threshold);
            const bool can_prune = threshold != INFINITY;
            bool pruned = false;

#define RINHA_GROUPED_AVX2_STEP(order_index) do { \
                const __m256 refs_lane = _mm256_loadu_ps(refs->grouped_ordered_dims[(order_index)] + offset); \
                const __m256 diff = _mm256_sub_ps(query_lanes[(order_index)], refs_lane); \
                accum = _mm256_fmadd_ps(diff, diff, accum); \
                if (can_prune) { \
                    const __m256 cmp = _mm256_cmp_ps(accum, threshold_lanes, _CMP_GE_OQ); \
                    if (_mm256_movemask_ps(cmp) == 0xFF) { \
                        pruned = true; \
                        goto rinha_grouped_avx2_chunk_done; \
                    } \
                } \
            } while (0)

            RINHA_GROUPED_AVX2_STEP(0);
            RINHA_GROUPED_AVX2_STEP(1);
            RINHA_GROUPED_AVX2_STEP(2);
            RINHA_GROUPED_AVX2_STEP(3);
            RINHA_GROUPED_AVX2_STEP(4);
            RINHA_GROUPED_AVX2_STEP(5);
            RINHA_GROUPED_AVX2_STEP(6);
            RINHA_GROUPED_AVX2_STEP(7);
            RINHA_GROUPED_AVX2_STEP(8);
            RINHA_GROUPED_AVX2_STEP(9);
            RINHA_GROUPED_AVX2_STEP(10);
            RINHA_GROUPED_AVX2_STEP(11);
            RINHA_GROUPED_AVX2_STEP(12);
            RINHA_GROUPED_AVX2_STEP(13);

rinha_grouped_avx2_chunk_done:
#undef RINHA_GROUPED_AVX2_STEP

            if (pruned) {
                continue;
            }

            float distances[8] __attribute__((aligned(32)));
            _mm256_store_ps(distances, accum);
            for (size_t lane = 0; lane < 8U; ++lane) {
                insert_top5(
                    top_dist,
                    top_label,
                    top_index,
                    distances[lane],
                    refs->grouped_labels[offset + lane],
                    refs->grouped_original_indices[offset + lane]
                );
            }
        }

        for (; offset < end; ++offset) {
            float sum = 0.0f;
            bool pruned = false;
            for (size_t order_index = 0; order_index < RINHA_DIMENSIONS; ++order_index) {
                const uint8_t dim = refs->dimension_order[order_index];
                const float delta = query[dim] - refs->grouped_dims[dim][offset];
                sum = fmaf(delta, delta, sum);
                if (sum >= top_dist[4]) {
                    pruned = true;
                    break;
                }
            }
            if (!pruned) {
                insert_top5(
                    top_dist,
                    top_label,
                    top_index,
                    sum,
                    refs->grouped_labels[offset],
                    refs->grouped_original_indices[offset]
                );
            }
        }
    }

    uint8_t fraud_count = 0;
    for (size_t index = 0; index < 5; ++index) {
        fraud_count += top_label[index] != 0;
    }
    classification->fraud_count = fraud_count;
    classification->approved = fraud_count < 3;
    return true;
}

static bool classify_top5_avx2(const ReferenceSet *refs, const float query[RINHA_DIMENSIONS], Classification *classification) {
    if (refs->grouped_ready) {
        return classify_top5_avx2_grouped(refs, query, classification);
    }

    float top_dist[5] = {INFINITY, INFINITY, INFINITY, INFINITY, INFINITY};
    uint8_t top_label[5] = {0, 0, 0, 0, 0};
    size_t top_index[5] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};

    __m256 query_lanes[RINHA_DIMENSIONS];
    for (size_t order_index = 0; order_index < RINHA_DIMENSIONS; ++order_index) {
        query_lanes[order_index] = _mm256_set1_ps(query[refs->dimension_order[order_index]]);
    }

    const size_t chunks = refs->rows / 8;
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
        const size_t offset = chunk * 8;
        __m256 accum = _mm256_setzero_ps();
        const float threshold = top_dist[4];
        const __m256 threshold_lanes = _mm256_set1_ps(threshold);
        const bool can_prune = threshold != INFINITY;
        bool pruned = false;

#define RINHA_AVX2_STEP(order_index) do { \
            const __m256 refs_lane = _mm256_load_ps(refs->ordered_dims[(order_index)] + offset); \
            const __m256 diff = _mm256_sub_ps(query_lanes[(order_index)], refs_lane); \
            accum = _mm256_fmadd_ps(diff, diff, accum); \
            if (can_prune) { \
                const __m256 cmp = _mm256_cmp_ps(accum, threshold_lanes, _CMP_GE_OQ); \
                if (_mm256_movemask_ps(cmp) == 0xFF) { \
                    pruned = true; \
                    goto rinha_avx2_chunk_done; \
                } \
            } \
        } while (0)

        RINHA_AVX2_STEP(0);
        RINHA_AVX2_STEP(1);
        RINHA_AVX2_STEP(2);
        RINHA_AVX2_STEP(3);
        RINHA_AVX2_STEP(4);
        RINHA_AVX2_STEP(5);
        RINHA_AVX2_STEP(6);
        RINHA_AVX2_STEP(7);
        RINHA_AVX2_STEP(8);
        RINHA_AVX2_STEP(9);
        RINHA_AVX2_STEP(10);
        RINHA_AVX2_STEP(11);
        RINHA_AVX2_STEP(12);
        RINHA_AVX2_STEP(13);

rinha_avx2_chunk_done:
#undef RINHA_AVX2_STEP

        if (pruned) {
            continue;
        }

        float distances[8] __attribute__((aligned(32)));
        _mm256_store_ps(distances, accum);
        for (size_t lane = 0; lane < 8; ++lane) {
            insert_top5(top_dist, top_label, top_index, distances[lane], refs->labels[offset + lane], offset + lane);
        }
    }

    for (size_t row = chunks * 8; row < refs->rows; ++row) {
        float sum = 0.0f;
        bool pruned = false;
        for (size_t order_index = 0; order_index < RINHA_DIMENSIONS; ++order_index) {
            const uint8_t dim = refs->dimension_order[order_index];
            const float delta = query[dim] - refs->dims[dim][row];
            sum = fmaf(delta, delta, sum);
            if (sum >= top_dist[4]) {
                pruned = true;
                break;
            }
        }
        if (!pruned) {
            insert_top5(top_dist, top_label, top_index, sum, refs->labels[row], row);
        }
    }

    uint8_t fraud_count = 0;
    for (size_t index = 0; index < 5; ++index) {
        fraud_count += top_label[index] != 0;
    }
    classification->fraud_count = fraud_count;
    classification->approved = fraud_count < 3;
    return true;
}

static bool classify_top5_scalar(const ReferenceSet *refs, const float query[RINHA_DIMENSIONS], Classification *classification) {
    float top_dist[5] = {INFINITY, INFINITY, INFINITY, INFINITY, INFINITY};
    uint8_t top_label[5] = {0, 0, 0, 0, 0};
    size_t top_index[5] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};

    for (size_t row = 0; row < refs->rows; ++row) {
        float sum = 0.0f;
        bool pruned = false;
        for (size_t order_index = 0; order_index < RINHA_DIMENSIONS; ++order_index) {
            const uint8_t dim = refs->dimension_order[order_index];
            const float delta = query[dim] - refs->dims[dim][row];
            sum = fmaf(delta, delta, sum);
            if (sum >= top_dist[4]) {
                pruned = true;
                break;
            }
        }
        if (!pruned) {
            insert_top5(top_dist, top_label, top_index, sum, refs->labels[row], row);
        }
    }

    uint8_t fraud_count = 0;
    for (size_t index = 0; index < 5; ++index) {
        fraud_count += top_label[index] != 0;
    }
    classification->fraud_count = fraud_count;
    classification->approved = fraud_count < 3;
    return true;
}

bool classifier_classify(const ReferenceSet *refs, const Payload *payload, Classification *classification) {
    if (refs == NULL || refs->rows < 5) {
        return false;
    }

    float query[RINHA_DIMENSIONS];
    if (!vectorize_payload(payload, query)) {
        return false;
    }

    return classify_top5_avx2(refs, query, classification);
}

bool classifier_classify_scalar(const ReferenceSet *refs, const Payload *payload, Classification *classification) {
    if (refs == NULL || refs->rows < 5) {
        return false;
    }

    float query[RINHA_DIMENSIONS];
    if (!vectorize_payload(payload, query)) {
        return false;
    }

    return classify_top5_scalar(refs, query, classification);
}

const char *classification_json(const Classification *classification, size_t *len) {
    static const char *responses[6] = {
        "{\"approved\":true,\"fraud_score\":0.0}",
        "{\"approved\":true,\"fraud_score\":0.2}",
        "{\"approved\":true,\"fraud_score\":0.4}",
        "{\"approved\":false,\"fraud_score\":0.6}",
        "{\"approved\":false,\"fraud_score\":0.8}",
        "{\"approved\":false,\"fraud_score\":1.0}",
    };
    static const size_t lengths[6] = {35, 35, 35, 36, 36, 36};

    uint8_t bucket = classification->fraud_count;
    if (bucket > 5) {
        bucket = 0;
    }
    if (len != NULL) {
        *len = lengths[bucket];
    }
    return responses[bucket];
}
