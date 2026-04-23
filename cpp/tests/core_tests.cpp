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

}  // namespace

int main() {
    test_parse_payload_handles_known_merchant_and_null_last_transaction();
    test_vectorize_handles_midnight_gap();
    test_classifier_matches_official_smoke_examples();
    return 0;
}
