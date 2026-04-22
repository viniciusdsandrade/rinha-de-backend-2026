use std::{env, process::ExitCode};

use rinha_backend_2026::{Classifier, ReferenceSet, oracle::evaluate_dataset};

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let mut args = env::args().skip(1);
    let dataset_path = args
        .next()
        .unwrap_or_else(|| "test/test-data.json".to_string());

    let mut limit = None;
    while let Some(arg) = args.next() {
        if arg == "--limit" {
            let value = args
                .next()
                .ok_or_else(|| "uso: oracle_check [dataset.json] [--limit N]".to_string())?;
            let parsed = value
                .parse::<usize>()
                .map_err(|error| format!("limit inválido {value}: {error}"))?;
            limit = Some(parsed);
        } else {
            return Err("uso: oracle_check [dataset.json] [--limit N]".to_string());
        }
    }

    let references_path =
        env::var("REFERENCES_PATH").unwrap_or_else(|_| "resources/references.json.gz".to_string());
    let references_bin_path = env::var("REFERENCES_BIN_PATH").ok();
    let labels_bin_path = env::var("LABELS_BIN_PATH").ok();

    let refs = match (references_bin_path.as_deref(), labels_bin_path.as_deref()) {
        (Some(references_bin), Some(labels_bin)) => {
            ReferenceSet::load_binary(references_bin, labels_bin)?
        }
        (None, None) => ReferenceSet::load_gzip_json(&references_path)?,
        _ => {
            return Err(
                "REFERENCES_BIN_PATH e LABELS_BIN_PATH devem ser informados juntos".to_string(),
            );
        }
    };

    let report = evaluate_dataset(&Classifier::new(refs), dataset_path, limit)?;
    println!(
        "{{\"compared\":{},\"mismatches\":{}}}",
        report.compared, report.mismatches
    );

    if report.mismatches == 0 {
        Ok(())
    } else {
        Err(format!(
            "oracle_check encontrou {} divergências em {} entradas",
            report.mismatches, report.compared
        ))
    }
}
