# Plano de Implementacao — Rinha de Backend 2026

> Deteccao de fraude por busca vetorial, em Rust, sob 1 CPU e 350 MB de RAM.

## 1. Objetivo

O desafio pede um backend que, para cada `POST /fraud-score`, transforme a
transacao em um vetor de 14 dimensoes, encontre os `K=5` vizinhos mais proximos
no dataset de referencia e responda:

```json
{
  "approved": false,
  "fraud_score": 0.8
}
```

Tudo isso respeitando:

- **1.0 CPU** e **350 MB de RAM** somando todos os containers
- **porta 9999** no load balancer
- **topologia minima obrigatoria**: 1 load balancer + 2 APIs web
- **p99 alvo <= 10 ms**
- pesos da avaliacao: `TP/TN = +1`, `FP = -1`, `FN = -3`, `HTTP error = -5`

O dado mais importante para a arquitetura e este:

- os vetores de referencia ocupam cerca de `100_000 x 14 x 4 B = 5.6 MB`

Conclusao pratica:

- **nao ha necessidade tecnica de banco de dados, Redis ou banco vetorial externo**
- a melhor aposta inicial e **carregar tudo em RAM e fazer busca exata**
- a disputa real e **latencia/CPU**, nao capacidade de armazenamento

---

## 2. Tese do plano

A estrategia recomendada e:

1. **correcao primeiro**
2. **busca exata em RAM como baseline**
3. **validacao offline contra a massa rotulada antes de expor HTTP**
4. **otimizacao incremental so depois de comprovar gargalo real**

Isso leva ao seguinte desenho:

- duas APIs identicas em Rust
- um `nginx` como load balancer round-robin simples
- dataset preprocessado em build para um formato binario compacto
- busca vetorial **exata** sobre os 100k vetores
- caminho de otimizacao opcional com AVX2 somente apos a versao escalar estar
  correta e medida

### Por que busca exata como baseline?

- o proprio desafio foi rotulado a partir de **k-NN com k=5 e distancia
  euclidiana**
- a memoria dos vetores e pequena
- ANN/HNSW traz complexidade, overhead de memoria e perda potencial de recall
- se a busca exata ja bater o `p99`, ela tende a dar o melhor equilibrio entre
  score final, simplicidade e reprodutibilidade

### O que este plano evita de proposito

- superestimar ganhos sem benchmark
- depender de comportamento fragil de alinhamento de memoria
- acoplar a implementacao a uma janela especifica de datas do teste local
- usar o load balancer para qualquer logica de negocio

---

## 3. Arquitetura recomendada

```
                 :9999
Cliente --------------------> nginx
                               |
                        Docker bridge
                               |
                  +------------+------------+
                  |                         |
                  v                         v
               api1:3000                 api2:3000
             (Rust / axum)             (Rust / axum)
             refs em RAM               refs em RAM
```

### Decisao importante: TCP interno por padrao

O plano original usava Unix domain sockets entre containers. Isso pode ser
feito, mas adiciona risco operacional desnecessario:

- permissao de socket
- UID/GID compartilhado
- debug menos direto
- ganho de latencia que precisa ser medido, nao assumido

Por isso, a recomendacao revisada e:

- **usar TCP entre containers na rede bridge como padrao**
- considerar UDS somente se o benchmark provar ganho material e a permissao
  estiver totalmente resolvida

### Orcamento de recursos

| Servico | CPU     | RAM     | Justificativa |
|---------|---------|---------|---------------|
| nginx   | `0.05`  | `20MB`  | proxy simples, round-robin, sem log |
| api1    | `0.475` | `165MB` | dataset em RAM + buffers + runtime |
| api2    | `0.475` | `165MB` | idem |
| **Soma**| `1.000` | `350MB` | dentro do limite oficial |

Observacoes:

- o dataset e pequeno o bastante para ser duplicado nas duas APIs
- a arquitetura continua obedecendo literalmente o documento
  [ARQUITETURA.md](./ARQUITETURA.md)

---

## 4. Stack e escolhas tecnicas

### API

