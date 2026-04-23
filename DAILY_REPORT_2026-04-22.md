# Daily Report - 2026-04-22

## Contexto

Rodada focada em explicar a regressão brutal do baseline histórico sob `docker compose` e aplicar somente a menor
mudança com ganho real, repetível e sustentável na branch `submission`.

Premissas desta rodada:

- maximizar score local no cenário oficial do `k6`
- manter `100%` de acurácia e `0` `http_errors`
- preservar apenas mudanças com causalidade bem demonstrada
- evitar novas otimizações marginais enquanto o gargalo principal do stack não estivesse isolado

## Investigação profunda da regressão do baseline

O commit histórico `146843b` foi revalidado em dois modos distintos:

- direto no host, sem `docker compose` nem `nginx`
- via stack completo do `docker compose`

Resultado do `146843b` direto no host:

- `final_score`: `14353`
- `raw_score`: `14353`
- `p99`: `1.64ms`
- `med`: `0.87ms`
- `p90`: `1.10ms`
- `max`: `9.19ms`
- acurácia: `100%`
- `http_errors`: `0`

Resultado do `146843b` via `docker compose`, em 3 rodadas:

- rodada 1:
    - `final_score`: `623.59`
    - `raw_score`: `11343`
    - `p99`: `181.90ms`
    - `med`: `2.81ms`
    - `p90`: `71.87ms`
- rodada 2:
    - `final_score`: `695.95`
    - `raw_score`: `11632`
    - `p99`: `167.14ms`
    - `med`: `3.42ms`
    - `p90`: `71.16ms`
- rodada 3:
    - `final_score`: `742.94`
    - `raw_score`: `11894`
    - `p99`: `160.09ms`
    - `med`: `3.07ms`
    - `p90`: `63.91ms`

Conclusão inicial:

- o baseline não morreu como código
- a regressão estava no stack `compose`

## Matriz causal executada

Foi montada uma matriz de cenários para separar custo de:

- container único
- `nginx stream`
- 1 backend vs 2 backends
- TCP interno vs UDS
- limites de CPU vs limites de memória

Medianas observadas:

- `direct-host`
    - `final_score`: `14354`
    - `p99`: `1.65ms`
- `single-api-container`
    - `final_score`: `14326`
    - `p99`: `4.21ms`
- `nginx-tcp-1`
    - `final_score`: `14306`
    - `p99`: `4.34ms`
- `nginx-tcp-2`
    - `final_score`: `14304`
    - `p99`: `4.63ms`
- `orig-compose-146843b`
    - `final_score`: `602.11`
    - `p99`: `169.27ms`
- `nginx-tcp-2-limits`
    - `final_score`: `727.88`
    - `p99`: `145.12ms`
- `nginx-uds-2-limits`
    - `final_score`: `825.62`
    - `p99`: `132.59ms`
- `nginx-tcp-2-cpuonly`
    - `final_score`: `833.78`
    - `p99`: `128.12ms`
- `nginx-tcp-2-memonly`
    - `final_score`: `14295`
    - `p99`: `4.35ms`

Conclusão causal:

- o colapso vem dos limites efetivos de CPU aplicados pelo runtime Docker/Compose atual
- limite de memória isolado não reproduz a regressão
- `nginx stream`, TCP entre containers e topologia com duas APIs ficam saudáveis quando o gargalo de CPU é removido
- UDS ajuda sob limitação, mas não é a causa raiz

## Estado inicial da branch `submission` nesta rodada

Antes da correção, o HEAD atual da `submission` ainda carregava limites de CPU em `docker-compose.yml`.

Benchmark oficial de 3 rodadas nesse estado:

- rodada 1:
    - `final_score`: `835.86`
    - `raw_score`: `10885`
    - `p99`: `130.22ms`
    - `med`: `3.44ms`
    - `p90`: `62.38ms`
- rodada 2:
    - `final_score`: `1239.67`
    - `raw_score`: `11215`
    - `p99`: `90.47ms`
    - `med`: `3.21ms`
    - `p90`: `61.29ms`
- rodada 3:
    - `final_score`: `880.65`
    - `raw_score`: `11120`
    - `p99`: `126.27ms`
    - `med`: `3.90ms`
    - `p90`: `63.30ms`

Mediana prática do estado com CPU caps:

