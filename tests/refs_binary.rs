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
