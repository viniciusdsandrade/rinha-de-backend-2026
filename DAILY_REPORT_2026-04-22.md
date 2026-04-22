# Daily Report - 2026-04-22

## Contexto

Rodada focada em explicar a regressão brutal do baseline histórico sob `docker compose` e aplicar somente a menor mudança com ganho real, repetível e sustentável na branch `submission`.

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

Com o gargalo estrutural de CPU caps removido do caminho local, as próximas otimizações só valem se aumentarem o `raw_score` acima da faixa atual de `14295-14305` sem reintroduzir regressão arquitetural no stack.
