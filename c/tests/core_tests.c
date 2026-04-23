#include "rinha.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RINHA_REPO_ROOT
#define RINHA_REPO_ROOT "."
#endif

static void require_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void require_close(float actual, float expected, float tolerance, const char *message) {
    if (fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "FAIL: %s: actual=%f expected=%f\n", message, actual, expected);
        exit(1);
    }
}

static void test_parse_and_vectorize_null_last_transaction(void) {
    const char *json =
        "{\"id\":\"tx-1329056812\",\"transaction\":{\"amount\":41.12,\"installments\":2,\"requested_at\":\"2026-03-11T18:45:53Z\"},"
        "\"customer\":{\"avg_amount\":82.24,\"tx_count_24h\":3,\"known_merchants\":[\"MERC-003\",\"MERC-016\"]},"
        "\"merchant\":{\"id\":\"MERC-016\",\"mcc\":\"5411\",\"avg_amount\":60.25},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":29.2331036248},"
        "\"last_transaction\":null}";

    Payload payload;
    require_true(parse_payload(json, strlen(json), &payload), "parse payload with null last_transaction");
    require_true(payload.known_merchant, "known merchant is detected");

    float vector[RINHA_DIMENSIONS];
    require_true(vectorize_payload(&payload, vector), "vectorize payload with null last_transaction");
    require_close(vector[0], 0.004112f, 0.00001f, "amount");
    require_close(vector[1], 0.1666667f, 0.0001f, "installments");
    require_close(vector[3], 18.0f / 23.0f, 0.0001f, "hour");
    require_close(vector[4], 2.0f / 6.0f, 0.0001f, "weekday");
    require_close(vector[5], -1.0f, 0.0f, "minutes without last_transaction");
    require_close(vector[6], -1.0f, 0.0f, "km without last_transaction");
    require_close(vector[11], 0.0f, 0.0f, "known merchant vector flag");
    require_close(vector[12], 0.15f, 0.0f, "mcc risk");
}

static void test_vectorize_last_transaction_across_midnight(void) {
    const char *json =
        "{\"transaction\":{\"amount\":100.0,\"installments\":1,\"requested_at\":\"2026-03-11T00:02:00Z\"},"
        "\"customer\":{\"avg_amount\":100.0,\"tx_count_24h\":0,\"known_merchants\":[\"merchant-1\"]},"
        "\"merchant\":{\"id\":\"merchant-1\",\"mcc\":\"5411\",\"avg_amount\":100.0},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":0.0},"
        "\"last_transaction\":{\"timestamp\":\"2026-03-10T23:58:30Z\",\"km_from_current\":25.0}}";

    Payload payload;
    require_true(parse_payload(json, strlen(json), &payload), "parse payload across midnight");

    float vector[RINHA_DIMENSIONS];
    require_true(vectorize_payload(&payload, vector), "vectorize payload across midnight");
    require_close(vector[3], 0.0f, 0.0001f, "hour across midnight");
    require_close(vector[4], 2.0f / 6.0f, 0.0001f, "weekday across midnight");
    require_close(vector[5], 3.0f / 1440.0f, 0.0001f, "minutes across midnight");
    require_close(vector[6], 0.025f, 0.0001f, "km across midnight");
}

static void test_reference_loader_and_classifier_smoke(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/resources/references.json.gz", RINHA_REPO_ROOT);

    char error[256] = {0};
    ReferenceSet refs;
    require_true(refs_load_gzip_json(path, &refs, error, sizeof(error)), error[0] == '\0' ? "load references" : error);
    require_true(refs.rows >= 5, "reference set has at least 5 rows");

    const char *json =
        "{\"id\":\"tx-1329056812\",\"transaction\":{\"amount\":41.12,\"installments\":2,\"requested_at\":\"2026-03-11T18:45:53Z\"},"
        "\"customer\":{\"avg_amount\":82.24,\"tx_count_24h\":3,\"known_merchants\":[\"MERC-003\",\"MERC-016\"]},"
        "\"merchant\":{\"id\":\"MERC-016\",\"mcc\":\"5411\",\"avg_amount\":60.25},"
        "\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":29.2331036248},"
        "\"last_transaction\":null}";
    Payload payload;
    Classification classification = {0};
    require_true(parse_payload(json, strlen(json), &payload), "parse classifier payload");
    require_true(classifier_classify(&refs, &payload, &classification), "classify payload");
    require_true(classification.approved, "first example is approved");

    refs_free(&refs);
}

int main(void) {
    test_parse_and_vectorize_null_last_transaction();
    test_vectorize_last_transaction_across_midnight();
    test_reference_loader_and_classifier_smoke();
    puts("core_tests: ok");
    return 0;
}
