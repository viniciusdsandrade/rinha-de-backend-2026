#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "rinha/classifier.hpp"
#include "rinha/request.hpp"
#include "rinha/refs.hpp"
#include "rinha/vectorize.hpp"

namespace {

using rinha::Classification;
using rinha::Classifier;
using rinha::Payload;
using rinha::QueryVector;
using rinha::ReferenceSet;

constexpr std::string_view kLegitPayload =
    R"({"id":"tx-1329056812","transaction":{"amount":41.12,"installments":2,"requested_at":"2026-03-11T18:45:53Z"},"customer":{"avg_amount":82.24,"tx_count_24h":3,"known_merchants":["MERC-003","MERC-016"]},"merchant":{"id":"MERC-016","mcc":"5411","avg_amount":60.25},"terminal":{"is_online":false,"card_present":true,"km_from_home":29.2331036248},"last_transaction":null})";

constexpr std::string_view kFraudPayload =
    R"({"id":"tx-1788243118","transaction":{"amount":4368.82,"installments":8,"requested_at":"2026-03-17T02:04:06Z"},"customer":{"avg_amount":68.88,"tx_count_24h":18,"known_merchants":["MERC-004","MERC-004","MERC-015","MERC-017","MERC-007"]},"merchant":{"id":"MERC-062","mcc":"7801","avg_amount":25.55},"terminal":{"is_online":true,"card_present":false,"km_from_home":881.6139684714},"last_transaction":{"timestamp":"2026-03-17T01:58:06Z","km_from_current":660.9200962961}})";

constexpr std::string_view kUnknownMerchantPayload =
    R"({"id":"tx-edge-unknown","transaction":{"amount":15000.0,"installments":20,"requested_at":"2024-02-29T23:59:59Z"},"customer":{"avg_amount":0.0,"tx_count_24h":30,"known_merchants":[]},"merchant":{"id":"MERC-999","mcc":"0000","avg_amount":20000.0},"terminal":{"is_online":true,"card_present":false,"km_from_home":1500.0},"last_transaction":null})";

constexpr std::string_view kDuplicateKnownMerchantPayload =
    R"({"id":"tx-edge-duplicate","transaction":{"amount":100.0,"installments":1,"requested_at":"2026-03-11T12:00:00Z"},"customer":{"avg_amount":100.0,"tx_count_24h":1,"known_merchants":["MERC-DUP","MERC-DUP"]},"merchant":{"id":"MERC-DUP","mcc":"7802","avg_amount":100.0},"terminal":{"is_online":false,"card_present":true,"km_from_home":10.0},"last_transaction":null})";

[[noreturn]] void fail(const std::string& message) {
    std::cerr << message << '\n';
    std::exit(1);
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void expect_near(float actual, float expected, float epsilon, const std::string& message) {
    if (std::fabs(actual - expected) > epsilon) {
        fail(message + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
    }
}

void test_parse_payload_handles_known_merchant_and_null_last_transaction() {
    Payload payload{};
    std::string error;
    expect(rinha::parse_payload(kLegitPayload, payload, error), "parse do payload legit falhou: " + error);
    expect(payload.known_merchant, "merchant conhecido deveria ser detectado");
    expect(!payload.has_last_transaction, "last_transaction null deveria ser preservado");
    expect(payload.transaction_requested_at == "2026-03-11T18:45:53Z", "requested_at incorreto");
    expect(payload.merchant_mcc == "5411", "mcc incorreto");
}

void test_vectorize_handles_midnight_gap() {
    Payload payload{};
    payload.transaction_amount = 100.0f;
    payload.transaction_installments = 1U;
    payload.transaction_requested_at = "2026-03-11T00:02:00Z";
    payload.customer_avg_amount = 100.0f;
    payload.customer_tx_count_24h = 0U;
    payload.known_merchant = true;
    payload.merchant_mcc = "5411";
    payload.merchant_avg_amount = 100.0f;
    payload.terminal_is_online = false;
    payload.terminal_card_present = true;
    payload.terminal_km_from_home = 0.0f;
    payload.has_last_transaction = true;
    payload.last_transaction_timestamp = "2026-03-10T23:58:30Z";
    payload.last_transaction_km_from_current = 25.0f;

    QueryVector vector{};
    std::string error;
    expect(rinha::vectorize(payload, vector, error), "vectorize falhou: " + error);
    expect_near(vector[3], 0.0f, 0.0001f, "hora normalizada incorreta");
    expect_near(vector[4], 2.0f / 6.0f, 0.0001f, "weekday normalizado incorreto");
    expect_near(vector[5], 3.0f / 1440.0f, 0.0001f, "minutos desde a ultima transacao incorretos");
    expect_near(vector[6], 0.025f, 0.0001f, "distancia da ultima transacao incorreta");
}

void test_vectorize_clamps_and_defaults_edge_values() {
    Payload payload{};
    std::string error;
    expect(rinha::parse_payload(kUnknownMerchantPayload, payload, error), "parse do payload edge falhou: " + error);
    expect(!payload.known_merchant, "merchant ausente da lista deveria ser desconhecido");

    QueryVector vector{};
    expect(rinha::vectorize(payload, vector, error), "vectorize do payload edge falhou: " + error);
    expect_near(vector[0], 1.0f, 0.0f, "amount acima do teto deveria ser limitado");
    expect_near(vector[1], 1.0f, 0.0f, "installments acima do teto deveria ser limitado");
    expect_near(vector[2], 1.0f, 0.0f, "amount_vs_avg com avg zero e amount positivo deveria ser 1");
    expect_near(vector[3], 1.0f, 0.0001f, "hora 23 deveria normalizar para 1");
    expect_near(vector[4], 3.0f / 6.0f, 0.0001f, "weekday de 2024-02-29 deveria ser quinta-feira");
    expect_near(vector[5], -1.0f, 0.0f, "minutes sem last_transaction deveria usar sentinela");
    expect_near(vector[6], -1.0f, 0.0f, "km sem last_transaction deveria usar sentinela");
    expect_near(vector[7], 1.0f, 0.0f, "km_from_home acima do teto deveria ser limitado");
    expect_near(vector[8], 1.0f, 0.0f, "tx_count_24h acima do teto deveria ser limitado");
    expect_near(vector[9], 1.0f, 0.0f, "is_online deveria virar 1");
    expect_near(vector[10], 0.0f, 0.0f, "card_present false deveria virar 0");
    expect_near(vector[11], 1.0f, 0.0f, "merchant desconhecido deveria virar 1");
    expect_near(vector[12], 0.5f, 0.0f, "MCC desconhecido deveria usar risco default");
    expect_near(vector[13], 1.0f, 0.0f, "merchant avg acima do teto deveria ser limitado");
}

void test_parse_payload_handles_duplicate_known_merchants() {
    Payload payload{};
    std::string error;
    expect(
        rinha::parse_payload(kDuplicateKnownMerchantPayload, payload, error),
        "parse do payload com merchants duplicados falhou: " + error
    );
    expect(payload.known_merchant, "merchant duplicado na lista deveria ser reconhecido");

    QueryVector vector{};
    expect(rinha::vectorize(payload, vector, error), "vectorize do payload com merchants duplicados falhou: " + error);
    expect_near(vector[11], 0.0f, 0.0f, "merchant conhecido deveria virar 0");
    expect_near(vector[12], 0.75f, 0.0f, "MCC 7802 deveria usar risco 0.75");
}

void test_vectorize_rejects_invalid_non_leap_february_date() {
    Payload payload{};
    payload.transaction_amount = 100.0f;
    payload.transaction_installments = 1U;
    payload.transaction_requested_at = "2023-02-29T00:00:00Z";
    payload.customer_avg_amount = 100.0f;
    payload.customer_tx_count_24h = 0U;
    payload.known_merchant = true;
    payload.merchant_mcc = "5411";
    payload.merchant_avg_amount = 100.0f;
    payload.terminal_is_online = false;
    payload.terminal_card_present = true;
    payload.terminal_km_from_home = 0.0f;
    payload.has_last_transaction = false;

    QueryVector vector{};
    std::string error;
    expect(!rinha::vectorize(payload, vector, error), "2023-02-29 deveria ser rejeitado");
    expect(!error.empty(), "erro de timestamp inválido deveria ser preenchido");
}

void test_classifier_matches_official_smoke_examples() {
    const std::filesystem::path repo_root = RINHA_REPO_ROOT;
    const std::filesystem::path references_path = repo_root / "resources" / "references.json.gz";

    ReferenceSet refs;
    std::string error;
    expect(
        ReferenceSet::load_gzip_json(references_path.string(), refs, error),
        "falha ao carregar referencias oficiais: " + error
    );

    Classifier classifier(std::move(refs));

    Payload legit{};
    expect(rinha::parse_payload(kLegitPayload, legit, error), "parse do payload legit falhou: " + error);
    Classification legit_classification{};
    expect(
        classifier.classify(legit, legit_classification, error),
        "classificacao do payload legit falhou: " + error
    );
    expect(legit_classification.approved, "payload legit deveria ser aprovado");
    expect_near(legit_classification.fraud_score, 0.0f, 0.0001f, "fraud_score legit inesperado");

    Payload fraud{};
    error.clear();
    expect(rinha::parse_payload(kFraudPayload, fraud, error), "parse do payload fraud falhou: " + error);
    Classification fraud_classification{};
    expect(
        classifier.classify(fraud, fraud_classification, error),
        "classificacao do payload fraud falhou: " + error
    );
    expect(!fraud_classification.approved, "payload fraud deveria ser negado");
    expect_near(fraud_classification.fraud_score, 1.0f, 0.0001f, "fraud_score fraud inesperado");
}

void test_reference_set_builds_pruning_groups() {
    const std::filesystem::path repo_root = RINHA_REPO_ROOT;
    const std::filesystem::path references_path = repo_root / "resources" / "references.json.gz";

    ReferenceSet refs;
    std::string error;
    expect(
        ReferenceSet::load_gzip_json(references_path.string(), refs, error),
        "falha ao carregar referencias oficiais para grupos: " + error
    );

    expect(!refs.groups().empty(), "referencias deveriam criar grupos para pruning");
    expect(refs.groups().size() < refs.len(), "grupos precisam ser menores que o total de referencias");
    expect(refs.groups().size() <= 512, "grupos deveriam permanecer dentro do limite do caminho AVX2");
}

}  // namespace

int main() {
    test_parse_payload_handles_known_merchant_and_null_last_transaction();
    test_vectorize_handles_midnight_gap();
    test_vectorize_clamps_and_defaults_edge_values();
    test_parse_payload_handles_duplicate_known_merchants();
    test_vectorize_rejects_invalid_non_leap_february_date();
    test_classifier_matches_official_smoke_examples();
    test_reference_set_builds_pruning_groups();
    return 0;
}
