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

## Rodada Investigativa Posterior

Objetivo: continuar os experimentos de performance sem aceitar mudanças marginais. Critério usado: só promover para produção quando o ganho aparecesse de forma sustentável no k6, não apenas no microbenchmark offline.

### Experimento 1: travessia de grupos sem `std::sort`

Hipótese: como o grouped pruning visita em média apenas ~2.8 grupos por consulta, calcular todos os lower bounds e selecionar incrementalmente o menor grupo ainda não visitado poderia ser mais barato que ordenar todos os grupos com `std::sort`.

Mudança testada:

- Adicionado modo `traversal` em `cpp/tools/benchmark_classifier.cpp`.
- Comparados três modos: `sort`, `select_min` e `unsorted`.
- A implementação de produção chegou a ser alterada para `select_min`, mas foi revertida depois do k6.

Microbenchmark offline:

```text
queries=3000 repeat=1 refs=100000 groups=117
order=global traversal=sort mismatches=0 ns_per_query=122367 rows_per_query=13625.2 groups_per_query=2.805 checksum=4983
order=global traversal=select_min mismatches=0 ns_per_query=121003 rows_per_query=13625.2 groups_per_query=2.805 checksum=4983
order=global traversal=unsorted mismatches=0 ns_per_query=298257 rows_per_query=33181.7 groups_per_query=6.37733 checksum=4983
order=group_local traversal=sort mismatches=0 ns_per_query=78617.6 rows_per_query=13625.2 groups_per_query=2.805 checksum=4983
order=group_local traversal=select_min mismatches=0 ns_per_query=73160.5 rows_per_query=13625.2 groups_per_query=2.805 checksum=4983
order=group_local traversal=unsorted mismatches=0 ns_per_query=248879 rows_per_query=33181.7 groups_per_query=6.37733 checksum=4983
```

k6 com `select_min` em produção:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.58ms | 5446.33 | 0 | 0 | 0 |
| 2 | 3.03ms | 5518.55 | 0 | 0 | 0 |
| 3 | 3.06ms | 5514.50 | 0 | 0 | 0 |

Validação após restauração do baseline `std::sort`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.99ms | 5398.81 | 0 | 0 | 0 |
| 2 | 3.06ms | 5514.18 | 0 | 0 | 0 |

Decisão: rejeitado em produção. O microbenchmark favoreceu `select_min`, mas o k6 real não mostrou ganho sustentável contra o baseline histórico aquecido (`p99 médio=2.86ms`, `final_score médio=5543.78`). O modo `unsorted` também foi rejeitado: exato, mas muito mais lento.

Aprendizado: reduzir custo algorítmico dentro do kernel não necessariamente reduz p99 quando o gargalo observado passa por HTTP, parse, scheduling e ruído de container. A ferramenta de benchmark foi mantida porque melhora a capacidade de testar hipóteses futuras sem tocar produção.

### Experimento 2: agrupamento adicional por `amount4`

Hipótese: adicionar 4 buckets de `amount` normalizado à chave de agrupamento poderia reduzir linhas escaneadas por consulta mantendo exatidão, pois o `amount` é uma dimensão discriminativa.

Microbenchmark offline de estratégias:

```text
queries=3000 repeat=1 refs=100000 baseline_ns_per_query=892943 rows_per_query=100000 checksum=4983
strategy=base order=group_local groups=117 mismatches=0 grouped_ns_per_query=75712.6 rows_per_query=13625.2 groups_per_query=2.805 checksum=4983
strategy=amount4 order=group_local groups=250 mismatches=0 grouped_ns_per_query=71506.2 rows_per_query=11830.5 groups_per_query=4.15067 checksum=4983
strategy=amount8 order=group_local groups=407 mismatches=0 grouped_ns_per_query=74881.7 rows_per_query=11293.2 groups_per_query=5.573 checksum=4983
strategy=hour order=group_local groups=1470 mismatches=0 grouped_ns_per_query=138591 rows_per_query=5455.92 groups_per_query=10.8647 checksum=4983
strategy=amount4_hour order=group_local groups=2243 mismatches=0 grouped_ns_per_query=179851 rows_per_query=3822.27 groups_per_query=16.6523 checksum=4983
strategy=amount4_hour_day order=group_local groups=7362 mismatches=0 grouped_ns_per_query=558472 rows_per_query=696.012 groups_per_query=19.738 checksum=4983
```

