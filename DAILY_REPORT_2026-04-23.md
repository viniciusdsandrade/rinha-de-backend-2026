# Daily Report - 2026-04-23

## Contexto

Rodada focada em deixar a implementação Rust mais competitiva depois de a branch `submission` já ter atingido a faixa
de teto do benchmark oficial local.

Premissas efetivas desta rodada:

- manter a topologia oficial da submissão: `nginx stream` + 2 APIs + rede `bridge`
- manter `100%` de acurácia e `0` `http_errors`
- assumir AVX2/FMA como requisito efetivo
- aceitar somente mudanças pequenas, mensuráveis e sustentáveis
- preservar a comparação por benchmark oficial local em rodadas repetidas

O teto prático de requisições do cenário `test/test.js` é `14355`, conforme `estimated-requests.sh`.

## Experimento 1 - substituir `axum` por servidor HTTP manual

Objetivo:

- remover overhead de framework no hot path
- manter o mesmo parser JSON, a mesma classificação e o mesmo contrato HTTP
- responder com buffers HTTP estáticos para `/ready`, erros e os 6 resultados possíveis de score

Arquivos alterados:

- `Cargo.toml`
- `Cargo.lock`
- `src/http.rs`
- `src/main.rs`
- `tests/http_api.rs`

Commit:

- `d3b6f1b` - `replace axum with manual http server`

Validação funcional:

- `cargo test`: passou
- `git diff --check`: passou
- `docker compose config -q`: passou

Benchmark inicial de 3 rodadas:

- diretório: `/tmp/rinha-bench-rust-manual-http-20260423-173600`
- rodada 1:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.16ms`
    - `p90`: `1.45ms`
    - `med`: `1.06ms`
    - `max`: `9.35ms`
- rodada 2:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `1.71ms`
    - `p90`: `0.95ms`
    - `med`: `0.58ms`
    - `max`: `8.08ms`
- rodada 3:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.05ms`
    - `p90`: `1.48ms`
    - `med`: `1.05ms`
    - `max`: `8.02ms`

Benchmark promovido de 5 rodadas:

- diretório: `/tmp/rinha-bench-rust-manual-http-5x-20260423-174030`
- mediana:
    - `final_score`: `14354`
    - `raw_score`: `14354`
    - `p99`: `2.11ms`
    - `p90`: `1.53ms`
    - `med`: `1.11ms`
    - pior `p99`: `2.45ms`
    - pior `max`: `8.56ms`
    - `http_errors`: `0`

Benchmark pós-commit de 3 rodadas:

- diretório: `/tmp/rinha-bench-rust-manual-http-postcommit-20260423-175458`
- rodada 1:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `1.97ms`
    - `p90`: `1.49ms`
    - `med`: `1.12ms`
    - `max`: `4.35ms`
- rodada 2:
    - `final_score`: `14351`
    - `raw_score`: `14351`
    - `p99`: `2.04ms`
    - `p90`: `1.50ms`
    - `med`: `1.12ms`
    - `max`: `14.80ms`
- rodada 3:
    - `final_score`: `14354`
    - `raw_score`: `14354`
    - `p99`: `1.81ms`
    - `p90`: `1.35ms`
    - `med`: `1.01ms`
    - `max`: `6.48ms`

Decisão:

- aceita
- reduziu latência do stack Rust de forma relevante
- manteve acurácia e erros em `0`
- ainda apresentou pequena variação de raw score em rodada isolada, então a próxima hipótese passou a mirar consistência
  no teto `14355`

## Experimento 2 - compilar a imagem Rust para `x86-64-v3`

Objetivo:

- permitir que o compilador assuma o nível de CPU compatível com AVX2/FMA
- evitar `target-cpu=native`, que seria menos portátil e mais dependente da máquina exata
- testar melhoria somente via build flag, sem alterar contrato, algoritmo ou topologia

Arquivo alterado:

- `Dockerfile`

Mudança:

- `ENV RUSTFLAGS="-C target-cpu=x86-64-v3"` no estágio builder

Commit:

- `bd17404` - `compile rust image for x86-64-v3`

Validação:

- `rustc --print target-cpus`: confirmou suporte a `x86-64-v3`
- `git diff --check`: passou
- `docker compose config -q`: passou
- benchmark oficial local de 3 rodadas passou com build completo da imagem

Benchmark de 3 rodadas:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-20260423-175940`
- rodada 1:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.07ms`
    - `p90`: `0.88ms`
    - `med`: `0.61ms`
    - `max`: `14.09ms`
- rodada 2:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `1.85ms`
    - `p90`: `1.41ms`
    - `med`: `0.88ms`
    - `max`: `5.95ms`
- rodada 3:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.04ms`
    - `p90`: `1.39ms`
    - `med`: `0.65ms`
    - `max`: `6.63ms`

Mediana:

- `final_score`: `14355`
- `raw_score`: `14355`
- `p99`: `2.04ms`
- `p90`: `1.39ms`
- `med`: `0.65ms`
- pior `p99`: `2.07ms`
- pior `max`: `14.09ms`
- `http_errors`: `0`

Comparação direta contra o baseline pós-commit imediatamente anterior:

- `final_score` mediano: `14354 -> 14355`
- `raw_score` mediano: `14354 -> 14355`
- `p99` mediano: `1.97ms -> 2.04ms`
- `p90` mediano: `1.49ms -> 1.39ms`
- `med` mediano: `1.12ms -> 0.65ms`
- pior queda de raw score observada: `14351 -> 14355`

Decisão:

- aceita
- melhorou a consistência no teto do benchmark oficial local em 3/3 rodadas
- reduziu fortemente a mediana de latência
- o pequeno aumento de `p99` mediano é irrelevante para score enquanto permanece muito abaixo do alvo de `10ms`

## Estado final

Branch:

- `submission`

Commits locais adicionados hoje:

- `d3b6f1b` - `replace axum with manual http server`
- `bd17404` - `compile rust image for x86-64-v3`

Estado frente ao remoto:

- branch local está à frente de `origin/submission`

Observação:

- os commits ainda não foram publicados nesta rodada
- o Git reportou aviso de `gc.log` e objetos soltos durante commits; isso é manutenção local do repositório e não altera a
  implementação nem os resultados de benchmark

## Próximo passo sugerido

O próximo ganho sustentável em Rust provavelmente não virá de latência HTTP genérica, porque o `p99` já está muito abaixo
do alvo e o score já bate no teto. As próximas hipóteses devem focar em consistência de `raw_score` e reduzir variância:

- rodar um `5x` adicional no estado `x86-64-v3` para confirmar que o `14355` se sustenta fora do primeiro `3x`
- comparar `nginx stream` UDS atual contra ajustes mínimos de `worker_connections`, `backlog` e timeouts, sempre com A/B
  no mesmo daemon
- só voltar ao parser JSON se houver microbench e benchmark oficial demonstrando queda de iterações associada ao tempo de
  CPU do backend, porque parser seletivo aumenta risco de bug de contrato
