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

## Rodada Investigativa: custo de request e busca aproximada por orçamento de grupos

Objetivo: separar custo de corpo/JSON/vetorização/classificação antes de mexer novamente no hot path de produção. A hipótese a validar era se ainda valia insistir em parser/alocação ou se o gargalo real tinha voltado claramente para o KNN.

### Microbenchmark de request

Mudança instrumental:

- Adicionado `cpp/tools/benchmark_request.cpp`.
- Adicionado target CMake `benchmark-request-cpp` como `EXCLUDE_FROM_ALL`.
- O benchmark usa `test/test-data.json`, minifica cada `request` para simular o corpo enviado pelo k6 e mede etapas isoladas: append do body, parse DOM, parse do payload, parse+vectorize e parse+classify.
- Nenhuma mudança de produção foi feita.

Comando de build:

```text
nice -n 10 ionice -c3 cmake --build cpp/build --target benchmark-request-cpp -j2
```

Resultado: build OK. Houve apenas warnings já conhecidos do single-header vendorizado do `simdjson`.

Triagem leve:

```text
nice -n 10 ionice -c3 cpp/build/benchmark-request-cpp test/test-data.json resources/references.json.gz 5 3000
```

Resultado:

```text
samples=3000 repeat=5 avg_body_bytes=432.922 max_body_bytes=468
body_append_default_ns_per_query=23.9168 checksum=6493835
body_append_reserve768_ns_per_query=15.923 checksum=6493835
dom_padded_parse_ns_per_query=223.671 checksum=6493835
dom_reserve768_parse_ns_per_query=224.145 checksum=6493835
parse_payload_ns_per_query=595.916 checksum=1009914138760885
parse_vectorize_ns_per_query=626.338 checksum=10270221083520
parse_classify_ns_per_query=27289 checksum=59850
```

Rodada completa:

```text
nice -n 10 ionice -c3 cpp/build/benchmark-request-cpp test/test-data.json resources/references.json.gz 10 0
```

Resultado:

```text
samples=14500 repeat=10 avg_body_bytes=433.529 max_body_bytes=471
body_append_default_ns_per_query=19.3698 checksum=62861670
body_append_reserve768_ns_per_query=14.5428 checksum=62861670
dom_padded_parse_ns_per_query=231.774 checksum=62861670
dom_reserve768_parse_ns_per_query=232.655 checksum=62861670
parse_payload_ns_per_query=592.512 checksum=9878896953920980
parse_vectorize_ns_per_query=653.227 checksum=99324863360270
parse_classify_ns_per_query=28246.9 checksum=577480
```

Decisão: não priorizar parser/cópia/alocação de body nesta etapa. O ganho máximo visível em `reserve(768)` é de ~5 ns no append do corpo, enquanto o caminho `parse_classify` fica em ~28.247 ns por request no microbenchmark. `parse_payload` fica em ~593 ns e `parse_vectorize` em ~653 ns; portanto, JSON e vetorização somados representam uma fração pequena do custo total.

Aprendizado: a hipótese de otimizar `simdjson::padded_string`, reserva do body ou parse DOM deve ficar congelada até haver uma evidência nova no k6. O foco sustentável volta para reduzir trabalho do KNN ou mudar a estratégia de busca.

### Revalidação leve de travessia do classificador

Uma tentativa inicial de rodar `traversal` sobre a massa completa foi interrompida por custo alto de validação. A rodada foi repetida com amostra de 1000 queries para manter a máquina estável.

Comando:

```text
nice -n 10 ionice -c3 cpp/build/benchmark-classifier-cpp resources/references.json.gz test/test-data.json 5 1000 traversal
```

Resultado:

