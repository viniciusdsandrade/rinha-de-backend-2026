use std::{env, fs, net::SocketAddr, os::unix::fs::PermissionsExt, path::Path, process::ExitCode};

use mimalloc::MiMalloc;
use rinha_backend_2026::{
    Classifier, ReferenceSet,
    http::{serve_tcp, serve_unix},
};
use tokio::net::{TcpListener, UnixListener};

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

#[derive(Debug, PartialEq, Eq)]
enum ListenerConfig {
    Tcp(SocketAddr),
    Unix(String),
}

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
    let unix_socket_path = env::var("UNIX_SOCKET_PATH").ok();

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

    match listener_config(&bind_addr, unix_socket_path.as_deref())? {
        ListenerConfig::Tcp(addr) => {
            let listener = TcpListener::bind(addr)
                .await
                .map_err(|error| format!("falha ao abrir listener em {bind_addr}: {error}"))?;

            serve_tcp(listener, classifier).await
        }
        ListenerConfig::Unix(path) => serve_unix_socket(&path, classifier).await,
    }
}

fn listener_config(
    bind_addr: &str,
    unix_socket_path: Option<&str>,
) -> Result<ListenerConfig, String> {
    if let Some(path) = unix_socket_path
        .map(str::trim)
        .filter(|path| !path.is_empty())
    {
        return Ok(ListenerConfig::Unix(path.to_string()));
    }

    let addr: SocketAddr = bind_addr
        .parse()
        .map_err(|error| format!("bind inválido {bind_addr}: {error}"))?;
    Ok(ListenerConfig::Tcp(addr))
}

async fn serve_unix_socket(path: &str, classifier: Classifier) -> Result<(), String> {
    let socket_path = Path::new(path);

    if let Some(parent) = socket_path.parent() {
        fs::create_dir_all(parent)
            .map_err(|error| format!("falha ao criar diretório do socket {path}: {error}"))?;
    }

    if socket_path.exists() {
        fs::remove_file(socket_path)
            .map_err(|error| format!("falha ao remover socket antigo {path}: {error}"))?;
    }

    let listener = UnixListener::bind(socket_path)
        .map_err(|error| format!("falha ao abrir listener unix em {path}: {error}"))?;

    fs::set_permissions(socket_path, fs::Permissions::from_mode(0o777))
        .map_err(|error| format!("falha ao ajustar permissões do socket {path}: {error}"))?;

    serve_unix(listener, classifier).await
}

#[cfg(test)]
mod tests {
    use super::{ListenerConfig, listener_config};

    #[test]
    fn unix_socket_path_has_precedence_over_tcp_bind() {
        let config = listener_config("0.0.0.0:3000", Some("/tmp/rinha.sock")).unwrap();

        assert_eq!(config, ListenerConfig::Unix("/tmp/rinha.sock".to_string()));
    }

    #[test]
    fn invalid_bind_addr_is_rejected_when_socket_is_absent() {
        let error = listener_config("not-an-addr", None).unwrap_err();

        assert!(error.contains("bind inválido"));
    }
}
