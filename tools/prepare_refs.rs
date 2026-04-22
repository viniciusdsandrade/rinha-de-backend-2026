use std::{env, process::ExitCode};

use rinha_backend_2026::ReferenceSet;

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

    let input_path = args.next().ok_or_else(usage)?;
    let references_bin = args.next().ok_or_else(usage)?;
    let labels_bin = args.next().ok_or_else(usage)?;

    if args.next().is_some() {
        return Err(usage());
    }

    let refs = ReferenceSet::load_gzip_json(&input_path)?;
    refs.write_binary(&references_bin, &labels_bin)?;

    eprintln!(
        "prepared {} referências em {} e {}",
        refs.len(),
        references_bin,
        labels_bin
    );

    Ok(())
}

fn usage() -> String {
    "uso: prepare_refs <references.json.gz> <references.bin> <labels.bin>".to_string()
}