```text
queries=1000 repeat=5 refs=100000 groups=117
order=global traversal=sort mismatches=0 ns_per_query=122787 rows_per_query=13469.8 groups_per_query=2.812 checksum=8690
order=global traversal=select_min mismatches=0 ns_per_query=119680 rows_per_query=13469.8 groups_per_query=2.812 checksum=8690
order=global traversal=unsorted mismatches=0 ns_per_query=300447 rows_per_query=32759.1 groups_per_query=6.375 checksum=8690
order=group_local traversal=sort mismatches=0 ns_per_query=78365 rows_per_query=13469.8 groups_per_query=2.812 checksum=8690
order=group_local traversal=select_min mismatches=0 ns_per_query=76466.2 rows_per_query=13469.8 groups_per_query=2.812 checksum=8690
order=group_local traversal=unsorted mismatches=0 ns_per_query=256402 rows_per_query=32759.1 groups_per_query=6.375 checksum=8690
```

Decisão: manter a conclusão anterior. `select_min` ainda parece melhor no microbenchmark escalar, mas já falhou em sustentar ganho no k6 de produção. `unsorted` continua descartado.

### Experimento offline: budget aproximado de grupos

Hipótese: como o scoring permite alguns erros de detecção, talvez limitar a busca aos N grupos mais próximos por lower-bound reduzisse custo suficiente para compensar pequenos erros. A validação compara cada budget contra o KNN exato atual e conta erro de decisão (`approved` divergente), não apenas divergência de `fraud_score`.

Mudança instrumental:

- Adicionado modo `budget` em `cpp/tools/benchmark_classifier.cpp`.
- O modo mede o `group_local sort` exato e depois budgets fixos de `1, 2, 3, 4, 5, 6, 8, 10` grupos.
- Nenhuma mudança de produção foi feita.

Comando:

```text
nice -n 10 ionice -c3 cpp/build/benchmark-classifier-cpp resources/references.json.gz test/test-data.json 3 0 budget
```

Resultado:

```text
queries=14500 repeat=3 refs=100000 groups=117 exact_ns_per_query=77387.3 exact_rows_per_query=13836.9 exact_groups_per_query=2.8069 checksum=72090
budget_groups=1 fraud_count_mismatches=336 decision_errors=176 ns_per_query=35149.4 rows_per_query=5160.13 groups_per_query=1 checksum=72129
budget_groups=2 fraud_count_mismatches=281 decision_errors=136 ns_per_query=58726.3 rows_per_query=10288.4 groups_per_query=1.98745 checksum=72135
budget_groups=3 fraud_count_mismatches=189 decision_errors=80 ns_per_query=75914.7 rows_per_query=13694.2 groups_per_query=2.68034 checksum=72093
budget_groups=4 fraud_count_mismatches=118 decision_errors=46 ns_per_query=75864 rows_per_query=13756.8 groups_per_query=2.7269 checksum=72069
budget_groups=5 fraud_count_mismatches=52 decision_errors=19 ns_per_query=77443.9 rows_per_query=13788.6 groups_per_query=2.75586 checksum=72057
budget_groups=6 fraud_count_mismatches=29 decision_errors=15 ns_per_query=77552.6 rows_per_query=13803.5 groups_per_query=2.77979 checksum=72069
budget_groups=8 fraud_count_mismatches=3 decision_errors=0 ns_per_query=77683.3 rows_per_query=13833.5 groups_per_query=2.80359 checksum=72087
budget_groups=10 fraud_count_mismatches=0 decision_errors=0 ns_per_query=77434.5 rows_per_query=13836.9 groups_per_query=2.8069 checksum=72090
```

Decisão: rejeitar budget aproximado de grupos como otimização de produção neste momento.

Motivos:

- `budget=1` reduz o tempo escalar de ~77.4 us para ~35.1 us, mas cria `176` erros de decisão em `14500` requests. A perda de `detection_score` tende a superar o ganho de p99.
- `budget=2` ainda gera `136` erros de decisão e o ganho já cai para ~24%.
- `budget=3` a `6` mantêm erros e praticamente não reduzem custo.
- `budget=8` zera erro de decisão na massa local, mas custa ~77.7 us, sem ganho real contra o exato.
- `budget=10` converge para o exato.

