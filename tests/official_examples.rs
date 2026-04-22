use std::{fs, path::PathBuf, sync::OnceLock};

use rinha_backend_2026::{Classifier, Payload, ReferenceSet, vector::vectorize};

const EXAMPLE_1_ID: &str = "tx-1329056812";
const EXAMPLE_2_ID: &str = "tx-1788243118";
const EXAMPLE_3_ID: &str = "tx-2174907811";
const EXAMPLE_4_ID: &str = "tx-3330991687";

fn fixture_path(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

fn payloads() -> &'static Vec<Payload> {
    static PAYLOADS: OnceLock<Vec<Payload>> = OnceLock::new();
    PAYLOADS.get_or_init(|| {
        let content = fs::read_to_string(fixture_path("resources/example-payloads.json")).unwrap();
        serde_json::from_str(&content).unwrap()
    })
}

fn payload_by_id(id: &str) -> Payload {
    payloads()
        .iter()
        .find(|payload| payload.id == id)
        .unwrap_or_else(|| panic!("payload {id} nao encontrado"))
        .clone()
}

fn reference_set() -> &'static ReferenceSet {
    static REFS: OnceLock<ReferenceSet> = OnceLock::new();
    REFS.get_or_init(|| {
        ReferenceSet::load_gzip_json(fixture_path("resources/references.json.gz")).unwrap()
    })
}

fn classifier() -> &'static Classifier {
    static CLASSIFIER: OnceLock<Classifier> = OnceLock::new();
    CLASSIFIER.get_or_init(|| Classifier::new(reference_set().clone()))
}

fn assert_vector_close(actual: [f32; 14], expected: [f32; 14]) {
    for (index, (actual, expected)) in actual.into_iter().zip(expected).enumerate() {
        let diff = (actual - expected).abs();
        assert!(
            diff <= 0.0002,
            "dim {index}: esperado {expected}, obtido {actual}, diff {diff}"
        );
    }
}

#[test]
fn vectorizes_official_legit_example() {
    let actual = vectorize(&payload_by_id(EXAMPLE_1_ID)).unwrap();
    assert_vector_close(
        actual,
        [
            0.0041, 0.1667, 0.05, 0.7826, 0.3333, -1.0, -1.0, 0.0292, 0.15, 0.0, 1.0, 0.0,
            0.15, 0.006,
        ],
    );
}

#[test]
fn vectorizes_official_fraud_example() {
    let actual = vectorize(&payload_by_id(EXAMPLE_2_ID)).unwrap();
    assert_vector_close(
        actual,
        [
            0.4369, 0.6667, 1.0, 0.0870, 0.1667, 0.0042, 0.6609, 0.8816, 0.9, 1.0, 0.0, 1.0,
            0.8, 0.0026,
        ],
    );
}

#[test]
fn classifies_official_examples_against_full_dataset() {
    let cases = [
        (EXAMPLE_1_ID, true, 0.0),
        (EXAMPLE_2_ID, false, 1.0),
        (EXAMPLE_3_ID, true, 0.4),
        (EXAMPLE_4_ID, false, 1.0),
    ];

    for (id, expected_approved, expected_score) in cases {
        let classification = classifier().classify(&payload_by_id(id)).unwrap();
        assert_eq!(
            classification.approved, expected_approved,
            "approved divergente para {id}"
        );
        assert!(
            (classification.fraud_score - expected_score).abs() <= 0.0001,
            "fraud_score divergente para {id}: esperado {expected_score}, obtido {}",
            classification.fraud_score
        );
    }
}
