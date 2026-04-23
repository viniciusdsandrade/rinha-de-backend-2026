# Plano — Rinha de Backend 2026 (detecção de fraude por busca vetorial)

## 1. Context

O desafio pede um backend que, para cada requisição `POST /fraud-score`, normaliza
uma transação em um **vetor de 14 dimensões** e faz uma **busca vetorial (K-NN, K=5)**
contra um dataset de **100k vetores rotulados** (`references.json.gz`), devolvendo
`{ approved, fraud_score }`. Tudo isso sob:

- **1.0 CPU** e **350 MB de RAM** somando **todos** os containers
- **Porta 9999** no load balancer (mínimo: 1 LB + 2 APIs, round-robin simples)
- **Alvo p99 ≤ 10 ms** (acima disso o `final_score` é cortado linearmente)
- Pesos: `TP/TN = +1`, `FP = -1`, `FN = -3`, erro HTTP = `-5`

A chave do plano é a seguinte observação quantitativa: **o dataset cru ocupa apenas
100 000 × 14 × 4 B = 5,6 MB**. Não há pressão real de memória — a pressão é toda de
CPU/latência. Logo, a arquitetura *não tem banco de dados, Redis ou worker externo*.
Cada API carrega o dataset uma vez na inicialização e responde tudo em RAM.

A estratégia escolhida pelo usuário (Rust + SIMD manual + brute-force SoA) entrega
**100 % de recall idêntico ao labeler do teste** (que usa KNN exato com distância
euclidiana) e, em 14 D, bate HNSW em p50/p99 porque elimina o overhead do grafo.

---

## 2. Arquitetura

```
              :9999
  Cliente ─────────▶ Nginx (LB)
                       │
            Unix sockets (menor overhead que TCP loopback)
                       │
             ┌─────────┴─────────┐
             ▼                   ▼
          api1                 api2
       (Rust/axum)          (Rust/axum)
       references.bin       references.bin
       (5,6 MB em RAM)      (5,6 MB em RAM)
```

### Orçamento de recursos (soma = 1.0 CPU / 350 MB)

| Serviço | CPU     | RAM     | Justificativa                                      |
|---------|---------|---------|----------------------------------------------------|
| nginx   | `0.05`  | `20MB`  | LB burro; só proxy HTTP/1.1 keep-alive, sem TLS    |
| api1    | `0.475` | `165MB` | Dataset (6 MB) + stack + buffers axum + work arena |
| api2    | `0.475` | `165MB` | Idem                                               |
| **Σ**   | `1.000` | `350MB` |                                                    |

> As APIs recebem quase toda a CPU porque o hot-path é 100 % trabalho de CPU. O LB
> é burro e barato.

---

## 3. Stack e dependências

### APIs (Rust)

| Camada        | Escolha                               | Por quê                                               |
|---------------|---------------------------------------|-------------------------------------------------------|
| Runtime async | `tokio` (feat: `rt`, `net`, `macros`) | Maduro, estável, multi-thread opcional                |
| HTTP          | `axum` 0.7 + `hyper` 1                | Ergonomia + perf próxima de actix, suporta UDS nativo |
| JSON          | `sonic-rs` 0.3+                       | 1,5-2× mais rápido que `simd-json`, 3-4× que `serde`  |
| Alocador      | `mimalloc`                            | Melhor p99 em alta concorrência que o malloc do musl  |
| SIMD          | `std::arch::x86_64` (AVX2+FMA)        | Sem dependência externa; controle total               |
| Time parsing  | Parser manual zero-alloc              | Formato ISO é fixo (`YYYY-MM-DDTHH:MM:SSZ`, 20 bytes) |

### Load balancer

- `nginx:1.27-alpine` (~7 MB imagem, ~10 MB RSS em idle)
- `worker_processes 1;` `worker_connections 1024;` `use epoll;`
- Upstream round-robin em **Unix sockets** (reduz ~30 % latência vs TCP em Docker bridge)
- `keepalive 64;` no upstream + `proxy_http_version 1.1` (evita reconectar a cada req)
- `access_log off;` (desnecessário e custoso no p99)

### Dockerfile (multi-stage, estático, musl)

```dockerfile
FROM rust:1.82-alpine AS builder
RUN apk add --no-cache musl-dev pkgconfig
WORKDIR /build
COPY Cargo.toml Cargo.lock build.rs ./
COPY src/ ./src/
COPY tools/ ./tools/
COPY resources/ ./resources/
# 1) prepara o blob binário SoA a partir do .json.gz
RUN cargo run --release --bin prepare_refs --target x86_64-unknown-linux-musl
# 2) build final já com references.bin embutido via include_bytes!
RUN RUSTFLAGS="-C target-cpu=x86-64-v3" \
    cargo build --release --bin api --target x86_64-unknown-linux-musl

FROM scratch
COPY --from=builder /build/target/x86_64-unknown-linux-musl/release/api /api
ENTRYPOINT ["/api"]
```