| Camada | Escolha | Motivo |
|--------|---------|--------|
| Linguagem | Rust | previsibilidade de memoria e boa perf para hot-path numerico |
| HTTP | `axum` + `hyper` | stack simples, madura e suficiente para 2 instancias |
| Runtime | `tokio` current-thread | reduz complexidade e evita threads extras por container |
| JSON baseline | `serde` + `serde_json` | baseline robusta, portavel e facil de validar |
| JSON opcional | `sonic-rs` | so entra se profiling provar gargalo real de JSON |
| Busca | implementacao propria | dataset pequeno, requisitos simples, total controle |
| SIMD opcional | `std::arch::x86_64` AVX2 | so depois da versao escalar estar correta |

### Load balancer

- `nginx:1.27-alpine`
- round-robin padrao
- `access_log off`
- `proxy_http_version 1.1`
- upstream com `keepalive`

### Imagem

- build multi-stage
- binario estatico com musl se o conjunto de crates fechar bem
- se algum crate criar atrito desnecessario no musl, priorizar a versao mais
  simples e estavel em vez da mais agressiva

---

## 5. Estrategia de implementacao

O plano deve ser executado em **duas fases tecnicas**.

### Fase A — baseline correto

Objetivo:

- ter uma biblioteca de classificacao correta
- validar contra exemplos canonicos e contra a massa local rotulada
- expor a API e medir o comportamento real

Nesta fase:

- a busca pode ser inicialmente **escalar**
- a prioridade e fechar **paridade funcional**
- nenhuma otimizacao de baixo nivel entra antes disso

### Fase B — otimizacao medida

Objetivo:

- reduzir p99 sem comprometer recall

Nesta fase:

- substituir kernel escalar por AVX2 se isso realmente baixar latencia
- testar `serde_json` vs `sonic-rs` se JSON aparecer no perfil
- considerar UDS apenas se houver ganho real e risco operacional controlado

---

## 6. Dados e preprocessamento

Os arquivos relevantes do desafio sao:

- `resources/references.json.gz`
- `resources/mcc_risk.json`
- `resources/normalization.json`

### Decisao recomendada

Preprocessar `references.json.gz` durante o build da imagem e gerar dois
artefatos binarios:

- `references.bin`
- `labels.bin`

### Layout recomendado

Usar **SoA** (Structure of Arrays):

```text
references.bin
  [dim0: 100_000 x f32]
  [dim1: 100_000 x f32]
  ...
  [dim13: 100_000 x f32]

labels.bin
  [100_000 x u8]
```

Vantagens:

- leitura sequencial por dimensao
- bom comportamento de cache
- caminho natural para vetorizar em lotes

### O que nao fazer

Nao depender de `include_bytes!` + transmutacao direta para `&[f32]` assumindo
alinhamento especial. Isso torna o plano fragil.

### Estrategia segura de carga

Carregar os blobs no startup e materializar estruturas explicitas:

- `Vec<f32>` por dimensao, ou
- um buffer proprio validado e depois dividido em slices

Se a versao SIMD usar `_mm256_loadu_ps`, a implementacao nao precisa supor
alinhamento de 32 bytes. Se benchmarks posteriores mostrarem que alinhamento
explicitamente controlado ajuda, isso pode entrar como otimizacao da Fase B.

### Startup

No boot da API:

1. abrir `references.bin` e `labels.bin`
2. validar tamanho esperado
3. preencher estrutura `Refs`
4. so entao publicar `GET /ready` com sucesso

Isso cabe confortavelmente na janela de readiness do desafio:

- `30` retries
- `3000 ms` por intervalo

ver `config.json`.

---

## 7. Vetorizacao do payload

O payload deve seguir exatamente as regras de
[REGRAS_DE_DETECCAO.md](./REGRAS_DE_DETECCAO.md).

### Regra geral

Implementar `vector::from_payload(&Payload) -> [f32; 14]` com:

- clamp para `[0.0, 1.0]`
- sentinela `-1.0` nos indices `5` e `6` quando `last_transaction = null`
- `mcc_risk` com default `0.5`
- `unknown_merchant = 1.0` quando o merchant nao estiver em
  `customer.known_merchants`

