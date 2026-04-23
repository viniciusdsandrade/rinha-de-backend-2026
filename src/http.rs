use std::{io, sync::Arc};

use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    net::{TcpListener, UnixListener},
};

use crate::{Classification, Classifier, Payload};

const READ_CAPACITY: usize = 16_384;

const RESPONSE_READY: &[u8] = b"HTTP/1.1 204 No Content\r\n\
Connection: keep-alive\r\n\
Content-Length: 0\r\n\
\r\n";

const RESPONSE_400: &[u8] = b"HTTP/1.1 400 Bad Request\r\n\
Connection: keep-alive\r\n\
Content-Length: 0\r\n\
\r\n";

const RESPONSE_404: &[u8] = b"HTTP/1.1 404 Not Found\r\n\
Connection: keep-alive\r\n\
Content-Length: 0\r\n\
\r\n";

const RESPONSE_JSON_0: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: application/json\r\n\
Content-Length: 35\r\n\
Connection: keep-alive\r\n\
\r\n\
{\"approved\":true,\"fraud_score\":0.0}";

const RESPONSE_JSON_1: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: application/json\r\n\
Content-Length: 35\r\n\
Connection: keep-alive\r\n\
\r\n\
{\"approved\":true,\"fraud_score\":0.2}";

const RESPONSE_JSON_2: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: application/json\r\n\
Content-Length: 35\r\n\
Connection: keep-alive\r\n\
\r\n\
{\"approved\":true,\"fraud_score\":0.4}";

const RESPONSE_JSON_3: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: application/json\r\n\
Content-Length: 36\r\n\
Connection: keep-alive\r\n\
\r\n\
{\"approved\":false,\"fraud_score\":0.6}";

const RESPONSE_JSON_4: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: application/json\r\n\
Content-Length: 36\r\n\
Connection: keep-alive\r\n\
\r\n\
{\"approved\":false,\"fraud_score\":0.8}";

const RESPONSE_JSON_5: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: application/json\r\n\
Content-Length: 36\r\n\
Connection: keep-alive\r\n\
\r\n\
{\"approved\":false,\"fraud_score\":1.0}";

pub async fn serve_tcp(listener: TcpListener, classifier: Classifier) -> Result<(), String> {
    let classifier = Arc::new(classifier);

    loop {
        let (stream, _) = listener
            .accept()
            .await
            .map_err(|error| format!("falha ao aceitar conexão TCP: {error}"))?;
        let classifier = Arc::clone(&classifier);
        tokio::spawn(async move {
            let _ = handle_connection(stream, classifier).await;
        });
    }
}

pub async fn serve_unix(listener: UnixListener, classifier: Classifier) -> Result<(), String> {
    let classifier = Arc::new(classifier);

    loop {
        let (stream, _) = listener
            .accept()
            .await
            .map_err(|error| format!("falha ao aceitar conexão UNIX: {error}"))?;
        let classifier = Arc::clone(&classifier);
        tokio::spawn(async move {
            let _ = handle_connection(stream, classifier).await;
        });
    }
}

async fn handle_connection<S>(mut stream: S, classifier: Arc<Classifier>) -> io::Result<()>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let mut buffer = [0u8; READ_CAPACITY];
    let mut read_len = 0usize;

    loop {
        if read_len == READ_CAPACITY {
            return Ok(());
        }

        let bytes_read = stream.read(&mut buffer[read_len..]).await?;
        if bytes_read == 0 {
            return Ok(());
        }
        read_len += bytes_read;

        while let Some(header_len) = find_header_end(&buffer[..read_len]) {
            let content_length = parse_content_length(&buffer[..header_len]).unwrap_or(0);
            if content_length > READ_CAPACITY || header_len + content_length > READ_CAPACITY {
                return Ok(());
            }
            if read_len < header_len + content_length {
                break;
            }

            let body_start = header_len;
            let body_end = body_start + content_length;
            let response = handle_request_bytes(
                &classifier,
                &buffer[..header_len],
                &buffer[body_start..body_end],
            );
            stream.write_all(response).await?;

            let consumed = header_len + content_length;
            read_len -= consumed;
            if read_len > 0 {
                buffer.copy_within(consumed..consumed + read_len, 0);
            }
        }
    }
}

pub fn handle_request_bytes(classifier: &Classifier, headers: &[u8], body: &[u8]) -> &'static [u8] {
    if request_matches(headers, b"GET", b"/ready") {
        return RESPONSE_READY;
    }

    if !request_matches(headers, b"POST", b"/fraud-score") {
        return RESPONSE_404;
    }

    let payload: Payload = match sonic_rs::from_slice(body) {
        Ok(payload) => payload,
        Err(_) => return RESPONSE_400,
    };

    classification_response(classifier.classify(&payload).unwrap_or(Classification {
        approved: true,
        fraud_score: 0.0,
    }))
}

fn classification_response(classification: Classification) -> &'static [u8] {
    match (
        classification.approved,
        fraud_bucket(classification.fraud_score),
    ) {
        (true, 0) => RESPONSE_JSON_0,
        (true, 1) => RESPONSE_JSON_1,
        (true, 2) => RESPONSE_JSON_2,
        (false, 3) => RESPONSE_JSON_3,
        (false, 4) => RESPONSE_JSON_4,
        (false, 5) => RESPONSE_JSON_5,
        _ => RESPONSE_JSON_0,
    }
}

fn request_matches(headers: &[u8], method: &[u8], path: &[u8]) -> bool {
    let min_len = method.len() + 1 + path.len() + 1;
    headers.len() >= min_len
        && &headers[..method.len()] == method
        && headers[method.len()] == b' '
        && &headers[method.len() + 1..method.len() + 1 + path.len()] == path
        && headers[method.len() + 1 + path.len()] == b' '
}

fn find_header_end(buffer: &[u8]) -> Option<usize> {
    buffer
        .windows(4)
        .position(|window| window == b"\r\n\r\n")
        .map(|position| position + 4)
}

fn parse_content_length(headers: &[u8]) -> Option<usize> {
    let mut lines = headers.split(|byte| *byte == b'\n');
    lines.next();

    for line in lines {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        let Some((name, value)) = line.split_once_byte(b':') else {
            continue;
        };
        if !name.eq_ignore_ascii_case(b"content-length") {
            continue;
        }

        let mut content_length = 0usize;
        for byte in value
            .iter()
            .copied()
            .skip_while(|byte| *byte == b' ' || *byte == b'\t')
        {
            if !byte.is_ascii_digit() {
                break;
            }
            content_length = (content_length * 10) + usize::from(byte - b'0');
        }
        return Some(content_length);
    }

    None
}

#[inline(always)]
fn fraud_bucket(score: f32) -> u8 {
    (score.mul_add(5.0, 0.5).floor() as i32).clamp(0, 5) as u8
}

trait SplitOnceByte {
    fn split_once_byte(&self, delimiter: u8) -> Option<(&[u8], &[u8])>;
}

impl SplitOnceByte for [u8] {
    fn split_once_byte(&self, delimiter: u8) -> Option<(&[u8], &[u8])> {
        let position = self.iter().position(|byte| *byte == delimiter)?;
        Some((&self[..position], &self[position + 1..]))
    }
}

#[cfg(test)]
mod tests {
    use super::{classification_response, parse_content_length};
    use crate::Classification;

    #[test]
    fn uses_prebuilt_json_for_all_supported_scores() {
        let cases = [
            (
                true,
                0.0,
                b"{\"approved\":true,\"fraud_score\":0.0}".as_slice(),
            ),
            (
                true,
                0.2,
                b"{\"approved\":true,\"fraud_score\":0.2}".as_slice(),
            ),
            (
                true,
                0.4,
                b"{\"approved\":true,\"fraud_score\":0.4}".as_slice(),
            ),
            (
                false,
                0.6,
                b"{\"approved\":false,\"fraud_score\":0.6}".as_slice(),
            ),
            (
                false,
                0.8,
                b"{\"approved\":false,\"fraud_score\":0.8}".as_slice(),
            ),
            (
                false,
                1.0,
                b"{\"approved\":false,\"fraud_score\":1.0}".as_slice(),
            ),
        ];

        for (approved, fraud_score, expected_body) in cases {
            let response = classification_response(Classification {
                approved,
                fraud_score,
            });
            let body = response.split(|byte| *byte == b'\n').last().unwrap();

            assert_eq!(body, expected_body);
        }
    }

    #[test]
    fn parses_content_length_case_insensitively() {
        let headers = b"POST /fraud-score HTTP/1.1\r\ncontent-length: 42\r\n\r\n";

        assert_eq!(parse_content_length(headers), Some(42));
    }
}