> `target-cpu=x86-64-v3` habilita AVX2+FMA (disponível em todo Mac Mini 2014 i5,
> CPU do ambiente oficial da Rinha). Se a imagem-alvo não suportar, trocar por
> `x86-64-v2` e reescrever os intrínsecos para SSE2.

---

## 4. Pré-processamento do dataset (no build)

**Objetivo:** evitar qualquer custo de I/O ou parsing de JSON do dataset em runtime.
O arquivo vira um blob binário flat, carregável com `include_bytes!` ou `mmap`.

### Binário `prepare_refs` (`tools/prepare_refs.rs`)

Lê `resources/references.json.gz`, decodifica com `flate2`, parseia com `sonic-rs`
em streaming, e escreve dois blobs **alinhados em 32 bytes** no layout SoA:

```
references.bin:
  [dim0:  100_000 × f32]   ← 400 000 bytes
  [dim1:  100_000 × f32]
  ...
  [dim13: 100_000 × f32]
  total: 5 600 000 bytes (5,6 MB)

labels.bin:
  [100_000 × u8]   (0 = legit, 1 = fraud) → 100 000 bytes
```

SoA é escolhido em vez de AoS (`[[f32;14]; 100_000]`) porque durante a busca
vetorial varremos **todas as 100k referências pela dimensão 0, depois pela 1, …** em
lotes — isso alinha perfeitamente com `_mm256_load_ps` carregando 8 refs de uma
dimensão com 1 instrução.

### Embutir no binário

`build.rs` emite as constantes `REFS_PATH`/`LABELS_PATH` e `src/refs.rs` faz
`static REFS: &[u8] = include_bytes!(...)`. Assim o binário final tem ~12 MB
(6 MB dataset + ~6 MB código Rust/musl) e a inicialização custa uma
transmutação `&[u8] → &[f32; 1_400_000]`.

---

## 5. Runtime (estrutura de `src/`)

```
src/
├── main.rs       # boot: carrega refs, sobe axum em UDS, rotas
├── refs.rs       # struct Refs (SoA) + loader a partir de include_bytes!
├── vector.rs     # vetorização: payload JSON → [f32; 14]
├── parse.rs      # parse ISO 8601 + helpers zero-alloc
├── knn.rs        # brute-force SIMD + top-5 insertion sort
└── score.rs      # votação + threshold → (approved, fraud_score)
```

### Fluxo de uma requisição

1. **Axum recebe** `POST /fraud-score` com corpo `Bytes`
2. **`sonic-rs::from_slice`** desserializa para uma struct `Payload`
3. **`vector::from_payload(&payload) -> [f32; 14]`**:
    - Normaliza amount, installments, etc. com constantes hard-coded de
      `normalization.json` (são fixas, não leio JSON em runtime)
    - Parse do timestamp com `parse::iso_to_fields` (20 bytes fixos, sem regex)
    - `day_of_week` via **Zeller ou tabela pré-computada** para 2026 (uma janela
      pequena de datas cobre o teste)
    - `mcc_risk` via `phf::Map<&'static str, f32>` (perfect hash build-time) com
      default `0.5` para MCC ausente
    - `unknown_merchant` = `1` se `merchant.id ∉ customer.known_merchants`, senão `0`
      (varredura linear — a lista é minúscula, ≤ 10 itens tipicamente)
4. **`knn::top5(&query, &REFS) -> [u8; 5]`** — ver §6
5. **`score::decide(&labels)`**:
    - `fraud_score = labels.iter().filter(|&&l| l == 1).count() as f32 / 5.0`
    - `approved = fraud_score < 0.6`
6. **Resposta** via `sonic-rs::to_vec` em um `Bytes` reaproveitado

### `GET /ready`

Handler retorna `StatusCode::NO_CONTENT` imediatamente. Usado pelo poller da
engine (ver `config.json`: 30 retries × 3 s).

### Alocação de threads

Tokio em modo `rt-multi-thread` com `worker_threads(1)` — a CPU é fracionada,
ter mais threads só gera context switches inúteis dentro de 0,475 CPU. Se
medirmos throughput insuficiente, experimentar **ntex + `io_uring`** ou `monoio`
como substituição drop-in (ambos thread-per-core, menor overhead).

