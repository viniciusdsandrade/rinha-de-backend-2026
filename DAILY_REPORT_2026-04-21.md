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

## Publicação

Após o fechamento desta rodada:

- o daily report foi movido para a raiz do repositório em `DAILY_REPORT_2026-04-21.md`
- a branch `submission` foi publicada no fork `viniciusdsandrade/rinha-de-backend-2026`
- foi aberto o PR `#1` de `submission` para `main` no próprio fork:
  - `https://github.com/viniciusdsandrade/rinha-de-backend-2026/pull/1`

## Próximo passo sugerido

Testar hipóteses mais localizadas e menos disruptivas que atuem no hot path sem trocar a infraestrutura principal, por exemplo:

- respostas JSON estáticas mantendo `axum`
- refinamentos do parser/handler HTTP sem abandonar o runtime atual
- otimizações de layout/alinhamento no kernel SIMD e na organização das referências

## Rodada extra de hipóteses sustentáveis

Após a publicação do baseline estável em `submission`, foi iniciada uma nova rodada estritamente orientada a ganhos concretos e sustentáveis, seguindo a ordem de hipóteses priorizadas por evidência técnica externa.

### 4. Payload borrowed/zero-copy com `serde` + `sonic-rs`

Objetivo:

- reduzir alocação e cópia de strings por request
- manter o runtime HTTP atual e a lógica de classificação inalterada
- atacar o custo de desserialização sem mexer na regra de fraude

Mudanças temporárias testadas:

- `src/payload.rs`
- `src/lib.rs`
- `src/classifier.rs`
- `src/http.rs`
- `src/oracle.rs`
- `src/vector.rs`
- `tests/http_api.rs`
- `tests/official_examples.rs`
- `tests/search_parity.rs`

Validação funcional:

- `cargo test` passou
- `./target/release/oracle_check test/test-data.json --limit 5000`: `0` divergências

Sinal rápido observado:

- `ab -k -n 5000 -c 100 ... /fraud-score`
  - aproximadamente `982.22 req/s`

Resultado oficial:

- `final_score`: `1468.79`
- `raw_score`: `13010`
- `p99`: `88.58ms`
- `med`: `1.40ms`
- `p90`: `42.16ms`
- `max`: `171.31ms`
- acurácia: `100%`
- `http_errors`: `0`

Decisão:

- rejeitada
- embora funcionalmente correta, a hipótese piorou de forma severa o benchmark oficial e não atende ao critério de melhora sustentável

### 5. `nginx` em `http` upstream com `keepalive`

Objetivo:

- trocar o proxy `stream` por upstream HTTP com pool keepalive real entre `nginx` e as APIs
- reduzir churn de conexão entre load balancer e aplicação
- manter a mesma topologia de duas instâncias de API atrás do proxy

Mudanças temporárias testadas:

- `nginx.conf`
- `docker-compose.yml`

Observação de ambiente:

- a porta local `9999` ficou presa por um `docker-proxy` órfão fora do alcance de encerramento do usuário atual
- para não interromper a rodada, a medição desta hipótese foi feita temporariamente no host `10099`, mantendo o mesmo tráfego HTTP contra o `nginx`

Validação funcional:

- `cargo test` passou
- `docker --context default compose config -q` passou
- `curl http://127.0.0.1:10099/ready`: `204`

Sinal rápido observado:

- `ab -k -n 5000 -c 100 ... /fraud-score`
  - aproximadamente `478.92 req/s`

Evidência sob carga oficial:

- primeira execução do `k6` com script temporário apontando para `10099` mostrou `connection refused` em massa e o stack chegou a aparecer todo como `Exited (255)` em uma checagem subsequente
- após religar o stack, novas execuções do `k6` continuaram instáveis e o próprio processo `k6` foi encerrado pelo sistema antes de gravar `results.json`
- por isso, esta hipótese não produziu um `final_score` confiável e comparável; o resultado operacional ficou pior do que o baseline a ponto de inviabilizar a medição oficial completa

Decisão:

- rejeitada
- o `ab` já caiu para menos da metade do sinal observado no baseline forte e o benchmark oficial ficou operacionalmente instável

### 6. Layout alinhado e padded para o kernel AVX2

Objetivo:

- adicionar blocos alinhados em 32 bytes para o caminho AVX2
- eliminar o tail escalar do hot path vetorizado
- permitir uso de `_mm256_load_ps` em vez de `_mm256_loadu_ps`

Mudanças temporárias testadas:

