use std::{fs, path::Path};

use serde::Deserialize;

use crate::{Classifier, Payload};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OracleReport {
    pub compared: usize,
    pub mismatches: usize,
}

pub fn evaluate_dataset(
    classifier: &Classifier,
    dataset_path: impl AsRef<Path>,
    limit: Option<usize>,
) -> Result<OracleReport, String> {
    let content = fs::read_to_string(dataset_path.as_ref())
        .map_err(|error| format!("falha ao ler dataset de teste: {error}"))?;
    let dataset: OracleDataset = serde_json::from_str(&content)
        .map_err(|error| format!("falha ao decodificar dataset de teste: {error}"))?;

    let mut compared = 0usize;
    let mut mismatches = 0usize;

    for entry in dataset
        .entries
        .into_iter()
        .take(limit.unwrap_or(usize::MAX))
    {
        let actual = classifier.classify(&entry.request)?;
        compared += 1;

        if actual.approved != entry.info.expected_response.approved {
            mismatches += 1;
        }
    }

    Ok(OracleReport {
        compared,
        mismatches,
    })
}

#[derive(Debug, Deserialize)]
struct OracleDataset {
    entries: Vec<OracleEntry>,
}

#[derive(Debug, Deserialize)]
struct OracleEntry {
    request: Payload,
    info: OracleEntryInfo,
}

#[derive(Debug, Deserialize)]
struct OracleEntryInfo {
    expected_response: OracleExpectedResponse,
}

#[derive(Debug, Deserialize)]
struct OracleExpectedResponse {
    approved: bool,
}
