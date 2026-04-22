use std::sync::Arc;

use axum::{
    Json, Router,
    body::Bytes,
    extract::State,
    http::StatusCode,
    routing::{get, post},
};

use crate::{Classification, Classifier, Payload};

pub fn app(_classifier: Classifier) -> Router {
    Router::new()
        .route("/ready", get(ready))
        .route("/fraud-score", post(fraud_score))
        .with_state(Arc::new(_classifier))
}

async fn ready() -> StatusCode {
    StatusCode::NO_CONTENT
}

async fn fraud_score(
    State(classifier): State<Arc<Classifier>>,
    body: Bytes,
) -> Result<Json<Classification>, StatusCode> {
    let payload: Payload =
        sonic_rs::from_slice(body.as_ref()).map_err(|_| StatusCode::BAD_REQUEST)?;

    Ok(Json(classifier.classify(&payload).unwrap_or(
        Classification {
            approved: true,
            fraud_score: 0.0,
        },
    )))
}
