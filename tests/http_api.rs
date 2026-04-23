use std::{fs, path::PathBuf, sync::OnceLock};

use rinha_backend_2026::{
    Classification, Classifier, Payload, ReferenceSet, http::handle_request_bytes,
};

fn fixture_path(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

fn reference_set() -> &'static ReferenceSet {
    static REFS: OnceLock<ReferenceSet> = OnceLock::new();
    REFS.get_or_init(|| {
        ReferenceSet::load_gzip_json(fixture_path("resources/references.json.gz")).unwrap()
    })
}

fn classifier() -> Classifier {
    Classifier::new(reference_set().clone())
}

fn payload(id: &str) -> Payload {
    let content = fs::read_to_string(fixture_path("resources/example-payloads.json")).unwrap();
    let payloads: Vec<Payload> = serde_json::from_str(&content).unwrap();
    payloads
        .into_iter()
        .find(|payload| payload.id == id)
        .unwrap_or_else(|| panic!("payload {id} nao encontrado"))
}

#[tokio::test]
async fn ready_endpoint_returns_no_content() {
    let classifier = classifier();
    let response = handle_request_bytes(&classifier, b"GET /ready HTTP/1.1\r\n\r\n", b"");

    assert_status(response, 204);
}

#[tokio::test]
async fn fraud_score_endpoint_classifies_official_payload() {
    let payload = payload("tx-1329056812");
    let request_body = serde_json::to_vec(&payload).unwrap();
    let classifier = classifier();

    let response = handle_request_bytes(
        &classifier,
        b"POST /fraud-score HTTP/1.1\r\ncontent-type: application/json\r\n\r\n",
        &request_body,
    );

    assert_status(response, 200);
    let actual: Classification = serde_json::from_slice(response_body(response)).unwrap();
    assert_eq!(
        actual,
        Classification {
            approved: true,
            fraud_score: 0.0,
        }
    );
}

#[tokio::test]
async fn malformed_json_returns_bad_request() {
    let classifier = classifier();
    let response = handle_request_bytes(
        &classifier,
        b"POST /fraud-score HTTP/1.1\r\ncontent-type: application/json\r\n\r\n",
        b"{",
    );

    assert_status(response, 400);
}

#[tokio::test]
async fn classifier_error_returns_safe_fallback() {
    let mut payload = payload("tx-1329056812");
    payload.transaction.requested_at = "invalid-date".to_string();
    let request_body = serde_json::to_vec(&payload).unwrap();
    let classifier = classifier();

    let response = handle_request_bytes(
        &classifier,
        b"POST /fraud-score HTTP/1.1\r\ncontent-type: application/json\r\n\r\n",
        &request_body,
    );

    assert_status(response, 200);
    let actual: Classification = serde_json::from_slice(response_body(response)).unwrap();
    assert_eq!(
        actual,
        Classification {
            approved: true,
            fraud_score: 0.0,
        }
    );
}

fn assert_status(response: &[u8], expected: u16) {
    let status = std::str::from_utf8(&response[9..12])
        .unwrap()
        .parse::<u16>()
        .unwrap();
    assert_eq!(status, expected);
}

fn response_body(response: &[u8]) -> &[u8] {
    let header_end = response
        .windows(4)
        .position(|window| window == b"\r\n\r\n")
        .unwrap()
        + 4;
    &response[header_end..]
}
