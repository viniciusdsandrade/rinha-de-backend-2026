# Daily Report - 2026-04-21

## Contexto

Rodada de otimização local orientada exclusivamente a score, partindo do baseline já validado na implementação Rust publicada originalmente na branch `impl/rust-baseline`, depois renomeada para `submission`.

Ambiente e premissas desta rodada:

- foco em maximizar score local no `k6`
- manter `100%` de acurácia e `0` `http_errors`
- preservar o melhor estado conhecido caso uma hipótese piorasse o benchmark oficial
- não tocar arquivos fora do escopo da implementação

## Baseline preservado

Estado de referência mantido ao final da rodada:

- commit: `146843b`
- branch final: `submission`
- benchmark oficial local validado anteriormente:
  - `final_score`: `4775.36`
  - `raw_score`: `14230`
  - `p99`: `29.80ms`
  - `med`: `1.02ms`
  - `p90`: `1.63ms`
  - `max`: `64.13ms`
  - acurácia: `100%`
  - `http_errors`: `0`

## Hipóteses testadas hoje

### 1. Troca do runtime HTTP para servidor manual em `hyper`

Objetivo:

- reduzir overhead de `axum::Router` e extratores
- manter a topologia com `nginx stream` e duas APIs
- pré-montar respostas JSON estáticas no hot path HTTP

Mudanças temporárias testadas:

- `src/http.rs`
- `src/main.rs`
- `Cargo.toml`
- `Cargo.lock`
- `tests/http_api.rs`

Validação funcional:

- `cargo test` passou
- `oracle_check test/test-data.json --limit 5000`: `0` divergências

Sinal rápido observado:

- `ab -k -n 5000 -c 100 ... /fraud-score`
  - aproximadamente `1280.02 req/s`

Resultado oficial:

- `final_score`: `2267.71`
- `raw_score`: `13703`
- `p99`: `60.43ms`
- `med`: `1.17ms`
- `p90`: `11.57ms`
- `max`: `151.93ms`
- acurácia: `100%`
- `http_errors`: `0`

Decisão:

- rejeitada
- apesar de melhorar o `ab`, piorou fortemente o `k6` oficial

### 2. Build agressivo com `RUSTFLAGS=\"-C target-cpu=native\"`

Objetivo:

- testar ganho de performance apenas via compilação, sem mudar a arquitetura da aplicação

Mudança temporária testada:

- `Dockerfile`

Sinal rápido observado:

- `ab -k -n 5000 -c 100 ... /fraud-score`
  - aproximadamente `891.85 req/s`

Resultado oficial:

- `final_score`: `2128.18`
- `raw_score`: `13570`
- `p99`: `63.76ms`
- `med`: `1.26ms`
- `p90`: `21.96ms`
- `max`: `90.57ms`
- acurácia: `100%`
- `http_errors`: `0`

Decisão:

- rejeitada
- piorou tanto o sinal rápido quanto o benchmark oficial

### 3. Cache do modo de busca AVX2 no startup do `Classifier`

Objetivo:

- evitar consulta repetida a `supports_avx2()` em cada classificação
- manter a mesma topologia, o mesmo servidor HTTP e o mesmo load balancer

Mudança temporária testada:

- `src/classifier.rs`

Validação funcional:

- `cargo test` passou
- `oracle_check test/test-data.json --limit 5000`: `0` divergências

Sinal rápido observado:

- `ab -k -n 5000 -c 100 ... /fraud-score`
  - aproximadamente `1069.25 req/s`

Resultado oficial:

- `final_score`: `2056.21`
- `raw_score`: `13607`
- `p99`: `66.18ms`
- `med`: `1.26ms`
- `p90`: `21.00ms`
- `max`: `87.62ms`
- acurácia: `100%`
- `http_errors`: `0`

Decisão:

- rejeitada
- o ganho no `ab` não se sustentou no `k6` oficial

## Decisão final da rodada

Nenhuma hipótese desta rodada foi incorporada.

O repositório foi devolvido exatamente ao melhor baseline conhecido de implementação, sem mudanças pendentes nos arquivos versionados da solução. Permaneceram fora do escopo, e intactos, os arquivos não relacionados:

- `AGENTS.md`
- `docs/br/PLANO_IMPLEMENTACAO_2.md`

## Branching

Ao fim da rodada:

- a branch local `impl/rust-baseline` foi renomeada para `submission`
- `origin/submission` passou a apontar para o commit `146843b`
- a branch remota antiga `origin/impl/rust-baseline` foi removida

## Próximo passo sugerido

Testar hipóteses mais localizadas e menos disruptivas que atuem no hot path sem trocar a infraestrutura principal, por exemplo:

- respostas JSON estáticas mantendo `axum`
- refinamentos do parser/handler HTTP sem abandonar o runtime atual
- otimizações de layout/alinhamento no kernel SIMD e na organização das referências
