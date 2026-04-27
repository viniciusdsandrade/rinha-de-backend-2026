# Daily Report 2026-04-27

Branch ativa: `submission-2`.

Objetivo da rodada: continuar a validação da hipótese `group_local` iniciada em `2026-04-26`, fechando o bloco planejado de 9 execuções do k6 com carga leve e sem alterar a implementação.

## Contexto Herdado

Estado técnico vindo do encerramento de `2026-04-26`:

| Item | Valor |
|---|---:|
| Commit de implementação `group_local` | `ba01c42` |
| Commit do report final de 2026-04-26 | `d20b3fe` |
| Melhor resultado acumulado antes de hoje | `p99=2.69ms`, `final_score=5570.78` |
| Bloco `group_local` antes de hoje | 7 runs |
| Média `group_local` antes de hoje | `p99=2.99ms`, `final_score=5528.84` |
| Erros acumulados antes de hoje | `FP=0`, `FN=0`, `HTTP=0` |

Referências de comparação:

| Baseline | p99 médio | final_score médio | Observação |
|---|---:|---:|---|
| Baseline aceito de 2026-04-25 | 4.04ms | 5393.78 | Stack C++ antes do grouped pruning |
| Grouped pruning com ordem global | 3.31ms | 5481.36 | 9 runs em 2026-04-26 |

## Experimentos

| Horário | Hipótese | Mudança | Evidência | Decisão | Aprendizado |
|---|---|---|---|---|---|
| 12:26 | O estado publicado em `submission-2` ainda está alinhado ao binário `group_local` e pode ser reamostrado sem rebuild custoso | `docker compose up -d --build` em baixa prioridade (`nice/ionice`); build ficou em cache | `/ready=204`; containers `api1`, `api2` e `nginx` iniciados | Prosseguir para k6 | O ambiente local foi restaurado sem recompilação real relevante, mantendo comparabilidade com o estado publicado. |
| 12:28 | Fechar run 8 do bloco `group_local` | `./run.sh` com `nice -n 10 ionice -c3` | `p99=2.99ms`, `final_score=5524.52`, `FP=0`, `FN=0`, `HTTP=0` | Manter hipótese | Resultado acima do baseline global e sem regressão de detecção. |
| 12:29 | Fechar run 9 do bloco `group_local` | Segunda execução consecutiva do `./run.sh` no mesmo estado do compose | `p99=3.04ms`, `final_score=5517.46`, `FP=0`, `FN=0`, `HTTP=0` | Considerar bloco `group_local` fechado | O run 9 ficou um pouco mais lento que os melhores da noite, mas ainda sustentou vantagem clara sobre o baseline aceito e sobre a média do grouped pruning global. |

## Resultado Consolidado

Bloco completo `group_local` com 9 execuções:

| Run | p99 | final_score | FP | FN | HTTP errors |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.04ms | 5393.66 | 0 | 0 | 0 |
| 2 | 2.86ms | 5544.16 | 0 | 0 | 0 |
| 3 | 2.90ms | 5537.86 | 0 | 0 | 0 |
| 4 | 2.79ms | 5553.98 | 0 | 0 | 0 |
| 5 | 2.73ms | 5563.18 | 0 | 0 | 0 |
| 6 | 2.90ms | 5538.28 | 0 | 0 | 0 |
| 7 | 2.69ms | 5570.78 | 0 | 0 | 0 |
| 8 | 2.99ms | 5524.52 | 0 | 0 | 0 |
| 9 | 3.04ms | 5517.46 | 0 | 0 | 0 |

Médias consolidadas:

| Amostra | p99 médio | final_score médio | Observação |
|---|---:|---:|---|
| `group_local` 9x | 2.99ms | 5527.10 | Inclui o primeiro run frio de `4.04ms` |
| `group_local` 8x sem o primeiro run | 2.86ms | 5543.78 | Melhor proxy do estado aquecido |

Comparação contra baselines:

| Comparação | Delta p99 médio | Delta final_score médio | Leitura |
|---|---:|---:|---|
| `group_local` 9x vs baseline aceito 5x | -1.05ms, ~26.0% melhor | +133.32 pontos | Ganho material e sustentável no bloco completo |
| `group_local` 9x vs grouped global 9x | -0.32ms, ~9.7% melhor | +45.74 pontos | Ordem por grupo melhora a versão agrupada anterior |
| Melhor `group_local` vs melhor baseline aceito | -1.23ms, ~31.4% melhor | +163.57 pontos | Melhor run local conhecido da branch |

## Estado Atual

- A implementação `group_local` segue como melhor candidata técnica no stack C++.
- O primeiro run de `4.04ms` deve ser tratado como provável run frio/outlier de aquecimento, porque os 8 runs seguintes ficaram entre `2.69ms` e `3.04ms`.
- A precisão permaneceu intacta em todas as execuções: `0 FP`, `0 FN`, `0 HTTP errors`.
- Nenhuma mudança de código foi feita hoje; a rodada foi exclusivamente de validação e documentação.

## Validações Executadas

```text
docker compose up -d --build
curl -sS -o /dev/null -w '%{http_code}\n' http://localhost:9999/ready
./run.sh  # 2 execuções controladas via nice/ionice
git diff --check
```

Resultados:

- `/ready`: HTTP 204.
- k6 run 8: `p99=2.99ms`, `final_score=5524.52`, `FP=0`, `FN=0`, `HTTP=0`.
- k6 run 9: `p99=3.04ms`, `final_score=5517.46`, `FP=0`, `FN=0`, `HTTP=0`.
- `git diff --check`: sem problemas.

## Próximos Passos

1. Congelar `group_local` como baseline atual de performance da `submission-2`.
2. Próxima investigação de baixo risco: reduzir overhead da ordenação dos grupos por query, possivelmente substituindo `std::sort` por seleção parcial ou scan ordenado estável, mas somente se o benchmark offline e o k6 mostrarem ganho consistente.
3. Manter qualquer nova hipótese sob comparação contra `group_local 9x`, não contra o baseline antigo.