Aprendizado: o agrupamento atual já está muito eficiente em média (`~2.81` grupos por query). Para ficar rápido o suficiente, a aproximação precisa cortar grupos demais e passa a errar decisões; quando corta pouco, não ganha performance. A próxima hipótese precisa reduzir custo dentro dos grupos escaneados ou usar uma estrutura de busca diferente, não apenas limitar quantidade fixa de grupos.

### Estado após a rodada

- Produção permanece no baseline `group_local` exato.
- Mantidas apenas ferramentas offline de benchmark:
  - `benchmark-classifier-cpp` com modos `traversal` e `budget`.
  - `benchmark-request-cpp` para separar body/parse/vectorize/classify.
- Próximo passo recomendado: benchmarkar variantes do kernel AVX2 de produção diretamente, especialmente custo de `std::sort`/alocação de `group_order`, `std::array<const float*, 14>` por grupo e possíveis buffers reutilizáveis, antes de qualquer novo A/B no k6.

## Rodada Investigativa: benchmark do kernel AVX2 de produção

Objetivo: criar um benchmark específico para o kernel AVX2 agrupado, porque o benchmark escalar anterior mostrou sinais que não se sustentaram no k6. O novo benchmark precisa medir o caminho mais próximo possível da produção: grupos já construídos em `ReferenceSet`, lower-bound por grupo, top-5 exato, `group.dimension_order`, chunks AVX2 de 8 linhas e fallback escalar apenas na cauda do grupo.

### Contrato do benchmark

Mudança instrumental:

- Adicionado `cpp/tools/benchmark_kernel.cpp`.
- Adicionado target CMake `benchmark-kernel-cpp` como `EXCLUDE_FROM_ALL`.
- O benchmark carrega `resources/references.json.gz` e os vetores esperados de `test/test-data.json`.
- A variante `baseline_production` replica o kernel agrupado AVX2 atual, incluindo `std::sort` do `group_order` e montagem local de `std::array<const float*, 14>` por grupo visitado.
- Cada variante é validada contra a baseline e imprime:
  - `fraud_count_mismatches`.
  - `decision_errors`.
  - `ns_per_query`.
  - `rows_per_query`.
  - `groups_per_query`.
  - `checksum`.

Comando de contrato antes da implementação:

```text
cmake --build cpp/build --target benchmark-kernel-cpp -j2
```

Resultado esperado antes da mudança:

```text
ninja: error: unknown target 'benchmark-kernel-cpp'
```

Esse foi o "red" da rodada: o alvo ainda não existia.

### Build do benchmark

Comando:

```text
nice -n 10 ionice -c3 cmake --build cpp/build --target benchmark-kernel-cpp -j2
```

Resultado: build OK. Os warnings emitidos são os warnings já conhecidos do single-header vendorizado do `simdjson`; não houve erro do código novo.

### Triagem curta

Comando:

```text
nice -n 10 ionice -c3 cpp/build/benchmark-kernel-cpp resources/references.json.gz test/test-data.json 5 1000
```

Resultado:

```text
queries=1000 repeat=5 refs=100000 groups=117 expected_rows_per_query=13469.8 expected_groups_per_query=2.812
variant=baseline_production fraud_count_mismatches=0 decision_errors=0 ns_per_query=28194.2 rows_per_query=13469.8 groups_per_query=2.812 validation_rows_per_query=13469.8 checksum=8690
variant=cached_group_ptrs fraud_count_mismatches=0 decision_errors=0 ns_per_query=28312 rows_per_query=13469.8 groups_per_query=2.812 validation_rows_per_query=13469.8 checksum=8690
variant=cached_group_ptrs_finite_once fraud_count_mismatches=0 decision_errors=0 ns_per_query=28287.4 rows_per_query=13469.8 groups_per_query=2.812 validation_rows_per_query=13469.8 checksum=8690
variant=cached_select_min_finite_once fraud_count_mismatches=0 decision_errors=0 ns_per_query=25957.9 rows_per_query=13469.8 groups_per_query=2.812 validation_rows_per_query=13469.8 checksum=8690
```