- `src/refs.rs`
- `src/classifier.rs`

Validação funcional:

- `cargo test` passou
- `cargo test -q` passou
- `./target/release/oracle_check test/test-data.json --limit 5000`: `0` divergências
- `docker --context default compose config -q` passou
- `curl http://127.0.0.1:10099/ready`: `204`

Sinal rápido observado:

- `ab -k -n 5000 -c 100 ... /fraud-score`
  - aproximadamente `707.06 req/s`

Resultado oficial:

- `final_score`: `705.62`
- `raw_score`: `11612`
- `p99`: `164.56ms`
- `med`: `2.83ms`
- `p90`: `71.45ms`
- `max`: `452.00ms`
- acurácia: `100%`
- `http_errors`: `0`

Decisão:

- rejeitada
- apesar da correção técnica do alinhamento, o comportamento global piorou drasticamente no benchmark oficial

## Rodada autônoma noturna

Com a branch `submission` já contendo as correções válidas da revisão do PR no commit `247dcb6`, foi iniciada uma rodada autônoma e sequencial de otimização seguindo esta ordem:

1. re-freezar baseline no `247dcb6` com `3x k6`
2. parser seletivo + tipos compactos
3. kernel exato com normas pré-computadas + dot product
4. PDE exata com early abort
5. PGO sobre o melhor estado vindo de `2-4`
6. UDS no `nginx stream`
7. `mimalloc` como screening tardio

### 7. Re-freeze do baseline no `247dcb6`

Objetivo:

- medir o estado real atual da branch antes de qualquer nova hipótese
- checar se a regressão frente ao baseline histórico `146843b` vinha mesmo das mudanças recentes do PR
- fixar uma linha de base comparável para aceitar ou rejeitar as próximas otimizações

Observações importantes:

- o baseline histórico forte do dia continuava sendo o commit `146843b`, com `final_score 4775.36` e `p99 29.80ms`
- no ambiente desta rodada, o `247dcb6` ficou muito abaixo desse histórico
- a única mudança com potencial de impacto em runtime entre `146843b` e `247dcb6` era a adição de `cpus` e `mem_limit` no `docker-compose.yml`

Teste de hipótese sobre as mudanças recentes do PR:

- foi revertida localmente apenas a adição de `cpus` e `mem_limit` em `api1`, `api2` e `nginx`
- essa reversão ajudou apenas marginalmente
- conclusão: a piora grande em relação ao baseline histórico não foi explicada de forma suficiente pelas mudanças recentes do PR, embora a reversão local continue ligeiramente melhor para score

Resultado oficial do re-freeze já com essa reversão local do `docker-compose`:

- rodada 1:
  - `final_score`: `1670.26`
  - `raw_score`: `13424`
  - `p99`: `80.37ms`
  - `med`: `1.57ms`
  - `p90`: `11.67ms`
- rodada 2:
  - `final_score`: `1663.11`
  - `raw_score`: `13668`
  - `p99`: `82.18ms`
  - `med`: `1.59ms`
  - `p90`: `20.03ms`
- rodada 3:
  - `final_score`: `1895.63`
  - `raw_score`: `13725`
  - `p99`: `72.40ms`
  - `med`: `1.48ms`
  - `p90`: `13.20ms`

Linha de base adotada para esta rodada:

- `final_score` mediano aproximado: `1670.26`
- `p99` mediano aproximado: `80.37ms`
- acurácia: `100%`
- `http_errors`: `0`

Decisão:

- baseline local re-freezado com sucesso
- as próximas hipóteses passaram a ser avaliadas contra este estado real, não contra o histórico excepcional do `146843b`

### 8. Parser seletivo + tipos compactos

Objetivo:

- evitar desserialização completa do payload
- deixar de materializar campos inúteis para a vetorização
- atacar alocação e parsing por request mantendo a regra de fraude intacta

Mudanças temporárias testadas:

- `src/hot_payload.rs`
- `src/http.rs`
- `src/lib.rs`
- `src/classifier.rs`
- `src/vector.rs`
- `tests/http_api.rs`

Estratégia:

- extração seletiva com `sonic-rs`
- tratamento específico de `last_transaction: null`
- separação entre erro estrutural de request e erro semântico com fallback seguro onde já havia contrato explícito

Validação funcional:

- foi criado antes um teste de regressão para garantir que o endpoint ignorasse o tipo de `id` quando o campo não fosse usado pelo classificador
- `cargo test` passou após a implementação
- `./target/release/oracle_check test/test-data.json --limit 5000`: `0` divergências

