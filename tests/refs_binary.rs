use std::{
    fs,
    path::PathBuf,
    time::{SystemTime, UNIX_EPOCH},
};

use rinha_backend_2026::ReferenceSet;

fn fixture_path(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

fn temp_file(name: &str) -> PathBuf {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    std::env::temp_dir().join(format!("rinha-backend-2026-{name}-{unique}"))
}

#[test]
fn binary_roundtrip_preserves_example_references() {
    let refs = ReferenceSet::load_json(fixture_path("resources/example-references.json")).unwrap();
    let refs_bin = temp_file("references.bin");
    let labels_bin = temp_file("labels.bin");

    refs.write_binary(&refs_bin, &labels_bin).unwrap();
    let loaded = ReferenceSet::load_binary(&refs_bin, &labels_bin).unwrap();

    assert_eq!(loaded.len(), refs.len());
    assert_eq!(loaded.labels(), refs.labels());

    for dim in 0..14 {
        assert_eq!(loaded.dim(dim), refs.dim(dim), "dim {dim} divergiu");
    }

    fs::remove_file(refs_bin).unwrap();
    fs::remove_file(labels_bin).unwrap();
}