Aprendizado inicial: todas as variantes exatas preservaram decisão (`0` divergências). A combinação `cached_select_min_finite_once` pareceu ~8% mais rápida na amostra, mas misturava três variáveis: cache de ponteiros de grupos, `std::isfinite` fora do loop interno e troca de `std::sort` por seleção incremental.

### Separação da variável `select_min`

Para evitar conclusão contaminada, foi adicionada a variante `baseline_select_min`, que mantém o mesmo scan AVX2 da produção e troca apenas a ordenação completa de grupos por seleção incremental do menor lower-bound ainda não visitado.

Comando:

```text
nice -n 10 ionice -c3 cpp/build/benchmark-kernel-cpp resources/references.json.gz test/test-data.json 5 0
```

Resultado:

```text
queries=14500 repeat=5 refs=100000 groups=117 expected_rows_per_query=13836.9 expected_groups_per_query=2.8069
variant=baseline_production fraud_count_mismatches=0 decision_errors=0 ns_per_query=28725.8 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=120150
variant=baseline_select_min fraud_count_mismatches=0 decision_errors=0 ns_per_query=25914 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=120150
variant=cached_group_ptrs fraud_count_mismatches=0 decision_errors=0 ns_per_query=28483.1 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=120150
variant=cached_group_ptrs_finite_once fraud_count_mismatches=0 decision_errors=0 ns_per_query=28484.9 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=120150
variant=cached_select_min_finite_once fraud_count_mismatches=0 decision_errors=0 ns_per_query=26714.3 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=120150
```

Rodada mais forte:

```text
nice -n 10 ionice -c3 cpp/build/benchmark-kernel-cpp resources/references.json.gz test/test-data.json 10 0
```

Resultado:

```text
queries=14500 repeat=10 refs=100000 groups=117 expected_rows_per_query=13836.9 expected_groups_per_query=2.8069
variant=baseline_production fraud_count_mismatches=0 decision_errors=0 ns_per_query=28851.4 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=240300
variant=baseline_select_min fraud_count_mismatches=0 decision_errors=0 ns_per_query=26060.7 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=240300
variant=cached_group_ptrs fraud_count_mismatches=0 decision_errors=0 ns_per_query=29034.8 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=240300
variant=cached_group_ptrs_finite_once fraud_count_mismatches=0 decision_errors=0 ns_per_query=28924.5 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=240300
variant=cached_select_min_finite_once fraud_count_mismatches=0 decision_errors=0 ns_per_query=26870.6 rows_per_query=13836.9 groups_per_query=2.8069 validation_rows_per_query=13836.9 checksum=240300
```

### Decisão

O benchmark foi criado com sucesso e já aponta a próxima hipótese de produção: testar `baseline_select_min` no kernel AVX2 real.

Leitura técnica:

- `baseline_select_min` foi a melhor variante nas duas rodadas completas.
- No repeat `10`, caiu de `28851.4 ns/query` para `26060.7 ns/query`, ganho offline de ~9.7%.
- Não houve divergência: `fraud_count_mismatches=0`, `decision_errors=0`, checksums idênticos.
- Cachear ponteiros de grupos (`cached_group_ptrs`) não ajuda; ficou levemente pior.
- Mover `std::isfinite` para uma vez por chunk também não ajuda de forma isolada; ficou neutro/pior.
- A combinação `cached_select_min_finite_once` é melhor que baseline, mas pior que `baseline_select_min`, então a melhoria vem da troca de traversal, não do cache de ponteiros.

### Próximo passo recomendado