Resultado oficial:

- rodada 1:
  - `final_score`: `1599.57`
  - `raw_score`: `13689`
  - `p99`: `85.58ms`
  - `med`: `1.57ms`
  - `p90`: `9.40ms`
- rodada 2:
  - `final_score`: `1420.15`
  - `raw_score`: `13509`
  - `p99`: `95.12ms`
  - `med`: `1.64ms`
  - `p90`: `22.71ms`
- rodada 3:
  - `final_score`: `1832.27`
  - `raw_score`: `13748`
  - `p99`: `75.03ms`
  - `med`: `1.54ms`
  - `p90`: `17.50ms`

Decisão:

- rejeitada
- apesar da correção funcional e da paridade total com o oracle, o ganho não se sustentou; na mediana, o score ficou abaixo do baseline re-freezado

### 9. Kernel exato com normas pré-computadas + dot product

Objetivo:

- reescrever a distância euclidiana exata para a forma `||q||² + ||x||² - 2 q·x`
- pré-computar `||x||²` no carregamento das referências
- reduzir trabalho aritmético repetido no núcleo de busca sem perder exatidão

Mudanças incorporadas nesta etapa:

- `src/refs.rs`
- `src/classifier.rs`
- `tests/refs_binary.rs`

Estratégia:

- cálculo e armazenamento de `norms` dentro de `ReferenceSet`
- acesso a `squared_norm`
- novo caminho de distância com `query_norm` pré-calculada
- hot path AVX2 reescrito para usar dot product e norma pré-computada

Validação funcional:

- foi criado antes um teste de regressão garantindo que a norma quadrática pré-computada casasse com a soma manual
- `cargo test` passou
- `./target/release/oracle_check test/test-data.json --limit 5000`: `0` divergências

Resultado oficial:

- rodada 1:
  - `final_score`: `1571.32`
  - `raw_score`: `13593`
  - `p99`: `86.51ms`
  - `med`: `1.65ms`
  - `p90`: `14.94ms`
- rodada 2:
  - `final_score`: `1807.88`
  - `raw_score`: `13720`
  - `p99`: `75.89ms`
  - `med`: `1.60ms`
  - `p90`: `9.76ms`
- rodada 3:
  - `final_score`: `1764.54`
  - `raw_score`: `13725`
  - `p99`: `77.78ms`
  - `med`: `1.54ms`
  - `p90`: `10.05ms`

Decisão:

- aceita
- houve melhora mediana sustentável frente ao baseline re-freezado, com queda do `p99` e preservação total da acurácia

### 10. PDE exata com early abort

Objetivo:

- reduzir trabalho no flat exact scan sem sair do regime exato
- explorar acumulação parcial da distância e interromper cedo candidatos ou chunks já piores do que o quinto melhor atual
- ordenar dimensões pelo maior poder discriminativo primeiro

Mudanças incorporadas nesta etapa:

- `src/refs.rs`
- `src/classifier.rs`
- `tests/refs_binary.rs`

Estratégia:

- cálculo de uma ordem fixa de dimensões baseada em variância
- novo método `distance_squared_if_below`
- pruning escalar por limiar parcial
- pruning vetorizado por chunk AVX2 quando todas as 8 lanes já ultrapassam o threshold corrente

Validação funcional:

- foi criado antes um teste de regressão garantindo que a distância limitada retornasse `None` quando o threshold fosse pequeno demais
- `cargo test` passou
- `./target/release/oracle_check test/test-data.json --limit 5000`: `0` divergências

Resultado oficial:

- rodada 1:
  - `final_score`: `2641.93`
  - `raw_score`: `13885`
  - `p99`: `52.56ms`
  - `med`: `1.64ms`
  - `p90`: `4.05ms`
- rodada 2:
  - `final_score`: `1489.84`
  - `raw_score`: `13678`
  - `p99`: `91.81ms`
  - `med`: `1.80ms`
  - `p90`: `23.34ms`
- rodada 3:
  - `final_score`: `2323.47`
  - `raw_score`: `13811`
  - `p99`: `59.44ms`
  - `med`: `1.59ms`
  - `p90`: `4.26ms`

Decisão:

- aceita provisoriamente
- houve variância alta, mas a mediana melhorou de forma relevante frente ao estado da etapa 9 e dois dos três cenários ficaram muito acima do baseline re-freezado
- este passou a ser o melhor estado corrente antes do item 5 da sequência
