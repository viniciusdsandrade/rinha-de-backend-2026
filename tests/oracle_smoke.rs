use std::{path::PathBuf, sync::OnceLock};

use rinha_backend_2026::{Classifier, ReferenceSet, oracle::evaluate_dataset};

fn fixture_path(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

fn classifier() -> &'static Classifier {
    static CLASSIFIER: OnceLock<Classifier> = OnceLock::new();
    CLASSIFIER.get_or_init(|| {
        let refs = ReferenceSet::load_gzip_json(fixture_path("resources/references.json.gz")).unwrap();
        Classifier::new(refs)
    })
}

#[test]
fn oracle_matches_the_first_twenty_entries_of_the_local_dataset() {
    let report = evaluate_dataset(
        classifier(),
        fixture_path("test/test-data.json"),
        Some(20),
    )
    .unwrap();

    assert_eq!(report.compared, 20);
    assert_eq!(report.mismatches, 0);
}