### Datas e tempo

O plano corrigido nao deve usar tabela de datas limitada a 2026 nem assumir que
o teste final ficara na mesma janela do arquivo local.

Recomendacao:

- parsear o ISO fixo `YYYY-MM-DDTHH:MM:SSZ` manualmente ou com helper pequeno
- calcular `hour_of_day` e `minutes_since_last_tx`
- calcular `day_of_week` com algoritmo deterministico valido para qualquer data
  Gregorian/UTC relevante

### `known_merchants`

A massa local atual mostra listas pequenas, entao uma varredura linear e
adequada. Nao vale complicar com hash set por requisicao.

### Fontes dos valores fixos

- `normalization.json` pode virar constantes em codigo
- `mcc_risk.json` pode virar tabela carregada no startup ou mapa estatico

Como os arquivos nao mudam durante o teste, ambas as abordagens sao validas.

---

## 8. Busca vetorial

### Baseline obrigatorio

Implementar primeiro um kernel **escalar exato**:

```text
para cada referencia:
  dist2 = soma((q[d] - ref[d])^2) para d em 0..14
  inserir em top-5 se necessario
```

Usar **distancia euclidiana ao quadrado**:

- a raiz nao e necessaria
- a ordenacao e preservada
- elimina custo inutil

### Top-5

Como `K=5` e pequeno e fixo, manter um vetor ordenado de 5 pares
`(dist2, idx)` e melhor que heap.

### Caminho SIMD opcional

Depois que o kernel escalar estiver validado:

- adicionar caminho AVX2 por lotes de 8 referencias
- usar `_mm256_loadu_ps`
- manter fallback escalar
- habilitar o caminho AVX2 apenas quando:
  - a CPU suportar a feature
  - os testes de paridade escalar vs SIMD passarem

### Sobre HNSW e ANN

Nao sao a escolha principal deste plano.

Motivos:

- o objetivo da versao inicial e **igualar o rotulador**
- ANN e aproximado por natureza
- o dataset e pequeno o bastante para justificar tentativa exata primeiro

ANN so vira plano B se benchmarks mostrarem, com evidencia, que a busca exata
nao fecha o alvo de latencia mesmo apos otimizacoes razoaveis.

---

## 9. Tratamento de erros e comportamento da API

### `GET /ready`

Responder `204 No Content` apenas quando:

- referencias carregadas
- tabelas auxiliares carregadas
- servidor pronto para aceitar trafego

### `POST /fraud-score`

Fluxo:

1. parse JSON
2. vetorizar
3. buscar top-5
4. contar fraudes
5. responder `{ approved, fraud_score }`

### Erros

Recomendacao pragmatica:

- **request malformada**: `400`
- **falha interna inesperada depois que o request valido ja foi aceito**:
  responder um fallback **200** deterministico e rapido em vez de deixar virar
  `5xx`

Fallback sugerido:

```json
{
  "approved": true,
  "fraud_score": 0.0
}
```

Justificativa:

- o proprio criterio de avaliacao penaliza `HTTP error` com `-5`
- no desafio, um fallback rapido custa menos que indisponibilidade

### O que nao fazer

Nao planejar resiliencia em cima de `panic hook` sozinho. Hook de panic nao
substitui controle explicito de fluxo nem garante resposta HTTP util.

O hot-path deve ser escrito para:

- nao panicar
- propagar `Result`
- converter falha conhecida em resposta controlada no boundary HTTP

---

## 10. Nginx

Configuracao sugerida:

```nginx
worker_processes 1;

events {
    worker_connections 1024;
    use epoll;
}

http {
    access_log off;
    keepalive_timeout 30;
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;

    upstream api {
        server api1:3000;
        server api2:3000;
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

Notas:

- round-robin simples atende a regra do desafio
- nenhuma logica de negocio no LB
- manter configuracao enxuta e previsivel

---

## 11. `docker-compose.yml`

Exemplo de branch `submission`:

```yaml
services:
  api1: &api
    image: ghcr.io/andrade/rinha2026-api:v1
    networks: [backend]
    expose:
      - "3000"
    deploy:
      resources:
        limits:
          cpus: "0.475"
          memory: "165MB"

  api2:
    <<: *api

  nginx:
    image: nginx:1.27-alpine
    depends_on: [api1, api2]
    volumes:
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
    networks: [backend]
    ports:
      - "9999:9999"
    deploy:
      resources:
        limits:
          cpus: "0.05"
          memory: "20MB"

networks:
  backend:
    driver: bridge
```

Observacoes:

- `bridge` respeita a restricao oficial
- `host` e `privileged` continuam proibidos
- a branch `submission` deve conter apenas os artefatos necessarios para
  execucao

---

## 12. Arquivos a criar

### Branch `main`

| Arquivo | Proposito |
|---------|-----------|
| `Cargo.toml` | dependencias e configuracao |
| `src/main.rs` | boot, rotas, readiness |
| `src/payload.rs` | structs do request/response |
| `src/vector.rs` | vetorizacao 14D |
| `src/parse.rs` | parse de timestamp e helpers |
| `src/refs.rs` | carga de `references.bin` e `labels.bin` |
| `src/knn.rs` | kernel escalar e opcional SIMD |
| `src/score.rs` | decisao final |
| `src/error.rs` | erros internos e fallback |
| `tools/prepare_refs.rs` | converte `.json.gz` em blobs binarios |
| `Dockerfile` | build da imagem |
| `nginx.conf` | configuracao do load balancer |
| `README.md` | execucao local e benchmark |

### Branch `submission`

| Arquivo | Conteudo |
|---------|----------|
| `docker-compose.yml` | stack final |
| `nginx.conf` | config do LB |
| `info.json` | metadata do participante |

### PR no repositorio oficial da Rinha

| Arquivo | Conteudo |
|---------|----------|
| `participants/andrade.json` | array com `id` e `repo` publico |

---

## 13. Validacao obrigatoria

Esta e a parte que mais faltava no plano anterior.

### 13.1. Testes unitarios de vetorizacao

Transformar os exemplos de [REGRAS_DE_DETECCAO.md](./REGRAS_DE_DETECCAO.md) em
testes automatizados para confirmar:

- vetor de 14 dimensoes na ordem correta
- clamp correto
- sentinela `-1.0` correto
- `approved` e `fraud_score` coerentes com os exemplos

### 13.2. Oracle offline sobre `test/test-data.json`

Antes da API HTTP existir, criar um verificador que:

1. leia `test/test-data.json`
2. para cada entrada, rode a classificacao local diretamente em processo
3. compare `approved` com `entry.info.expected_response.approved`

Meta:

- **100% de paridade na massa local atual**

Isso e muito mais valioso do que adivinhar se a API esta certa olhando so
alguns curls.

### 13.3. Paridade escalar vs SIMD

Se o kernel SIMD entrar:

- comparar top-5 e decisao final contra o kernel escalar em um conjunto de
  queries de teste
- nao aceitar nenhuma divergencia

### 13.4. Smoke HTTP

Subir stack e validar:

```bash
curl -sf http://localhost:9999/ready
curl -s -X POST http://localhost:9999/fraud-score ...
```

### 13.5. Carga local

Rodar:

```bash
./run.sh
```

e analisar:

- `http_errors`
- `p99`
- `raw_score`
- `final_score`

### 13.6. Volume de requests

Rodar:

```bash
./estimated-requests.sh
```

Na versao atual do repositorio, o script local totaliza **14.355 requests**.
Isso e util para planejar carga local, mas nao deve virar premissa rigida sobre
o teste final, porque a documentacao avisa que o script oficial pode variar.

### 13.7. Limites de recursos

Durante a carga:

```bash
docker stats --no-stream
```

Confirmar:

- RSS total abaixo de `350 MB`
- CPU total dentro de `1.0`

---

## 14. Critérios de aceitacao

| Metrica | Alvo |
|---------|------|
| `http_errors` | `0` |
| paridade offline na massa local | `100%` |
| `p99` | `<= 10 ms` |
| `latency_multiplier` | idealmente `1.0` |
| RSS total | `< 350 MB` |
| topologia | `nginx + 2 APIs` |

Se a versao correta escalar ja bater essas metas, ela pode ser a versao final.
Nao existe premio por complexidade desnecessaria.

---

## 15. Riscos e planos B

| Risco | Mitigacao |
|-------|-----------|
| busca escalar nao bate `p99` | adicionar AVX2 com `_mm256_loadu_ps` e manter fallback escalar |
| JSON aparece no perfil | testar `sonic-rs` contra `serde_json` com benchmark real |
| bug de vetorizacao | travar exemplos canonicos em testes unitarios |
| divergencia com a massa local | usar o oracle offline antes do HTTP |
| erro interno vira `5xx` | boundary HTTP com fallback 200 controlado |
| compose local ignora limites no seu ambiente | validar com `docker stats` e ajustar processo local sem mudar a submissao oficial |
| UDS parecer tentador cedo demais | manter TCP interno ate existir benchmark e estrategia de permissao fechada |
| CPU sem AVX2 | fallback escalar ou SSE somente se benchmark justificar |

---

## 16. Ordem recomendada de execucao

1. **Scaffold do projeto** em Rust com `GET /ready` stub.
2. **Ferramenta de preprocessamento** para gerar `references.bin` e `labels.bin`.
3. **Loader** dos blobs e estrutura `Refs`.
4. **Vetorizacao** com testes unitarios usando os exemplos oficiais.
5. **Kernel escalar exato** com top-5.
6. **Oracle offline** em cima de `test/test-data.json`.
7. **Handler `/fraud-score`** integrando tudo.
8. **Nginx + compose** com duas APIs.
9. **Carga local** com `./run.sh`.
10. **Profiling** para descobrir gargalo real.
11. **AVX2 opcional** se, e somente se, o gargalo continuar no kernel de busca.
12. **Publicacao da imagem**, branch `submission`, PR e issue `rinha/test`.

---

## 17. Submissao

Seguir [SUBMISSAO.md](./SUBMISSAO.md):

1. repo publico com branch `main`
2. branch `submission` com apenas arquivos de execucao
3. imagem publica em registry
4. PR no repositorio oficial adicionando `participants/andrade.json`
5. issue `rinha/test andrade-rust`

Exemplo de `info.json`:

```json
{
  "participants": ["Andrade"],
  "social": ["https://github.com/andrade"],
  "source-code-repo": "https://github.com/andrade/rinha-de-backend-2026",
  "stack": ["rust", "axum", "tokio", "nginx"],
  "open_to_work": true
}
```

---

## 18. Referencias principais

### Do proprio desafio

- [README.md](./README.md)
- [API.md](./API.md)
- [ARQUITETURA.md](./ARQUITETURA.md)
- [REGRAS_DE_DETECCAO.md](./REGRAS_DE_DETECCAO.md)
- [DATASET.md](./DATASET.md)
- [AVALIACAO.md](./AVALIACAO.md)
- [SUBMISSAO.md](./SUBMISSAO.md)
- [FAQ.md](./FAQ.md)

### Externas

- Docker Compose deploy resources
- Nginx upstream round-robin e upstream keepalive
- Rust intrinsics AVX2
- Faiss: `Flat` como baseline exato
- Qdrant: HNSW e aproximado, brute force quando a prioridade e exatidao

---

## 19. Resumo executivo

Se fosse para reduzir este plano a cinco decisoes:

1. **Rust + nginx + 2 APIs**
2. **dataset preprocessado e carregado em RAM**
3. **busca exata primeiro**
4. **paridade offline antes de benchmark HTTP**
5. **AVX2 e outras agressividades so com profiling e evidencia**

Esse e o plano mais forte, justo e tecnicamente defensavel para comecar a
Rinha com alta chance de score competitivo sem cair em complexidade fragil.

---

[← README principal](./README.md)
