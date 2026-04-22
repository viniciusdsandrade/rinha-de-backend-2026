use std::path::PathBuf;

use rinha_backend_2026::ReferenceSet;
use tempfile::tempdir;

fn fixture_path(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

#[test]
fn binary_roundtrip_preserves_example_references() {
    let refs = ReferenceSet::load_json(fixture_path("resources/example-references.json")).unwrap();
    let tempdir = tempdir().unwrap();
    let refs_bin = tempdir.path().join("references.bin");
    let labels_bin = tempdir.path().join("labels.bin");

    refs.write_binary(&refs_bin, &labels_bin).unwrap();
    let loaded = ReferenceSet::load_binary(&refs_bin, &labels_bin).unwrap();

    assert_eq!(loaded.len(), refs.len());
    assert_eq!(loaded.labels(), refs.labels());

    for dim in 0..14 {
        assert_eq!(loaded.dim(dim), refs.dim(dim), "dim {dim} divergiu");
    }
}

#[test]
fn precomputed_squared_norm_matches_manual_sum() {
    let refs = ReferenceSet::load_json(fixture_path("resources/example-references.json")).unwrap();

    let manual = (0..14)
        .map(|dim| {
            let value = refs.dim(dim)[0];
            value * value
        })
        .sum::<f32>();

    assert!((refs.squared_norm(0) - manual).abs() <= 0.000001);
}

#[test]
fn bounded_distance_returns_none_when_threshold_is_too_small() {
    let refs = ReferenceSet::load_json(fixture_path("resources/example-references.json")).unwrap();
    let query = [0.0f32; 14];
    let exact = refs.distance_squared(&query, 0);

    assert!(exact > 0.0001, "esperava distância positiva para o fixture");
    assert_eq!(
        refs.distance_squared_if_below(&query, 0, exact + 0.0001),
        Some(exact)
    );
    assert_eq!(refs.distance_squared_if_below(&query, 0, exact - 0.0001), None);
}
