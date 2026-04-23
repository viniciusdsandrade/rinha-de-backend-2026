use std::{fs, path::PathBuf, sync::OnceLock};

use rinha_backend_2026::{Classifier, Payload, ReferenceSet, classifier::SearchMode};
use serde::Deserialize;

fn fixture_path(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

fn classifier() -> &'static Classifier {
    static CLASSIFIER: OnceLock<Classifier> = OnceLock::new();
    CLASSIFIER.get_or_init(|| {
        let refs =
            ReferenceSet::load_gzip_json(fixture_path("resources/references.json.gz")).unwrap();
        Classifier::new(refs)
    })
}

fn example_payloads() -> Vec<Payload> {
    let content = fs::read_to_string(fixture_path("resources/example-payloads.json")).unwrap();
    serde_json::from_str(&content).unwrap()
}

fn first_test_entries(limit: usize) -> Vec<Payload> {
    let content = fs::read_to_string(fixture_path("test/test-data.json")).unwrap();
    let dataset: TestDataset = serde_json::from_str(&content).unwrap();
    dataset
        .entries
        .into_iter()
        .take(limit)
        .map(|entry| entry.request)
        .collect()
}

#[test]
fn avx2_search_matches_scalar_search_on_sample_queries() {
    if !Classifier::supports_avx2() {
        return;
    }

    let mut queries = example_payloads();
    queries.extend(first_test_entries(8));

    for payload in queries {
        let scalar = classifier()
            .classify_with_mode(&payload, SearchMode::Scalar)
            .unwrap();
        let avx2 = classifier()
            .classify_with_mode(&payload, SearchMode::Avx2)
            .unwrap();

        assert_eq!(scalar, avx2, "divergência na payload {}", payload.id);
    }
}

#[derive(Debug, Deserialize)]
struct TestDataset {
    entries: Vec<TestEntry>,
}

#[derive(Debug, Deserialize)]
struct TestEntry {
    request: Payload,
}