---

## 6. Busca vetorial — kernel SIMD (`src/knn.rs`)

### Distância euclidiana² (raiz é desnecessária — monotônica, só comparamos)

Para query `q: [f32; 14]` e 100 000 refs em SoA:

- Dividir os 100 000 em 12 500 **lotes de 8** refs
- Para cada lote: acumula `sum = Σ (q[d] - ref[d])²` usando AVX2 sobre os 8 refs

```rust
use std::arch::x86_64::*;

#[target_feature(enable = "avx2,fma")]
unsafe fn dist_batch8(q: &[f32; 14], refs: &Refs, batch: usize) -> [f32; 8] {
    let mut sum = _mm256_setzero_ps();
    // SAFETY: batch * 8 + 7 < 100_000 garantido pelo chamador
    for d in 0..14 {
        let q_d  = _mm256_set1_ps(q[d]);
        let r_d  = _mm256_load_ps(refs.dim(d).as_ptr().add(batch * 8));
        let diff = _mm256_sub_ps(q_d, r_d);
        sum      = _mm256_fmadd_ps(diff, diff, sum); // sum += diff²
    }
    let mut out = [0f32; 8];
    _mm256_storeu_ps(out.as_mut_ptr(), sum);
    out
}
```

### Top-5 (inserção direta, sem heap)

Como K = 5 é constante e pequeno, um **vetor de 5 pares `(dist, idx)` mantido
ordenado por inserção linear** é mais rápido que qualquer min-heap. Cada lote
contribui com 8 candidatos → 40 comparações/lote × 12 500 lotes ≈ 500 k
comparações baratas (branch-predictable).

```rust
fn insert_top5(top: &mut [(f32, u32); 5], d: f32, i: u32) {
    if d >= top[4].0 { return; }
    let mut j = 4;
    while j > 0 && top[j - 1].0 > d { top[j] = top[j - 1]; j -= 1; }
    top[j] = (d, i);
}
```

### Custo estimado

- 12 500 lotes × (14 × 3 instruções FMA/sub + loads) ≈ 525 k instruções SIMD
- Em CPU 2,6 GHz do Mac Mini oficial: ~200 µs por requisição (muito abaixo dos
  10 ms alvo)

### Tratamento do sentinela `-1`

Os índices 5 e 6 recebem `-1.0` quando `last_transaction` é `null`. O dataset
**já contém `-1.0`** nessas posições — é só não filtrar. A distância euclidiana
automaticamente aproxima queries "sem histórico" de refs "sem histórico" (ambas
com `-1` na mesma dimensão anulam a diferença).

---

## 7. Nginx (`nginx.conf`)

```nginx
worker_processes 1;
events { worker_connections 1024; use epoll; multi_accept on; }

http {
    access_log off;
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 30;

    upstream api {
        server unix:/sockets/api1.sock;
        server unix:/sockets/api2.sock;
        keepalive 64;
    }

    server {
        listen 9999 default_server;
        location / {
            proxy_pass http://api;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
        }
    }
}
```

---

## 8. `docker-compose.yml` (branch `submission`)

```yaml
services:
  api1: &api
    image: ghcr.io/andrade/rinha2026-api:v1
    volumes: [ api-sock:/sockets ]
    environment:
      SOCKET_PATH: /sockets/api1.sock
    networks: [ backend ]
    deploy:
      resources:
        limits:
          cpus: "0.475"
          memory: "165MB"

  api2:
    <<: *api
    environment:
      SOCKET_PATH: /sockets/api2.sock

  nginx:
    image: nginx:1.27-alpine
    depends_on: [ api1, api2 ]
    volumes:
      - api-sock:/sockets
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
    networks: [ backend ]
    ports: [ "9999:9999" ]
    deploy:
      resources:
        limits:
          cpus: "0.05"
          memory: "20MB"

networks:
  backend: { driver: bridge }
volumes:
  api-sock: { }
```

> Regras do desafio proíbem `network_mode: host` e `privileged`. `bridge` está ok.

---

## 9. Arquivos a criar

### Branch `main` (código-fonte)

