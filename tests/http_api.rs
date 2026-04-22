use std::{fs, path::PathBuf, sync::OnceLock};

use axum::{
    body::Body,
    http::{Request, StatusCode},
};
use http_body_util::BodyExt;
use rinha_backend_2026::{Classification, Classifier, Payload, ReferenceSet, http::app};
use tower::ServiceExt;

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
    let response = app(classifier())
        .oneshot(
            Request::builder()
                .uri("/ready")
                .body(Body::empty())
                .unwrap(),
        )
        .await
        .unwrap();

    assert_eq!(response.status(), StatusCode::NO_CONTENT);
}

#[tokio::test]
async fn fraud_score_endpoint_classifies_official_payload() {
    let payload = payload("tx-1329056812");
    let request_body = serde_json::to_vec(&payload).unwrap();

    let response = app(classifier())
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/fraud-score")
                .header("content-type", "application/json")
                .body(Body::from(request_body))
                .unwrap(),
        )
        .await
        .unwrap();

    assert_eq!(response.status(), StatusCode::OK);

    let bytes = response.into_body().collect().await.unwrap().to_bytes();
    let actual: Classification = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(
        actual,
        Classification {
            approved: true,
            fraud_score: 0.0,
        }
    );
}