Aplicar somente a troca `std::sort(group_order)` -> `select_min` no `Classifier::top5_avx2`, sem cache de ponteiros e sem outras alterações, e rodar A/B curto no k6:

1. Rebuild compose com produção baseline atual e rodar 2x k6 aquecido para referência imediata.
2. Aplicar `baseline_select_min` no hot path.
3. Rebuild compose com `--pull never`.
4. Rodar pelo menos 3x k6.
5. Aceitar a mudança apenas se o p99 médio aquecido superar o baseline do mesmo estado de máquina sem aumentar `FP`, `FN` ou `HTTP errors`.

Observação importante: uma tentativa anterior de `select_min` não sustentou ganho no k6. A diferença agora é que o novo benchmark mede o kernel AVX2 de produção e isola a variável; ainda assim, a decisão final precisa ser k6, não microbenchmark.

## Rodada até 18h: validação k6 das hipóteses finais

Objetivo: continuar a busca por ganho concreto e sustentável até o limite combinado, mantendo disciplina de uma variável por vez. Critério de decisão: aceitar apenas mudanças que sustentassem ganho no k6 com `0 FP`, `0 FN` e `0 HTTP errors`; ganhos apenas em microbenchmark foram tratados como insuficientes.

### Baseline imediato

Estado de comparação antes das novas mudanças de produção: `group_local` exato, `std::sort` no kernel agrupado, split `api=0.44` por instância e `nginx=0.12`.

Runs de referência imediata:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.36ms | 5473.03 | 0 | 0 | 0 |
| 2 | 2.86ms | 5543.05 | 0 | 0 | 0 |

Após uma reconstrução limpa posterior, o baseline com LTO parcial ficou:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 2.98ms | 5525.68 | 0 | 0 | 0 |
| 2 | 2.99ms | 5524.51 | 0 | 0 | 0 |
| 3 | 2.88ms | 5541.06 | 0 | 0 | 0 |

Leitura: o estado aquecido recente oscilou entre `2.86ms` e `2.99ms`; por isso a decisão usou médias aquecidas e evitou promover mudança por um único melhor run.

### Experimento: `select_min` no kernel AVX2 real

Hipótese: substituir `std::sort(group_order)` por seleção incremental do menor lower-bound economizaria trabalho porque a consulta visita em média apenas ~2.8 grupos.

Evidência k6 com `select_min` em produção:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.35ms | 5361.98 | 0 | 0 | 0 |
| 2 | 2.93ms | 5533.67 | 0 | 0 | 0 |
| 3 | 2.79ms | 5554.08 | 0 | 0 | 0 |
| 4 | 2.94ms | 5531.81 | 0 | 0 | 0 |
| 5 | 2.96ms | 5528.13 | 0 | 0 | 0 |

Decisão: rejeitado. A média aquecida (`2.905ms`) não superou o baseline aquecido imediato (`2.86ms`) nem trouxe margem clara sobre o estado publicado. O microbenchmark continua útil, mas não é suficiente para produção.

### Experimento: LTO/IPO

Hipótese: `INTERPROCEDURAL_OPTIMIZATION` no binário principal reduziria overhead sem alterar algoritmo, contrato ou topologia.

Resultado com IPO apenas em `rinha-backend-2026-cpp`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.96ms | 5401.91 | 0 | 0 | 0 |
| 2 | 2.96ms | 5529.24 | 0 | 0 | 0 |
| 3 | 2.74ms | 5562.48 | 0 | 0 | 0 |
| 4 | 2.87ms | 5542.38 | 0 | 0 | 0 |
| 5 | 2.85ms | 5544.83 | 0 | 0 | 0 |
| 6 | 2.81ms | 5551.00 | 0 | 0 | 0 |

Média aquecida: `2.846ms`. Decisão: aceito como candidato sustentável, porque é uma mudança de build sem impacto funcional e reproduziu pequena vantagem sobre o baseline aquecido imediato.