| Arquivo                 | Propósito                                                        |
|-------------------------|------------------------------------------------------------------|
| `Cargo.toml`            | deps: tokio, axum, sonic-rs, mimalloc, flate2 (só no tools), phf |
| `build.rs`              | Repassa paths para `include_bytes!`                              |
| `src/main.rs`           | Boot: carrega Refs, axum em UDS, rotas                           |
| `src/refs.rs`           | `struct Refs` SoA + loader                                       |
| `src/vector.rs`         | Vetorização 14D                                                  |
| `src/parse.rs`          | ISO timestamp parser zero-alloc                                  |
| `src/knn.rs`            | SIMD brute-force + top-5                                         |
| `src/score.rs`          | Votação e threshold                                              |
| `tools/prepare_refs.rs` | Bin auxiliar: .json.gz → .bin SoA (roda no `docker build`)       |
| `Dockerfile`            | Multi-stage Rust/musl → scratch                                  |
| `nginx.conf`            | LB config                                                        |
| `README.md`             | Como rodar local                                                 |

### Branch `submission` (apenas artefatos de execução — sem código)

| Arquivo              | Conteúdo                           |
|----------------------|------------------------------------|
| `docker-compose.yml` | Como §8                            |
| `nginx.conf`         | Idem                               |
| `info.json`          | Metadata do participante (ver §11) |

### PR ao repositório da Rinha

| Arquivo                     | Conteúdo                                    |
|-----------------------------|---------------------------------------------|
| `participants/andrade.json` | `[{ "id": "andrade-rust", "repo": "..." }]` |

---

## 10. Verificação (roteiro end-to-end)

### Pré-requisitos

- `nix-shell` no repositório da Rinha (entra com `gcc`, `k6`, `jq`)
- Docker 24+ com `compose` plugin

### Ciclo local

```bash
# 1. Build da imagem
cd ~/Desktop/rinha-de-backend-2026-andrade   # repo do participante
docker build -t ghcr.io/andrade/rinha2026-api:dev .

# 2. Sobe o stack com limites reais
docker compose up -d

# 3. Health check
curl -sf http://localhost:9999/ready

# 4. Sanity check manual (exemplo do REGRAS_DE_DETECCAO.md, espera approved=true, score=0.0)
curl -s -X POST http://localhost:9999/fraud-score \
  -H 'Content-Type: application/json' \
  -d '{"id":"tx-1329056812","transaction":{"amount":41.12,"installments":2,"requested_at":"2026-03-11T18:45:53Z"},"customer":{"avg_amount":82.24,"tx_count_24h":3,"known_merchants":["MERC-003","MERC-016"]},"merchant":{"id":"MERC-016","mcc":"5411","avg_amount":60.25},"terminal":{"is_online":false,"card_present":true,"km_from_home":29.23},"last_transaction":null}'
# → {"approved":true,"fraud_score":0.0}

# 5. Teste de carga oficial (no diretório do desafio)
cd ~/Desktop/rinha-de-backend-2026
./run.sh   # imprime results.json com breakdown, p99, final_score
```

### Critérios de aceitação

| Métrica                  | Alvo                                                       |
|--------------------------|------------------------------------------------------------|
| `http_errors`            | `0`                                                        |
| `p99`                    | `≤ 10 ms` (ideal `< 2 ms`)                                 |
| `detection_accuracy`     | `> 99 %` (idealmente 100 % se brute-force fiel ao labeler) |
| RSS total (docker stats) | `< 350 MB` somados                                         |
| CPU total                | `≤ 100 %`                                                  |

### Estratégia de regressão

- Rodar `./run.sh` **três vezes** e descartar a primeira (cold caches)
- Monitorar `docker stats --no-stream` em paralelo durante o teste
- Se p99 > 10 ms: investigar nesta ordem (mais provável → menos):
    1. Serialização/deserialização JSON (trocar por escrita manual de bytes)
    2. Contenção do tokio scheduler (experimentar `monoio`)
    3. Cache misses no varrimento SIMD (tentar `prefetcht0` explícito)
    4. Nginx buffering (desabilitar `proxy_buffering off;`)

---

## 11. Submissão (ver [SUBMISSAO.md](./SUBMISSAO.md))

1. Criar repo público `andrade/rinha-de-backend-2026`
2. Push do código na branch `main`
3. Criar branch `submission` com **apenas** `docker-compose.yml`, `nginx.conf`,
   `info.json`
4. Publicar imagem em registry público (ghcr.io ou docker.io)
5. Testar o `docker-compose.yml` da branch submission a partir de outro diretório
   (garantir que não depende de arquivos locais além dos declarados)
6. Fork do repo `zanfranceschi/rinha-de-backend-2026`
7. PR adicionando `participants/andrade.json`:
   ```json
   [{ "id": "andrade-rust", "repo": "https://github.com/andrade/rinha-de-backend-2026" }]
   ```
8. Após merge do PR, abrir issue `rinha/test andrade-rust` no repo oficial para
   disparar a engine de teste

