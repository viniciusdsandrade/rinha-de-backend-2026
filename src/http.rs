use std::sync::Arc;

use axum::{
    body::Bytes,
    extract::State,
    http::StatusCode,
    response::{IntoResponse, Response},
    routing::{get, post},
    Router,
};

use crate::{Classification, Classifier, Payload};

pub fn app(classifier: Classifier) -> Router {
    Router::new()
        .route("/ready", get(ready))
        .route("/fraud-score", post(fraud_score))
        .with_state(Arc::new(classifier))
}

async fn ready() -> StatusCode {
    StatusCode::NO_CONTENT
}

async fn fraud_score(
    State(classifier): State<Arc<Classifier>>,
    body: Bytes,
) -> Result<Response, StatusCode> {
    let payload: Payload =
        sonic_rs::from_slice(body.as_ref()).map_err(|_| StatusCode::BAD_REQUEST)?;

    Ok(classification_response(
        classifier.classify(&payload).unwrap_or(Classification {
            approved: true,
            fraud_score: 0.0,
        }),
    ))
}

fn classification_response(classification: Classification) -> Response {
    const JSON_0: &str = "{\"approved\":true,\"fraud_score\":0.0}";
    const JSON_1: &str = "{\"approved\":true,\"fraud_score\":0.2}";
    const JSON_2: &str = "{\"approved\":true,\"fraud_score\":0.4}";
    const JSON_3: &str = "{\"approved\":false,\"fraud_score\":0.6}";
    const JSON_4: &str = "{\"approved\":false,\"fraud_score\":0.8}";
    const JSON_5: &str = "{\"approved\":false,\"fraud_score\":1.0}";

    let body = match (classification.approved, fraud_bucket(classification.fraud_score)) {
        (true, 0) => JSON_0,
        (true, 1) => JSON_1,
        (true, 2) => JSON_2,
        (false, 3) => JSON_3,
        (false, 4) => JSON_4,
        (false, 5) => JSON_5,
        _ => JSON_0,
    };

    (
        [(axum::http::header::CONTENT_TYPE, "application/json")],
        body,
    )
        .into_response()
}

#[inline(always)]
fn fraud_bucket(score: f32) -> u8 {
    (score.mul_add(5.0, 0.5).floor() as i32).clamp(0, 5) as u8
}

#[cfg(test)]
mod tests {
    use axum::http::{StatusCode, header::CONTENT_TYPE};
    use http_body_util::BodyExt;

    use super::classification_response;
    use crate::Classification;

    #[tokio::test]
    async fn uses_prebuilt_json_for_all_supported_scores() {
        let cases = [
            (true, 0.0, "{\"approved\":true,\"fraud_score\":0.0}"),
            (true, 0.2, "{\"approved\":true,\"fraud_score\":0.2}"),
            (true, 0.4, "{\"approved\":true,\"fraud_score\":0.4}"),
            (false, 0.6, "{\"approved\":false,\"fraud_score\":0.6}"),
            (false, 0.8, "{\"approved\":false,\"fraud_score\":0.8}"),
            (false, 1.0, "{\"approved\":false,\"fraud_score\":1.0}"),
        ];

        for (approved, fraud_score, expected_body) in cases {
            let response = classification_response(Classification {
                approved,
                fraud_score,
            });
            let (parts, body) = response.into_parts();
            let body = body.collect().await.unwrap().to_bytes();

            assert_eq!(parts.status, StatusCode::OK);
            assert_eq!(
                parts.headers.get(CONTENT_TYPE).unwrap(),
                "application/json"
            );
            assert_eq!(std::str::from_utf8(&body).unwrap(), expected_body);
        }
    }
}