Resultado com IPO também em `usockets` e `simdjson_singleheader`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.43ms | 5353.48 | 0 | 0 | 0 |
| 2 | 2.94ms | 5531.49 | 0 | 0 | 0 |
| 3 | 2.93ms | 5533.79 | 0 | 0 | 0 |

Decisão: rejeitado. IPO completo piorou a média aquecida (`2.935ms`) e foi revertido para IPO apenas no executável da API.

### Experimento: `res->cork` no envio do `POST /fraud-score`

Hipótese: como o response é montado dentro de `onData`, agrupar `writeHeader` e `end` manualmente com `res->cork` reduziria writes/syscalls no uWebSockets.

Resultado:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.38ms | 5358.60 | 0 | 0 | 0 |
| 2 | 2.90ms | 5538.32 | 0 | 0 | 0 |
| 3 | 2.76ms | 5558.64 | 0 | 0 | 0 |
| 4 | 2.97ms | 5526.86 | 0 | 0 | 0 |
| 5 | 2.84ms | 5546.14 | 0 | 0 | 0 |

Média aquecida: `2.8675ms`. Decisão: aceito. O ganho é pequeno, mas a mudança é local, correta para o modelo do uWebSockets e não altera o contrato da API.

### Experimentos de response rejeitados

`res->cork` sem `Content-Type`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.13ms | 5384.56 | 0 | 0 | 0 |
| 2 | 3.05ms | 5516.25 | 0 | 0 | 0 |
| 3 | 2.96ms | 5528.04 | 0 | 0 | 0 |

Decisão: rejeitado. O contrato local passou, mas a média aquecida foi pior e a remoção do header é menos conservadora para submissão.

`UWS_HTTPRESPONSE_NO_WRITEMARK` para remover o header automático `uWebSockets: 20`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.13ms | 5384.35 | 0 | 0 | 0 |
| 2 | 2.95ms | 5530.80 | 0 | 0 | 0 |
| 3 | 3.10ms | 5508.75 | 0 | 0 | 0 |

Decisão: rejeitado. Reduz bytes, mas não reduziu p99 no k6.

### Experimentos de hot path rejeitados

Remover cópias de `shared_ptr<AppState>` no caminho de request:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.43ms | 5353.20 | 0 | 0 | 0 |
| 2 | 3.04ms | 5516.88 | 0 | 0 | 0 |
| 3 | 3.00ms | 5522.81 | 0 | 0 | 0 |

Decisão: rejeitado. A simplificação de lifetime é válida, mas não trouxe ganho e piorou a amostra aquecida.

Parse direto do chunk quando o body chega inteiro:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.26ms | 5370.91 | 0 | 0 | 0 |
| 2 | 2.96ms | 5528.69 | 0 | 0 | 0 |
| 3 | 2.81ms | 5551.07 | 0 | 0 | 0 |
| 4 | 2.98ms | 5525.89 | 0 | 0 | 0 |
| 5 | 2.90ms | 5536.95 | 0 | 0 | 0 |

Decisão: rejeitado. A média aquecida (`2.9125ms`) ficou pior que `res->cork` isolado.

Parser com `known_merchants` em `string_view` inline:

Microbenchmark:

```text
parse_payload_ns_per_query=504.708
parse_vectorize_ns_per_query=577.082
parse_classify_ns_per_query=27269.3
```

Comparação anterior aproximada no mesmo benchmark:

```text
parse_payload_ns_per_query=592.512
parse_vectorize_ns_per_query=653.227
parse_classify_ns_per_query=28246.9
```

k6:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.74ms | 5426.91 | 0 | 0 | 0 |
| 2 | 2.88ms | 5540.32 | 0 | 0 | 0 |
| 3 | 2.94ms | 5531.55 | 0 | 0 | 0 |

Decisão: rejeitado. O microbenchmark melhorou parse, mas o k6 não sustentou ganho final. Como a mudança aumentava complexidade do parser, foi revertida.