### `info.json`

```json
{
  "participants": [
    "Andrade"
  ],
  "social": [
    "https://github.com/andrade"
  ],
  "source-code-repo": "https://github.com/andrade/rinha-de-backend-2026",
  "stack": [
    "rust",
    "axum",
    "tokio",
    "nginx"
  ],
  "open_to_work": true
}
```

---

## 12. Riscos e planos B

| Risco                                             | Mitigação                                                                                                      |
|---------------------------------------------------|----------------------------------------------------------------------------------------------------------------|
| Target `x86-64-v3` (AVX2) ausente no host oficial | Fallback para `x86-64-v2` (SSE2) com intrínsecos SSE — perde ~30 % mas ainda bate 10 ms                        |
| `axum` + `tokio` adicionam jitter no p99          | Trocar por `ntex` ou `monoio` (thread-per-core io_uring) — drop-in a nível de handler                          |
| RSS estoura em carga alta                         | Reduzir `worker_threads` do tokio para 1, desabilitar jemalloc, forçar limites via env                         |
| `sonic-rs` falha ao compilar em musl              | Fallback para `serde_json` + `bytes` (perde ~1 µs por request, aceitável)                                      |
| Engine mostrar erros HTTP intermitentes           | `proxy_next_upstream error timeout;` no nginx + panic_handler custom no Rust garantindo resposta "0.0" default |
| `detection_accuracy` < 99 %                       | Log divergências do labeler e investigar parse do timestamp / MCC default / `known_merchants` edge cases       |

---

## 13. Roteiro de execução sugerido (ordem de implementação)

1. **Scaffold Rust** — `cargo init`, dependências do §3, handler `/ready` stub
2. **Pipeline de dataset** — `tools/prepare_refs.rs` + `build.rs`, valida com os
   2 exemplos de `resources/example-references.json`
3. **Vetorização** (`vector.rs`) — usa os 4 exemplos de
   [REGRAS_DE_DETECCAO.md](./REGRAS_DE_DETECCAO.md) como testes unitários de
   paridade
4. **KNN SIMD** (`knn.rs`) — testa primeiro uma versão escalar simples como
   oracle, depois a SIMD, comparando top-5 em 100 queries aleatórias
5. **`/fraud-score`** — conecta tudo, roda os 4 exemplos canônicos via curl
6. **UDS + Nginx** — muda axum para `UnixListener`, sobe docker compose
7. **Load test local** — `./run.sh`, lê `test/results.json`, itera
8. **Publica imagem + branches** — §11
9. **Abre PR e issue `rinha/test`** — §11

---

## 14. Referências-chave consultadas

- [github.com/cloudwego/sonic-rs](https://github.com/cloudwego/sonic-rs) — JSON SIMD
- [github.com/ashvardanian/SimSIMD](https://github.com/ashvardanian/SimSIMD) — kernels SIMD de referência
- [blog.cloudflare.com/computing-euclidean-distance-on-144-dimensions](https://blog.cloudflare.com/computing-euclidean-distance-on-144-dimensions/) —
  padrão SoA + SIMD
- [ricassiocosta.me (vencedor 2025)](https://ricassiocosta.me/2025/08/como-venci-a-rinha-de-backend-2025/) — lição "
  consolidar serviços"
- [docs.rs/axum](https://docs.rs/axum) — UDS listener + handlers
- [github.com/bytedance/monoio](https://github.com/bytedance/monoio) — plano B io_uring

---

## 15. Arquivos críticos do próprio repositório que serão reaproveitados

| Arquivo (no repo do desafio)                                  | Uso                                                      |
|---------------------------------------------------------------|----------------------------------------------------------|
| `resources/references.json.gz`                                | Fonte do blob SoA (é copiado para o build da imagem)     |
| `resources/mcc_risk.json`                                     | Hard-coded em `src/vector.rs` via `phf` build-time       |
| `resources/normalization.json`                                | Hard-coded como constantes em `src/vector.rs`            |
| `resources/example-references.json`                           | Testes de paridade do loader                             |
| `resources/example-payloads.json`                             | Testes de integração da API                              |
| [REGRAS_DE_DETECCAO.md](./REGRAS_DE_DETECCAO.md) (4 exemplos) | Testes unitários de vetorização + KNN + score            |
| `test/test.js`, `test/test-data.json`                         | Carga oficial — rodar via `../run.sh` do repo-desafio    |
| `config.json`                                                 | `submission_health_check_*` para ajustar /ready timeouts |

---

[← README principal](./README.md)
