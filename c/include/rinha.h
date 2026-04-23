#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RINHA_DIMENSIONS 14
#define RINHA_MAX_KNOWN_MERCHANTS 16
#define RINHA_MAX_MERCHANT_ID 64
#define RINHA_MAX_TIMESTAMP 21

typedef struct {
    float transaction_amount;
    uint32_t transaction_installments;
    char transaction_requested_at[RINHA_MAX_TIMESTAMP];

    float customer_avg_amount;
    uint32_t customer_tx_count_24h;
    char known_merchants[RINHA_MAX_KNOWN_MERCHANTS][RINHA_MAX_MERCHANT_ID];
    size_t known_merchants_count;

    char merchant_id[RINHA_MAX_MERCHANT_ID];
    char merchant_mcc[8];
    float merchant_avg_amount;

    bool terminal_is_online;
    bool terminal_card_present;
    float terminal_km_from_home;

    bool has_last_transaction;
    char last_transaction_timestamp[RINHA_MAX_TIMESTAMP];
    float last_transaction_km_from_current;

    bool known_merchant;
} Payload;

typedef struct {
    bool approved;
    uint8_t fraud_count;
} Classification;

typedef struct {
    size_t rows;
    size_t capacity;
    float *dims[RINHA_DIMENSIONS];
    uint8_t *labels;
    uint8_t dimension_order[RINHA_DIMENSIONS];
} ReferenceSet;

bool parse_payload(const char *body, size_t len, Payload *payload);
bool vectorize_payload(const Payload *payload, float out[RINHA_DIMENSIONS]);

bool refs_load_gzip_json(const char *path, ReferenceSet *refs, char *error, size_t error_len);
bool refs_load_binary(const char *references_path, const char *labels_path, ReferenceSet *refs, char *error, size_t error_len);
bool refs_write_binary(const ReferenceSet *refs, const char *references_path, const char *labels_path, char *error, size_t error_len);
void refs_free(ReferenceSet *refs);

bool classifier_classify(const ReferenceSet *refs, const Payload *payload, Classification *classification);
bool classifier_classify_scalar(const ReferenceSet *refs, const Payload *payload, Classification *classification);
const char *classification_json(const Classification *classification, size_t *len);