### Experimentos de recursos no compose

Todos os testes abaixo foram feitos sobre o binário com `res->cork`.

`api=0.45` por instância e `nginx=0.10`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 2.87ms | 5542.35 | 0 | 0 | 0 |
| 2 | 2.99ms | 5523.65 | 0 | 0 | 0 |
| 3 | 2.95ms | 5529.52 | 0 | 0 | 0 |

Decisão: rejeitado. Média `2.936ms`, pior que o split atual.

`api=0.43` por instância e `nginx=0.14`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 2.84ms | 5546.69 | 0 | 0 | 0 |
| 2 | 2.90ms | 5537.72 | 0 | 0 | 0 |
| 3 | 2.95ms | 5530.61 | 0 | 0 | 0 |

Decisão: rejeitado. Média `2.896ms`, competitiva mas sem superar claramente `0.44/0.12`.

`api=0.435` por instância e `nginx=0.13`:

| Run | p99 | final_score | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 1 | 2.97ms | 5527.48 | 0 | 0 | 0 |
| 2 | 2.92ms | 5535.09 | 0 | 0 | 0 |
| 3 | 2.98ms | 5526.51 | 0 | 0 | 0 |

Decisão: rejeitado. Média `2.956ms`; o split original continua melhor.

### Estado final aceito da rodada

Mudanças mantidas:

- `INTERPROCEDURAL_OPTIMIZATION` apenas no executável `rinha-backend-2026-cpp`.
- `res->cork` ao responder `POST /fraud-score`.
- `benchmark-kernel-cpp` como ferramenta offline para futuras hipóteses de kernel AVX2.
- `docker-compose.yml` restaurado para `api=0.44` por instância e `nginx=0.12`.

Mudanças explicitamente rejeitadas e revertidas:

- `select_min` no kernel de produção.
- IPO completo em libs auxiliares.
- Remoção de `Content-Type`.
- `UWS_HTTPRESPONSE_NO_WRITEMARK`.
- Remoção de `shared_ptr<AppState>`.
- Parse direto de chunk único.
- Parser com `known_merchants` por `string_view` inline.
- Splits `0.45/0.10`, `0.43/0.14` e `0.435/0.13`.

Leitura técnica: a maior parte do espaço restante já está no ruído do k6 local. A única mudança de produção mantida além do LTO foi `res->cork`, por ser pequena, tecnicamente correta e levemente positiva na média aquecida. O parser pode ser melhorado em microbenchmark, mas a classificação e o caminho containerizado ainda dominam o p99.

### Validação final antes do fechamento

Horário de fechamento operacional: `2026-04-27 17:34 -0300`, dentro da janela combinada até 18h.

Comandos executados:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-request-cpp benchmark-kernel-cpp -j2
git diff --check
docker compose config -q
ctest --test-dir cpp/build --output-on-failure
cpp/build/benchmark-kernel-cpp resources/references.json.gz test/test-data.json 3 1000
docker compose up -d --build --force-recreate --pull never
curl -sS -o /dev/null -w '%{http_code}\n' http://localhost:9999/ready
./run.sh
```

Resultados:

- Build local: OK (`ninja: no work to do`, alvo já atualizado).
- `git diff --check`: OK, sem whitespace error.
- `docker compose config -q`: OK.
- Testes C++: `1/1` passou.
- `/ready`: `204`.
- Benchmark de kernel curto: `0` divergências em todas as variantes; `baseline_production=27981 ns/query`, `baseline_select_min=25664 ns/query`.
- k6 final no estado aceito: `p99=3.00ms`, `final_score=5522.93`, `0 FP`, `0 FN`, `0 HTTP errors`.

Resultado final da rodada: manter somente mudanças sustentáveis e pequenas (`IPO` parcial, `res->cork`, benchmark offline). As demais hipóteses foram registradas e revertidas por não baterem o baseline aquecido no k6.