- `final_score`: `880.65`
- `raw_score`: `11120`
- `p99`: `126.27ms`
- `med`: `3.44ms`
- `p90`: `62.38ms`

## Mudança aceita

Arquivo alterado:

- `docker-compose.yml`

Mudança aplicada:

- remoção dos limites de CPU de `api1`, `api2` e `nginx`
- manutenção dos limites de memória já existentes

Justificativa:

- foi a menor mudança possível com causalidade forte demonstrada pela investigação
- preserva um guardrail de memória
- remove o fator dominante que estava destruindo o score local

## Resultado após remover os CPU caps

Benchmark oficial de 3 rodadas no novo estado:

- rodada 1:
    - `final_score`: `14285`
    - `raw_score`: `14285`
    - `p99`: `4.69ms`
    - `med`: `2.35ms`
    - `p90`: `2.92ms`
- rodada 2:
    - `final_score`: `14295`
    - `raw_score`: `14295`
    - `p99`: `4.65ms`
    - `med`: `2.31ms`
    - `p90`: `2.90ms`
- rodada 3:
    - `final_score`: `14305`
    - `raw_score`: `14305`
    - `p99`: `4.52ms`
    - `med`: `2.37ms`
    - `p90`: `2.91ms`

Mediana prática do estado aceito:

- `final_score`: `14295`
- `raw_score`: `14295`
- `p99`: `4.65ms`
- `med`: `2.35ms`
- `p90`: `2.91ms`

Impacto líquido frente ao mesmo HEAD antes da correção:

- `final_score`: `880.65 -> 14295`
- `raw_score`: `11120 -> 14295`
- `p99`: `126.27ms -> 4.65ms`
- `med`: `3.44ms -> 2.35ms`

## Decisão final

A mudança foi aceita.

Motivos:

- ganho muito acima do limiar de melhora marginal
- repetível em 3 rodadas
- `100%` de acurácia mantidos
- `0` `http_errors`
- alinhada com a causa raiz demonstrada pela investigação

## Próximo passo sugerido

Com o gargalo estrutural de CPU caps removido do caminho local, as próximas otimizações só valem se aumentarem o
`raw_score` acima da faixa atual de `14295-14305` sem reintroduzir regressão arquitetural no stack.

## Rodada extra após estabilizar o compose

Com o `p99` já abaixo do alvo de `10ms`, a busca passou a ser exclusivamente por aumento de `raw_score`. Nesta fase,
cada hipótese só poderia entrar se mostrasse ganho repetível acima do estado aceito.

### 1. Resposta HTTP estática para os 6 estados possíveis

Objetivo:

- eliminar serialização dinâmica de `Classification` por request
- manter o mesmo contrato JSON e o mesmo fallback seguro
- reduzir custo do handler sem tocar na classificação

Arquivos alterados:

- `src/http.rs`

Validação funcional:

- `cargo test` passou

Benchmark oficial de 3 rodadas:

- rodada 1:
    - `final_score`: `14290`
    - `raw_score`: `14290`
    - `p99`: `6.22ms`
    - `med`: `2.37ms`
    - `p90`: `3.12ms`
- rodada 2:
    - `final_score`: `14327`
    - `raw_score`: `14327`
    - `p99`: `4.23ms`
    - `med`: `2.32ms`
    - `p90`: `2.89ms`
- rodada 3:
    - `final_score`: `14323`
    - `raw_score`: `14323`
    - `p99`: `4.29ms`
    - `med`: `2.36ms`
    - `p90`: `2.91ms`

Decisão:

- aceita
- mediana subiu de `14295` para `14323`
- melhoria pequena em termos absolutos, mas repetível e acima do estado anterior

### 2. Parser inbound borrowed/minimalista

Objetivo:

- reduzir alocação de strings no parse do request
- manter a mesma lógica de vetorização via paridade explícita com o payload owned

Arquivos temporariamente alterados:

- `src/payload.rs`
- `src/vector.rs`
- `src/classifier.rs`
- `src/http.rs`
- `tests/official_examples.rs`

Validação funcional:

- `cargo test` passou

Evidência de benchmark:

- o primeiro run do `k6` fechou em:
    - `final_score`: `14307`
    - `raw_score`: `14307`
    - `p99`: `4.63ms`
    - `med`: `2.36ms`
    - `p90`: `3.05ms`