k6 com `amount4` em produção:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.78ms | 5422.76 | 0 | 0 | 0 |
| 2 | 3.03ms | 5519.25 | 0 | 0 | 0 |
| 3 | 2.96ms | 5529.15 | 0 | 0 | 0 |
| 4 | 3.00ms | 5522.72 | 0 | 0 | 0 |

Decisão: rejeitado por enquanto. O resultado é tecnicamente correto e o microbenchmark melhorou ~5.6%, mas o k6 aquecido ficou em média ~3.00ms, abaixo do baseline histórico aquecido de `2.86ms`. A mudança não é ruim o suficiente para descartar definitivamente, mas não é forte o bastante para substituir o baseline publicado.

Aprendizado: aumentar grupos reduz linhas escaneadas, mas também aumenta custo de lower bound, ordenação de grupos e pressão de cache. `amount4` segue como hipótese futura apenas se uma rodada A/B longa no mesmo estado de máquina mostrar diferença real.

### Experimento 3: parser sem cópia de `simdjson::padded_string`

Hipótese: reservar capacidade no buffer HTTP e chamar `simdjson::dom::parser::parse(const std::string&)` evitaria construir `simdjson::padded_string` por request. O maior payload local medido em `test/test-data.json` tem `634B`, então foram testados dois tamanhos de reserva.

Variante `reserve(2048)` combinada com `amount4`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.52ms | 5453.12 | 0 | 0 | 0 |
| 2 | 3.16ms | 5500.65 | 0 | 0 | 0 |
| 3 | 3.03ms | 5519.24 | 0 | 0 | 0 |

Variante `reserve(768)` combinada com `amount4`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.00ms | 5398.39 | 0 | 0 | 0 |
| 2 | 3.04ms | 5516.58 | 0 | 0 | 0 |

Decisão: rejeitado. A remoção da cópia teórica não pagou o custo de reservar/gerenciar buffer por request no caminho real. A versão de 2048B provavelmente aumentou pressão de cache/alocação; a versão 768B também não superou o baseline.

Aprendizado: evitar uma cópia pequena de payload JSON não é automaticamente ganho quando o custo extra aparece em alocação, cache e ciclo de vida do request. O parser atual com `padded_string` permanece mais previsível para este workload.

### Estado Final da Rodada

- Nenhuma mudança de hot path foi mantida em produção nesta rodada.
- O compose foi reconstruído de volta para o baseline `group_local` e `/ready` respondeu `204`.
- Foi mantida apenas a instrumentação em `cpp/tools/benchmark_classifier.cpp` para comparar traversal modes em futuras rodadas.
- Houve um erro ambiental temporário no rebuild (`TLS handshake timeout` ao consultar metadata do Docker Hub). A reconstrução foi refeita com `--pull never` usando cache local e concluiu com sucesso.

Validações executadas:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests rinha-backend-2026-cpp benchmark-classifier-cpp -j2
ctest --test-dir cpp/build --output-on-failure
git diff --check
docker compose config -q
docker compose up -d --build --force-recreate --pull never
curl -sS -o /dev/null -w '%{http_code}' http://localhost:9999/ready
./run.sh
```

Resultado das validações:

- Build local: OK.
- Testes C++: `1/1` passou.
- `git diff --check`: OK.
- Compose config: OK.
- Rebuild final com cache local: OK.
- `/ready`: `204`.

Próximas hipóteses recomendadas:

1. Investigar o overhead de alocação por request (`std::make_shared<RequestContext>` e `std::string body`) com alternativa segura de lifecycle no uWebSockets, mas só aplicar se houver forma clara de medir.
2. Criar um benchmark local específico de parse HTTP/payload para separar parser de classificador; k6 está agregando ruído demais para microdecisões.
3. Rodar A/B longo, no mesmo estado térmico da máquina, entre `base group_local` e `amount4 group_local` antes de reconsiderar `amount4`.
4. Evitar novas otimizações de kernel exato até aparecer evidência de que o kernel voltou a dominar o p99.
