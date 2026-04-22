use std::{env, net::SocketAddr, process::ExitCode};

use rinha_backend_2026::{Classifier, ReferenceSet, http::app};
use tokio::net::TcpListener;

#[tokio::main(flavor = "current_thread")]
async fn main() -> ExitCode {
    match run().await {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}

async fn run() -> Result<(), String> {
    let references_path =
        env::var("REFERENCES_PATH").unwrap_or_else(|_| "resources/references.json.gz".to_string());
    let references_bin_path = env::var("REFERENCES_BIN_PATH").ok();
    let labels_bin_path = env::var("LABELS_BIN_PATH").ok();
    let bind_addr = env::var("BIND_ADDR").unwrap_or_else(|_| "0.0.0.0:3000".to_string());

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
    let classifier = Classifier::new(refs);
    let app = app(classifier);
    let addr: SocketAddr = bind_addr
        .parse()
        .map_err(|error| format!("bind inválido {bind_addr}: {error}"))?;
    let listener = TcpListener::bind(addr)
        .await
        .map_err(|error| format!("falha ao abrir listener em {bind_addr}: {error}"))?;

    axum::serve(listener, app)
        .await
        .map_err(|error| format!("falha no servidor HTTP: {error}"))
}