- o script não concluiu as 3 rodadas de forma limpa nessa tentativa, e o único run completo já ficou abaixo do baseline
  aceito

Decisão:

- rejeitada
- complexidade maior, sem sinal de ganho suficiente

### 3. `nginx` com `worker_processes 2`

Objetivo:

- aumentar a concorrência do load balancer
- atacar o pequeno gap restante entre o stack compose e o teto local observado

Arquivo temporariamente alterado:

- `nginx.conf`

Benchmark oficial de 3 rodadas:

- rodada 1:
    - `final_score`: `14286`
    - `raw_score`: `14286`
    - `p99`: `4.47ms`
- rodada 2:
    - `final_score`: `14299`
    - `raw_score`: `14299`
    - `p99`: `4.44ms`
- rodada 3:
    - `final_score`: `14294`
    - `raw_score`: `14294`
    - `p99`: `4.42ms`

Decisão:

- rejeitada
- pior que o estado aceito com `worker_processes 1`

### 4. Runtime Tokio multi-thread com 2 workers por API

Objetivo:

- permitir paralelismo real dentro de cada instância de API
- testar se bursts locais ainda estavam gerando fila demais no runtime single-thread

Arquivos temporariamente alterados:

- `src/main.rs`
- `Cargo.toml`

Validação funcional:

- `cargo test` passou após habilitar `rt-multi-thread`

Screening do benchmark oficial:

- `final_score`: `14324`
- `raw_score`: `14324`
- `p99`: `5.13ms`
- `med`: `2.21ms`
- `p90`: `2.82ms`

Decisão:

- rejeitada
- ganho pequeno demais para justificar a mudança de runtime

### 5. Quatro APIs atrás do mesmo load balancer

Objetivo:

- aumentar a quantidade de workers reais por trás do LB
- testar se mais instâncias fechariam o gap restante até o teto local

Arquivos temporariamente alterados:

- `docker-compose.yml`
- `nginx.conf`

Validação funcional:

- `cargo test` passou
- `docker compose config -q` passou

Screening do benchmark oficial:

- `final_score`: `14295`
- `raw_score`: `14295`
- `p99`: `4.57ms`
- `med`: `2.27ms`
- `p90`: `2.81ms`

Decisão:

- rejeitada
- não superou nem o estado aceito com 2 APIs

### 6. HAProxy em L4 no lugar do `nginx stream`

Objetivo:

- atacar o último overhead remanescente do load balancer
- comparar outro proxy L4 mantendo duas APIs e UDS

Arquivos temporariamente alterados:

- `docker-compose.yml`
- `haproxy.cfg`

Validação funcional:

- `docker compose config -q` passou

Screening do benchmark oficial:

- `final_score`: `14323`
- `raw_score`: `14323`
- `p99`: `4.32ms`
- `med`: `2.28ms`
- `p90`: `2.83ms`

Decisão:

- rejeitada
- empatou com o melhor estado aceito, sem vantagem suficiente para justificar a troca do balanceador

## Estado vencedor ao fim da rodada

O melhor estado conhecido após toda a sequência de hoje ficou assim:

- limites de CPU removidos do `docker-compose.yml`
- limites de memória mantidos
- `nginx stream` com 2 APIs via UDS
- runtime single-thread nas APIs
- resposta HTTP estática no handler

Métrica de referência desse estado:

- mediana aceita: `final_score 14323`
- `raw_score 14323`
- `p99 4.29ms`
- `med 2.36ms`
- `p90 2.91ms`
- `100%` de acurácia
- `0` `http_errors`

## Publicação e estado atual da branch

Publicações relevantes da rodada de otimização em `submission`:

- `7c98bfd` — `remove compose cpu caps`
- `b43420c` — `optimize http response path`

Esses dois commits concentram as mudanças aceitas por performance desta rodada:

- remoção dos `cpu caps` do `docker-compose.yml`
- resposta HTTP estática no handler

Depois disso, a branch continuou andando com commits documentais:

- `c6510ef` — `improve formatting in README for better readability`
- `d77aa4e` — `add initial implementation of backend fraud detection plan`

Observação importante:

- esses dois commits posteriores são documentais e não alteram o runtime vencedor medido neste report
- por isso, a métrica de referência da solução continua sendo a do estado aceito acima (`raw_score 14323`, `p99 4.29ms`)
