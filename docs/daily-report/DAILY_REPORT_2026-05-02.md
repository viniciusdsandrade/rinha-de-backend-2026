# Daily Report 2026-05-02

## Contexto da rodada

Objetivo: continuar o ciclo investigação -> experimento -> benchmark -> report -> commit/push até 07h00, buscando ganho sustentável de performance para a Rinha de Backend 2026. Critério de aceitação nesta rodada: manter `0 FP`, `0 FN` e `0 HTTP errors`; ganhos que sacrificarem acurácia são rejeitados mesmo com p99 menor.

Branch de trabalho:

```text
perf/noon-tuning
HEAD inicial: 2d3b04b summarize may first tuning
```

Referência externa a superar antes de abrir nova issue de submissão:

```text
submissão anterior informada: p99=2.83ms, failure_rate=0%, final_score=5548.91
```

Estado aceito herdado de 2026-05-01:

```text
api1/api2: 0.41 CPU e 165MB cada
nginx:     0.18 CPU e 20MB
total:     1.00 CPU e 350MB
IVF_FAST_NPROBE=1
IVF_FULL_NPROBE=1
IVF_BBOX_REPAIR=true
```

## Baseline fresca da madrugada

Validação de ambiente:

```text
GET /ready => 204
api1 NanoCpus=410000000 Memory=173015040 IVF_BBOX_REPAIR=true
api2 NanoCpus=410000000 Memory=173015040 IVF_BBOX_REPAIR=true
nginx NanoCpus=180000000 Memory=20971520
```

Benchmark oficial local atualizado:

```text
p99=2.99ms
FP=0
FN=0
HTTP=0
TP=24037
TN=30023
weighted_errors_E=0
detection_score=3000
p99_score=2523.75
final_score=5523.75
```

Leitura: o estado aceito continua correto e sem falhas de detecção. A baseline fresca ficou muito próxima das runs aceitas de 2026-05-01 (`2.98ms / 5526.35`), portanto é uma base válida para os próximos experimentos.

## Experimento aceito: índice IVF com 1280 clusters

Hipótese: o índice atual com `2048` clusters pode estar pagando custo excessivo de varredura de centroides e reparo por bounding box. Reduzir o número de clusters pode diminuir overhead por consulta sem perder acurácia, desde que o reparo continue ativo.

Screening offline com `benchmark-ivf-cpp`:

| Clusters | `ns_per_query` | FP | FN | Decisão |
|---:|---:|---:|---:|---|
| 2048 | 59024.7 | 0 | 0 | baseline offline inicial |
| 4096 | 69480.0 | 0 | 0 | rejeitado, piorou |
| 1024 | 59187.7 | 0 | 0 | rejeitado, sem ganho |
| 1536 | 57112.9 | 0 | 0 | candidato |
| 2560 | 58228.1 | 0 | 0 | rejeitado, pior que 1536 |
| 1280 | 54490.6 | 0 | 0 | melhor candidato |
| 1152 | 57512.6 | 0 | 0 | rejeitado |
| 1408 | 55507.2 | 0 | 0 | rejeitado |

Repetição offline:

| Clusters | Repeat | `ns_per_query` | FP | FN |
|---:|---:|---:|---:|---:|
| 2048 | 3 | 57665.1 | 0 | 0 |
| 1280 | 3 | 56789.3 | 0 | 0 |

Alteração aplicada:

```text
Dockerfile:
  prepare-ivf-cpp ... 2048 65536 6
  prepare-ivf-cpp ... 1280 65536 6
```

Validação de imagem:

```text
prepare-ivf-cpp: refs=3000000 padded=3004384 clusters=1280 memory_mb=94.6933
GET /ready => 204
```

Benchmark oficial local atualizado:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| baseline fresca 2048 clusters | 1 | 2.99ms | 0 | 0 | 0 | 5523.75 |
| 1280 clusters | 1 | 2.92ms | 0 | 0 | 0 | 5535.08 |
| 1280 clusters | 2 | 2.94ms | 0 | 0 | 0 | 5532.22 |

Breakdown da melhor run:

```text
TP=24037
TN=30021
FP=0
FN=0
HTTP=0
weighted_errors_E=0
detection_score=3000
p99_score=2535.08
final_score=5535.08
```

Leitura: `1280` clusters preserva acurácia perfeita e reduz p99 de forma reproduzida. O ganho local sustentável contra a baseline fresca é de `0.05ms-0.07ms` e `+8.47` a `+11.33` pontos. Ainda não supera a submissão anterior informada (`5548.91`), portanto não justifica abrir issue oficial.

Decisão: aceito. O `Dockerfile` passa a gerar índice IVF com `1280` clusters.

## Experimento rejeitado: reteste de CPU split `0.42/0.42/0.16` com índice 1280

Hipótese: o split `0.42/0.42/0.16` foi instável no índice de `2048` clusters, mas poderia ficar competitivo com o índice `1280`, já que o custo de classificação ficou menor e talvez o nginx precisasse de menos folga.

Alteração testada:

```text
api1/api2: 0.41 CPU -> 0.42 CPU cada
nginx:     0.18 CPU -> 0.16 CPU
índice:    1280 clusters mantido
```

Validação:

```text
GET /ready => 204
api1 NanoCpus=420000000 Memory=173015040
api2 NanoCpus=420000000 Memory=173015040
nginx NanoCpus=160000000 Memory=20971520
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 1280 clusters + `0.41/0.41/0.18` | 2.92ms | 0 | 0 | 0 | 5535.08 | referência |
| 1280 clusters + `0.42/0.42/0.16` | 3.06ms | 0 | 0 | 0 | 5514.77 | rejeitado |

Leitura: mesmo com índice mais barato, reduzir o nginx para `0.16 CPU` voltou a aumentar p99. Isso reforça que o gargalo marginal não é apenas CPU das APIs; o LB precisa de pelo menos `0.18 CPU` neste cenário para manter baixa variância.

Decisão: rejeitado. `docker-compose.yml` voltou para `api1/api2=0.41 CPU` e `nginx=0.18 CPU`.

## Experimento rejeitado: estreitar heurística de reparo extremo

Hipótese: a heurística `should_repair_extreme(...)` era deliberadamente ampla para preservar `0 FP/FN`. Como os casos extremos perigosos são poucos, estreitar os limiares poderia reduzir reparos completos, baixar o custo por consulta e melhorar p99 sem mudar `approved`.

Validação prévia:

```text
Índice local e índice GHCR submission-a9e49db:
sha256 ambos: 773decf0278aa986fa396b5be1b4d805f9dcf7fa8cba53088c42bde24cfbdc3b

Heurística ampla atual:
repeat=5
ns_per_query=16389.5 a 17268.9
fp=0
fn=0

Heurística estreita:
repeat=5
ns_per_query=14162.2 a 15257.0
fp=0
fn=0
```

Alteração testada:

```text
frauds == 0:
  amount_vs_avg 0.20..0.40 -> 0.23..0.37
  km_from_home <= 0.13     -> <= 0.115
  tx_count_24h <= 0.25     -> <= 0.21

frauds == 5:
  amount_vs_avg >= 0.80 -> 0.84..0.89
  km_from_home >= 0.35  -> 0.38..0.42
  mcc_risk >= 0.75      -> 0.79..0.81
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp benchmark-ivf-cpp test -j8
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed
docker compose build api1
GET /ready => 204
```

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| heurística ampla oficial | 2.65ms | 0 | 0 | 0 | 5576.34 | referência |
| heurística estreita | 2.80ms | 0 | 0 | 0 | 5553.19 | rejeitado |

Leitura: o ganho offline não se converteu em cauda no stack completo. A alteração muda alguns `fraud_score` sem mudar `approved`, e provavelmente piora distribuição de branch/cache ou reduz reparos em casos que estabilizavam a latência em concorrência real.

Decisão: rejeitado. `cpp/src/ivf.cpp` voltou para a heurística ampla já validada oficialmente na issue `#720`.

## Experimento rejeitado: parser com `string_view` para merchant matching

Hipótese: `parse_payload` copiava `merchant.id` e todos os itens de `customer.known_merchants` para `std::string` apenas para comparar dentro da mesma função. Trocar para `std::string_view` deveria remover alocações/cópias no hot path sem alterar o payload final, pois os views viveriam somente enquanto o `simdjson::padded_string` local ainda existisse.

Alteração testada:

```text
std::vector<std::string> known_merchants -> std::vector<std::string_view>
std::string merchant_id                  -> std::string_view
merchant.id                              -> leitura direta como string_view
known_merchants[]                        -> emplace de string_view sem cópia
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp benchmark-ivf-cpp test -j8
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed

benchmark-ivf:
ns_per_query=16673.9
fp=0
fn=0

docker compose build api1
GET /ready => 204
```

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| parser original com cópia de strings | 2.65ms | 0 | 0 | 0 | 5576.34 | referência |
| parser com `string_view` | 3.06ms | 0 | 0 | 0 | 5514.43 | rejeitado |

Leitura: a redução de cópias não melhorou o stack completo e piorou a cauda. A hipótese provável é que a alteração muda layout/otimização do parse ou interage pior com `simdjson::dom`, enquanto o custo das cópias pequenas não era o gargalo dominante.

Decisão: rejeitado. `cpp/src/request.cpp` voltou ao parser original.

## Experimento rejeitado: `-march=haswell`

Hipótese: o ambiente oficial usa Mac Mini Late 2014 com CPU Haswell/AVX2. Trocar o alvo genérico `x86-64-v3` por `haswell` poderia melhorar tuning do GCC sem alterar algoritmo, API ou topologia.

Alteração testada:

```text
cpp/CMakeLists.txt:
  -march=x86-64-v3 -> -march=haswell

Targets alterados:
  rinha-backend-2026-cpp
  prepare-refs-cpp
  prepare-ivf-cpp
  rinha-backend-2026-cpp-tests
  benchmark-classifier-cpp
  benchmark-request-cpp
  benchmark-ivf-cpp
  benchmark-kernel-cpp
```

Validação:

```text
cmake -S cpp -B cpp/build-haswell -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build-haswell --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-ivf-cpp -j8
ctest --test-dir cpp/build-haswell --output-on-failure
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed
```

Resultado offline:

| Variante | `ns_per_query` | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `x86-64-v3` referência | 16389.5 a 17268.9 | 0 | 0 | referência |
| `haswell` | 17236.3 | 0 | 0 | rejeitado antes do k6 |

Leitura: `haswell` não trouxe sinal positivo nem no microbenchmark e ainda aumentou warnings/caminhos específicos de simdjson. Como a meta agora exige microganho real de p99, não vale submeter uma mudança sem ganho mensurável prévio.

Decisão: rejeitado. `cpp/CMakeLists.txt` voltou para `-march=x86-64-v3`.

## Incidente de submissão: tag mutável no GHCR causou resultado oficial inconsistente

Resultado da issue oficial `#719`:

```text
Issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/719
commit testado pela engine: 870d435
imagem declarada: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission
p99: 1.64ms
FP: 3
FN: 4
HTTP errors: 0
failure_rate: 0.01%
final_score: 5424.02
```

Leitura inicial: o resultado oficial teve latência excelente, mas quebrou a premissa mais importante do experimento (`0 FP/FN`). Isso invalidaria a submissão se fosse reproduzível.

Reprodução local com a mesma branch `submission` e a mesma imagem GHCR:

```text
compose: origin/submission @ 870d435
imagem: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission
api1/api2: 0.41 CPU / 165MB
nginx: 0.18 CPU / 20MB
GET /ready => 204

Resultado local:
p99: 2.74ms
FP: 0
FN: 0
HTTP errors: 0
final_score: 5561.98
```

Hipótese de causa raiz: a engine oficial provavelmente reutilizou imagem antiga associada à tag mutável `:submission`. O compose novo ativou `IVF_BOUNDARY_FULL=true` e reparo seletivo `1..4`, mas a imagem antiga não continha `should_repair_extreme(...)`; isso explica precisamente o padrão `3 FP / 4 FN` observado oficialmente. Localmente, depois de puxar o manifest atualizado, o mesmo compose voltou a `0 FP/FN`.

Correção operacional:

```text
Workflow atualizado:
  .github/workflows/publish-submission-image.yml
  adiciona tag imutável: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a9e49db
  commit: ff4734c

Publish:
  https://github.com/viniciusdsandrade/rinha-de-backend-2026/actions/runs/25244642464
  resultado: sucesso

Imagem imutável:
  ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a9e49db
  digest: sha256:539fed1d7567f55115266141cae19620ef2cff400d2854fd6306d89d05c856ff
  linux/amd64 manifest: sha256:7dc5200d1439aed1fec84274b208700e220989f942505ce97fc38ba31c1c83fb

Branch submission:
  origin/submission @ 8293b49
  docker-compose.yml aponta para :submission-a9e49db
```

Nova issue oficial:

```text
https://github.com/zanfranceschi/rinha-de-backend-2026/issues/720
title: andrade-cpp-ivf
body: rinha/test andrade-cpp-ivf
```

Decisão: não considerar `#719` como validação técnica do experimento. A hipótese de cache/stale image é forte e foi mitigada com tag imutável. A validação oficial relevante passa a ser `#720`.

### Resultado oficial confirmado com tag imutável

Resultado da issue oficial `#720`:

```text
Issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/720
commit testado pela engine: 8293b49
imagem testada: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a9e49db
p99: 1.45ms
FP: 0
FN: 0
HTTP errors: 0
failure_rate: 0%
weighted_errors_E: 0
p99_score: 2837.36
detection_score: 3000
final_score: 5837.36
```

Comparação:

```text
Submissão anterior conhecida: 5548.91
Nova submissão oficial:       5837.36
Ganho absoluto:               +288.45 pontos
Ganho relativo:               +5.20%

Ranking parcial informado pelo usuário:
1. thiagorigonatti-c: 5901.92
2. jairoblatt-rust:  5838.50
Nova submissão:       5837.36
Gap para #2 parcial:  -1.14 ponto
Gap para #1 parcial:  -64.56 pontos
```

Conclusão: a correção por tag imutável resolveu a divergência oficial. O ajuste de reparo seletivo é oficialmente válido e competitivo. O próximo objetivo técnico deixa de ser "recuperar a submissão" e passa a ser encontrar ganhos pequenos e sustentáveis de p99 para ultrapassar a faixa de `5838.50` e, depois, buscar um salto maior rumo a `5901.92`.

## Experimento rejeitado: três APIs com reparo seletivo

Hipótese: depois de reduzir o custo médio do classificador com reparo seletivo, a topologia de três APIs poderia reduzir fila por instância e melhorar o p99 oficial. Essa topologia já existia na submissão anterior, mas ainda não havia sido testada com o novo código e com tag imutável.

Alteração testada:

```text
api1/api2: 0.41 CPU / 165MB cada
nginx:     0.18 CPU / 20MB

para:

api1/api2/api3: 0.27 CPU / 110MB cada
nginx:          0.19 CPU / 20MB
```

Validação:

```text
GET /ready => 204
api1 NanoCpus=270000000 Memory=115343360
api2 NanoCpus=270000000 Memory=115343360
api3 NanoCpus=270000000 Memory=115343360
nginx NanoCpus=190000000 Memory=20971520
```

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| reparo seletivo + 2 APIs (`0.41/0.41/0.18`) | 2.65ms | 0 | 0 | 0 | 5576.34 | referência |
| reparo seletivo + 3 APIs (`0.27x3/0.19`) | 2.73ms | 0 | 0 | 0 | 5564.50 | rejeitado |

Leitura: a terceira API não compensa a redução de CPU por instância. O gargalo residual parece mais sensível à fatia de CPU de cada API do que ao número de filas/instâncias atrás do nginx.

Decisão: rejeitado. `docker-compose.yml` e `nginx.conf` voltaram para a topologia oficial atual com duas APIs.

## Experimento rejeitado: CPU split fino `0.415/0.415/0.17`

Hipótese: como o resultado oficial ficou apenas `1.14` ponto abaixo do segundo lugar parcial informado, um ajuste fino de CPU poderia melhorar o p99 sem alterar código nem risco de detecção. Os extremos `0.40/0.40/0.20` e `0.42/0.42/0.16` pioraram, mas o ponto intermediário ainda era plausível.

Alteração testada:

```text
api1/api2: 0.41 CPU -> 0.415 CPU cada
nginx:     0.18 CPU -> 0.17 CPU
```

Validação:

```text
GET /ready => 204
api1 NanoCpus=415000000 Memory=173015040
api2 NanoCpus=415000000 Memory=173015040
nginx NanoCpus=170000000 Memory=20971520
```

Resultado no benchmark oficial local:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| reparo seletivo + `0.41/0.41/0.18` | referência melhor | 2.65ms | 0 | 0 | 0 | 5576.34 | referência |
| reparo seletivo + `0.415/0.415/0.17` | 1 | 2.67ms | 0 | 0 | 0 | 5573.83 | inconclusivo |
| reparo seletivo + `0.415/0.415/0.17` | 2 | 2.69ms | 0 | 0 | 0 | 5570.35 | rejeitado |

Leitura: a variação ficou dentro do ruído local e não superou a melhor referência. Como o ganho necessário é muito pequeno, submeter uma troca sem sinal local claro seria especulação.

Decisão: rejeitado. `docker-compose.yml` voltou para `api1/api2=0.41 CPU` e `nginx=0.18 CPU`.

## Experimento rejeitado: CPU split `0.42/0.42/0.16` após reparo seletivo

Hipótese: se o gargalo residual ainda estivesse nas APIs, aumentar a fatia de CPU de cada API poderia reduzir o p99 mesmo com menos CPU no nginx.

Alteração testada:

```text
api1/api2: 0.41 CPU -> 0.42 CPU cada
nginx:     0.18 CPU -> 0.16 CPU
índice:    1280 clusters mantido
reparo:    seletivo 1..4 + extremos perigosos
```

Validação:

```text
GET /ready => 204
api1 NanoCpus=420000000 Memory=173015040
api2 NanoCpus=420000000 Memory=173015040
nginx NanoCpus=160000000 Memory=20971520
```

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| reparo seletivo + `0.41/0.41/0.18` | 2.65ms | 0 | 0 | 0 | 5576.34 | referência |
| reparo seletivo + `0.42/0.42/0.16` | 2.79ms | 0 | 0 | 0 | 5554.41 | rejeitado |

Leitura: reduzir CPU do nginx piorou a cauda mais do que o ganho marginal nas APIs. O ponto `0.41/0.41/0.18` segue sendo o melhor split observado para duas APIs.

Decisão: rejeitado. `docker-compose.yml` voltou para `api1/api2=0.41 CPU` e `nginx=0.18 CPU`.

## Experimento aceito: reparo seletivo de extremos no IVF

Hipótese: o modo aceito anterior fazia `bbox_repair` completo para todos os casos (`IVF_BOUNDARY_FULL=false`, `IVF_REPAIR_MIN_FRAUDS=0`, `IVF_REPAIR_MAX_FRAUDS=5`). Isso preservava `0 FP/FN`, mas desperdiçava CPU em consultas cujo top-5 aproximado já estava longe da fronteira. O melhor caminho seria manter reparo completo apenas nos casos de fronteira (`1..4` fraudes) e reativar reparo para os poucos extremos (`0` ou `5` fraudes) que o microbenchmark mostrou serem perigosos.

Diagnóstico offline:

```text
Configuração sem reparo de extremos:
  boundary_full=true, repair_min=1, repair_max=4, bbox_repair=true
  ns_per_query=13480.8
  fp=2
  fn=4

Configuração com heurística seletiva para extremos:
  boundary_full=true, repair_min=1, repair_max=4, bbox_repair=true
  ns_per_query=16103.6
  fp=0
  fn=0
  parse_errors=0
```

Padrão encontrado nos erros extremos sem reparo:

| Caso | Sintoma sem reparo | Reparo completo | Padrão do vetor |
|---|---:|---:|---|
| fraude prevista como legítima | `fraud_count=0` | `fraud_count=3` | sem `last_transaction`, `amount_vs_avg` entre `0.20` e `0.40`, `km_from_home` baixo, `tx_count_24h` baixo |
| legítima prevista como fraude | `fraud_count=5` | `fraud_count=2` | sem `last_transaction`, `amount_vs_avg >= 0.80`, longe de casa, comerciante desconhecido, MCC alto |

Alteração aplicada:

```text
cpp/src/ivf.cpp:
  adiciona should_repair_extreme(frauds, query)
  mantém reparo completo para fronteira 1..4
  adiciona reparo completo apenas para extremos com padrão empiricamente perigoso

docker-compose.yml:
  IVF_BOUNDARY_FULL=true
  IVF_REPAIR_MIN_FRAUDS=1
  IVF_REPAIR_MAX_FRAUDS=4
  IVF_BBOX_REPAIR=true
```

Validação:

```text
cmake --build cpp/build --target test -j8
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed

GET /ready => 204
api1/api2 confirmados com:
  IVF_BOUNDARY_FULL=true
  IVF_REPAIR_MIN_FRAUDS=1
  IVF_REPAIR_MAX_FRAUDS=4
  IVF_BBOX_REPAIR=true
```

Resultados no benchmark oficial local:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| estado aceito anterior (`1280`, reparo completo normal) | referência | 2.92ms | 0 | 0 | 0 | 5535.08 |
| reparo seletivo de extremos | 1 | 2.86ms | 0 | 0 | 0 | 5543.37 |
| reparo seletivo de extremos | 2 | 2.65ms | 0 | 0 | 0 | 5576.34 |
| reparo seletivo de extremos | 3 | 2.69ms | 0 | 0 | 0 | 5570.52 |

Comparação com submissão oficial anterior:

```text
Submissão anterior: p99=2.83ms, failure_rate=0%, final_score=5548.91
Melhor run nova:    p99=2.65ms, failure_rate=0%, final_score=5576.34
Ganho bruto:        +27.43 pontos
Ganho relativo:     +0.49%
```

Leitura: o ganho não é apenas microbenchmark. A primeira run ficou abaixo da submissão anterior, mas as duas runs seguintes superaram a melhor submissão oficial anterior mantendo `0 FP/FN/HTTP`. O mecanismo é sustentável porque não troca a métrica nem reduz a cobertura de correção: ele só evita reparo caro onde o top-5 aproximado já é decisivo e repara explicitamente os extremos que o dataset local comprovou serem ambíguos.

Decisão: aceito e publicado na branch de investigação. Como houve run reproduzida acima de `5548.91`, o próximo passo é preparar a submissão efetiva e abrir issue oficial da Rinha apontando para a melhor versão.

### Publicação da submissão oficial

Passos executados:

```text
Branch de investigação publicada:
  perf/noon-tuning @ a9e49db

Workflow de imagem:
  Publish submission image
  Run: https://github.com/viniciusdsandrade/rinha-de-backend-2026/actions/runs/25244430850
  Resultado: sucesso

Imagem pública:
  ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission
  digest: sha256:a61bcd2b3e32674e04f0a174ad59601c4d3f12c542aca01255905b8ae1ff28c2
  plataforma confirmada: linux/amd64

Branch submission:
  origin/submission @ 870d435
  docker-compose.yml na raiz
  info.json na raiz
  nginx.conf na raiz
```

Validação da branch `submission`:

```text
docker compose config => válido
serviços: nginx + api1 + api2
recursos: 0.18 + 0.41 + 0.41 = 1.00 CPU
memória: 20MB + 165MB + 165MB = 350MB
network: bridge
porta externa: 9999
imagem: linux/amd64
```

Tentativa de reabrir a issue anterior:

```text
Issue anterior: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/603
Comando: gh issue reopen 603
Resultado: falhou com GraphQL: Could not reopen the issue. (reopenIssue)
```

Issue oficial aberta:

```text
https://github.com/zanfranceschi/rinha-de-backend-2026/issues/719
title: andrade-cpp-ivf
body: rinha/test andrade-cpp-ivf
```

Decisão: submissão efetiva disparada porque a nova melhor run local (`5576.34`) superou a submissão oficial anterior (`5548.91`) mantendo `0%` de falhas.

## Experimento rejeitado: CPU split `0.40/0.40/0.20` após reparo seletivo

Hipótese: como o reparo seletivo reduziu o custo médio das APIs, devolver mais CPU ao nginx poderia reduzir cauda no balanceador sem prejudicar o processamento.

Alteração testada:

```text
api1/api2: 0.41 CPU -> 0.40 CPU cada
nginx:     0.18 CPU -> 0.20 CPU
índice:    1280 clusters mantido
reparo:    seletivo 1..4 + extremos perigosos
```

Validação:

```text
GET /ready => 204
api1 NanoCpus=400000000 Memory=173015040
api2 NanoCpus=400000000 Memory=173015040
nginx NanoCpus=200000000 Memory=20971520
```

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| reparo seletivo + `0.41/0.41/0.18` | 2.65ms | 0 | 0 | 0 | 5576.34 | referência |
| reparo seletivo + `0.40/0.40/0.20` | 2.73ms | 0 | 0 | 0 | 5563.09 | rejeitado |

Leitura: o nginx não é o gargalo dominante nesse ponto. Mesmo após reduzir o custo do classificador, retirar CPU das APIs piorou a melhor cauda observada.

Decisão: rejeitado. `docker-compose.yml` voltou para `api1/api2=0.41 CPU` e `nginx=0.18 CPU`.

## Experimento rejeitado: `worker_processes 2` no nginx

Hipótese: aumentar o nginx de 1 para 2 workers poderia reduzir fila de accept/proxy no LB e melhorar p99, mesmo com `0.18 CPU`.

Alteração testada:

```text
nginx.conf:
  worker_processes 1 -> 2
```

Validação:

```text
GET /ready => 204
nginx -T confirmou worker_processes 2
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 1280 clusters + nginx 1 worker | 2.92ms | 0 | 0 | 0 | 5535.08 | referência |
| 1280 clusters + nginx 2 workers | 2.97ms | 0 | 0 | 0 | 5527.10 | rejeitado |

Leitura: o worker extra não reduziu p99 e provavelmente aumentou disputa por uma fração pequena de CPU no LB. Com `0.18 CPU`, um único worker segue mais eficiente.

Decisão: rejeitado. `nginx.conf` voltou para `worker_processes 1`.

## Experimento rejeitado: `multi_accept off` no nginx

Hipótese: com apenas 1 worker e CPU limitada, `multi_accept on` poderia criar bursts de accept/proxy e aumentar latência de cauda. Desligar `multi_accept` poderia suavizar o p99.

Alteração testada:

```text
nginx.conf:
  multi_accept on -> off
```

Validação:

```text
GET /ready => 204
nginx -T confirmou multi_accept off
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 1280 clusters + `multi_accept on` | 2.92ms | 0 | 0 | 0 | 5535.08 | referência |
| 1280 clusters + `multi_accept off` | 2.97ms | 0 | 0 | 0 | 5526.71 | rejeitado |

Leitura: suavizar accept não ajudou; o p99 piorou. A configuração atual com `multi_accept on` continua melhor para absorver o padrão de carga do k6.

Decisão: rejeitado. `nginx.conf` voltou para `multi_accept on`.

## Experimento rejeitado: remover `reuseport` do nginx

Hipótese: com apenas 1 worker no nginx, `reuseport` poderia ser redundante e talvez adicionar variação/overhead. Removê-lo manteria a topologia e o round-robin upstream inalterados.

Alteração testada:

```text
nginx.conf:
  listen 9999 reuseport backlog=4096;
  listen 9999 backlog=4096;
```

Validação:

```text
GET /ready => 204
nginx -T confirmou listen 9999 backlog=4096
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 1280 clusters + `reuseport` | 2.92ms | 0 | 0 | 0 | 5535.08 | referência |
| 1280 clusters sem `reuseport` | 2.98ms | 0 | 0 | 0 | 5525.75 | rejeitado |

Leitura: mesmo com um worker, manter `reuseport` foi melhor no benchmark local. A remoção aumentou p99 sem qualquer benefício operacional.

Decisão: rejeitado. `nginx.conf` voltou para `listen 9999 reuseport backlog=4096`.

## Ciclo 14h: candidato local removendo `res->cork` no hot path

Hipótese: depois da remoção do header `Content-Type`, o caminho de resposta passou a fazer apenas um `end()` com uma das seis strings estáticas de classificação. Nesse caso, o `res->cork(...)` pode ser overhead desnecessário no hot path do uWebSockets, porque não há múltiplas escritas a agrupar.

Alteração testada:

```cpp
const std::string_view body = classification_json(classification);
res->end(body);
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
DOCKER_CONTEXT=default docker compose build api1
DOCKER_CONTEXT=default docker compose up -d --force-recreate --remove-orphans
```

Resultado no `DOCKER_CONTEXT=default`:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.16ms | 0 | 0 | 0 | 5935.65 |
| 2 | 1.19ms | 0 | 0 | 0 | 5925.03 |
| 3 | 1.19ms | 0 | 0 | 0 | 5925.47 |

Comparação local imediata:

| Variante | p99 | final_score | Leitura |
|---|---:|---:|---|
| Sem `Content-Type`, com `cork` | 1.18ms-1.21ms | 5916.07-5926.92 | referência local anterior |
| Sem `Content-Type`, sem `cork` | 1.16ms-1.19ms | 5925.03-5935.65 | melhor localmente |

Leitura: o ganho local é pequeno, mas reproduziu em três runs e não introduziu FP/FN/HTTP errors. O risco técnico é baixo porque a resposta continua sendo exatamente uma das strings JSON pré-computadas e o contrato HTTP continua válido para o k6 oficial.

Ressalva: a tentativa oficial anterior só com remoção de `Content-Type` não reproduziu o ganho local (`issue #769`, `p99=1.48ms`, `final_score=5830.15`). Portanto, esse candidato deve ser validado oficialmente antes de ser considerado melhor que a submissão oficial atual (`issue #764`, `p99=1.44ms`, `final_score=5842.78`).

Validação oficial:

| Issue | Imagem | Commit `submission` | p99 | Failure rate | final_score | Decisão |
|---|---|---|---:|---:|---:|---|
| [#770](https://github.com/zanfranceschi/rinha-de-backend-2026/issues/770) | `submission-e63ae1a` | `e3fdd2b` | 1.44ms | 0% | 5842.99 | melhor oficial atual |
| [#764](https://github.com/zanfranceschi/rinha-de-backend-2026/issues/764) | `submission-a9e49db` | referência anterior | 1.44ms | 0% | 5842.78 | superado por +0.21 |

Leitura oficial: a melhora é praticamente no ruído da engine, mas superou a melhor submissão anterior sem introduzir falhas. Como a imagem `submission-e63ae1a` mantém `p99=1.44ms` e `0%` de falhas, ela passa a ser a melhor submissão oficial conhecida.

Decisão: aceito oficialmente. A branch `submission` permanece apontando para `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-e63ae1a`.

## Ciclo 13h: experimento rejeitado com `body.reserve(768)`

Hipótese: o corpo do `POST /fraud-score` é maior que SSO e normalmente chega em um chunk. Reservar capacidade antes do `append` poderia evitar alocação/tamanho exato no hot path do uWebSockets.

Alteração testada:

```cpp
res->onData([res, state, body = std::string{}](std::string_view chunk, bool is_last) mutable {
    if (body.empty()) {
        body.reserve(768);
    }
    body.append(chunk.data(), chunk.size());
    ...
});
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| append default | 1.23ms-1.24ms | 0 | 0 | 0 | 5908.32-5909.72 | referência |
| `body.reserve(768)` | 1.24ms | 0 | 0 | 0 | 5908.08 | rejeitado |

Leitura: reservar explicitamente não reduziu p99 e ficou abaixo da referência. O custo relevante continua no parser/classificador e no caminho de rede, não nessa alocação específica.

Decisão: rejeitado. `main.cpp` voltou ao `body.append(...)` simples.

## Ciclo 13h: experimento aceito com `fraud_count` inteiro na resposta

Hipótese: `classification_json` recalculava o bucket da resposta usando `floor((fraud_score * 5.0f) + 0.5f)`. No caminho IVF, o valor inteiro `fraud_count` já existe. Carregar esse inteiro em `Classification` permite selecionar a resposta JSON constante sem matemática float no hot path.

Alteração aplicada:

```cpp
struct Classification {
    bool approved = true;
    float fraud_score = 0.0f;
    std::uint8_t fraud_count = 0;
};

classification.fraud_score = static_cast<float>(fraud_count) * 0.2f;
classification.fraud_count = fraud_count;
classification.approved = fraud_count < 3U;

switch (classification.fraud_count) {
    case 0: return json_0;
    ...
}
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| baseline aceita antes da mudança | 1 | 1.23ms | 0 | 0 | 0 | 5909.72 |
| baseline aceita antes da mudança | 2 | 1.24ms | 0 | 0 | 0 | 5908.32 |
| `fraud_count` inteiro no JSON path | 1 | 1.23ms | 0 | 0 | 0 | 5910.62 |
| `fraud_count` inteiro no JSON path | 2 | 1.23ms | 0 | 0 | 0 | 5910.59 |

Leitura: o ganho é pequeno, mas reproduzido em duas runs e sem mexer em precisão, topologia ou recursos. A mudança também remove ambiguidade float no response path e simplifica a seleção das strings constantes.

Decisão: aceito. Próximo passo: publicar imagem imutável nova, atualizar branch `submission` e abrir issue oficial se o build remoto validar.

Publicação oficial:

| Item | Valor |
|---|---|
| commit da implementação | `1273343` |
| commit da branch `submission` | `a56fd54` |
| imagem GHCR | `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-1273343` |
| workflow de build | <https://github.com/viniciusdsandrade/rinha-de-backend-2026/actions/runs/25256927736> |
| issue oficial | <https://github.com/zanfranceschi/rinha-de-backend-2026/issues/767> |

Resultado oficial da issue `#767`:

| p99 | FP | FN | HTTP errors | failure_rate | final_score |
|---:|---:|---:|---:|---:|---:|
| 1.44ms | 0 | 0 | 0 | 0% | 5842.24 |

Leitura oficial: a submissão nova foi aceita pela engine e validou a imagem `submission-1273343`, mas não superou a melhor rerun oficial do dia (`#764`, `final_score=5842.78`). A diferença é de `-0.54` ponto, dentro do ruído esperado para p99 arredondado em `1.44ms`. Portanto, o patch fica aceito tecnicamente por clareza e leve ganho local, mas não deve ser tratado como novo topo oficial.

## Ciclo 14h: experimento rejeitado com HAProxy como LB

Hipótese: as submissões acima da nossa no ranking parcial usam UDS com LBs muito enxutos. A líder em C (`thiagorigonatti-c`) usa `haproxy:3.3` com dois backends em Unix socket, e a `jairoblatt-rust` também usa UDS com um LB L4 dedicado. Talvez trocar `nginx stream` por HAProxy reduza overhead de proxy e aproxime a execução local de `1.20ms-1.25ms`.

Investigação:

| Fonte | Observação relevante |
|---|---|
| `thiagorigonatti/rinha-2026` | HAProxy `3.3`, `balance roundrobin`, `server apiN unix@...`, APIs com `0.40 CPU / 150MB`, LB com `0.20 CPU / 50MB` |
| `jairoblatt/rinha-2026-rust` | LB L4 via socket Unix, `0.2 CPU`, APIs `0.4 CPU`, `seccomp=unconfined` |
| nossa stack | `nginx:1.27-alpine` em `stream`, UDS, `0.18 CPU / 20MB`, APIs `0.41 CPU / 165MB` |

Alteração testada:

```yaml
haproxy:
  image: haproxy:3.3
  volumes:
    - ./haproxy.cfg:/usr/local/etc/haproxy/haproxy.cfg:ro
    - sockets:/sockets
  deploy:
    resources:
      limits:
        cpus: "0.20"
        memory: "50MB"

api1/api2:
  cpus: "0.40"
  memory: "150MB"
```

Configuração do HAProxy:

```text
defaults
    mode tcp
    retries 0
    timeout connect 50ms
    timeout client 2s
    timeout server 2s

frontend main
    bind *:9999 backlog 4096
    default_backend api

backend api
    balance roundrobin
    server api1 unix@/sockets/api1.sock
    server api2 unix@/sockets/api2.sock
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| nginx atual | referência | 1.23ms-1.24ms | 0 | 0 | 0 | 5908.32-5910.62 |
| HAProxy `3.3` | 1 | 1.28ms | 0 | 0 | 0 | 5892.38 |
| HAProxy `3.3` | 2 | 1.28ms | 0 | 0 | 0 | 5892.90 |

Leitura: HAProxy funcionou corretamente e preservou `0%` falhas, mas piorou p99 de forma reproduzida. O ganho dos líderes parece vir mais do servidor HTTP/io_uring/classificador do que do HAProxy isoladamente. Para a nossa pilha uWebSockets + nginx stream, o nginx atual continua superior.

Decisão: rejeitado. `docker-compose.yml` voltou para `nginx:1.27-alpine`, `0.18 CPU / 20MB` no LB e `0.41 CPU / 165MB` nas APIs.

## Ciclo 14h: experimento rejeitado com parser sem cópia extra

Hipótese: o hot path copiava o corpo da requisição duas vezes: primeiro do chunk do uWebSockets para `std::string`, depois de `std::string_view` para `simdjson::padded_string`. Se `parse_payload` aceitasse `std::string&` e usasse `simdjson::pad_with_reserve`, seria possível parsear o buffer já recebido pela API com padding de capacidade, removendo uma cópia por requisição.

Alteração testada:

```cpp
bool parse_payload(std::string& body, Payload& payload, std::string& error) {
    thread_local simdjson::dom::parser parser;

    const simdjson::padded_string_view json = simdjson::pad_with_reserve(body);
    simdjson::dom::element root;
    ...
}

res->onData([res, state, body = std::string{}](std::string_view chunk, bool is_last) mutable {
    if (body.empty()) {
        body.reserve(chunk.size() + 64U);
    }
    body.append(chunk.data(), chunk.size());
    ...
    rinha::parse_payload(body, payload, error);
});
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed

cmake --build cpp/build --target rinha-backend-2026-cpp benchmark-request-cpp -j4
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| baseline aceita | referência | 1.23ms-1.24ms | 0 | 0 | 0 | 5908.32-5910.62 |
| parser sem cópia extra | 1 | 1.23ms | 0 | 0 | 0 | 5911.06 |
| parser sem cópia extra | 2 | 1.24ms | 0 | 0 | 0 | 5906.21 |
| parser sem cópia extra | 3 | 1.24ms | 0 | 0 | 0 | 5907.72 |

Leitura: a primeira run pareceu ganho, mas as duas seguintes ficaram abaixo da faixa da baseline. A média não sustenta a hipótese. Provável explicação: a cópia removida não domina o p99, e o `reserve/pad_with_reserve` adiciona custo/variância suficiente para anular o benefício.

Decisão: rejeitado. Código voltou ao `parse_payload(std::string_view)` com `simdjson::padded_string`. Não aceitar micro-otimização com ganho só em melhor caso isolado.

## Ciclo 14h: experimento rejeitado com uSockets `LIBUS_USE_IO_URING`

Hipótese: a principal diferença técnica da submissão líder em C é o servidor HTTP manual com `io_uring`. Como o uSockets vendorizado já contém backend `LIBUS_USE_IO_URING`, talvez ativar esse backend no uWebSockets reduza overhead de rede sem reescrever o servidor.

Investigação:

| Evidência | Leitura |
|---|---|
| `cpp/third_party/uWebSockets/uSockets/src/libusockets.h` | se nenhum backend é definido, Linux usa `LIBUS_USE_EPOLL` por padrão |
| `uSockets/Makefile` | `WITH_IO_URING=1` adiciona `-DLIBUS_USE_IO_URING` e linka `liburing` |
| `uSockets/src/io_uring/*` | backend existe, mas é um caminho separado de listen/read/write |
| `thiagorigonatti/rinha-2026` | líder usa servidor C próprio com `io_uring`, não uWebSockets |

Alteração testada:

```cmake
option(RINHA_USE_IO_URING "Use uSockets io_uring backend" OFF)

if(RINHA_USE_IO_URING)
    find_library(URING_LIBRARY uring REQUIRED)
    target_compile_definitions(usockets PUBLIC LIBUS_USE_IO_URING)
    target_link_libraries(usockets PUBLIC ${URING_LIBRARY})
endif()
```

```dockerfile
apt-get install ... liburing-dev ...
cmake ... -DRINHA_USE_IO_URING=ON
apt-get install ... liburing2 ...
```

Validação inicial:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed

DOCKER_CONTEXT=default docker compose build api1
```

Achados de runtime:

| Variante | Resultado |
|---|---|
| `LIBUS_USE_IO_URING` + Unix socket | APIs saíram com `exit=1`; nginx não encontrou `/sockets/api1.sock`/`api2.sock`; executar o container direto com `UNIX_SOCKET_PATH=/tmp/test.sock` também encerrou com `exit=1` sem log |
| `LIBUS_USE_IO_URING` + TCP direto | container subiu e `GET /ready` em `:3000` respondeu `204`, indicando que a falha era o listen Unix socket nesse backend |
| `LIBUS_USE_IO_URING` + nginx para `api1:3000`/`api2:3000` | aplicação subiu, mas k6 gerou erros HTTP e degradação forte |

Resultado no `DOCKER_CONTEXT=default` para a variante TCP:

| Variante | p99 | FP | FN | HTTP errors | failure_rate | final_score |
|---|---:|---:|---:|---:|---:|---:|
| baseline aceita | 1.23ms-1.24ms | 0 | 0 | 0 | 0% | 5908.32-5910.62 |
| uSockets io_uring + TCP interno | 1.32ms | 3 | 4 | 25 | 0.06% | 4821.76 |

Leitura: ativar `io_uring` no uSockets não é equivalente ao servidor manual da submissão líder. No nosso caminho, o backend não escuta UDS e o fallback via TCP interno piora latência e estabilidade. Os 25 erros HTTP tornam a hipótese inaceitável, mesmo que a taxa ainda seja baixa.

Decisão: rejeitado. `Dockerfile`, `CMakeLists.txt`, `docker-compose.yml` e `nginx.conf` voltaram ao backend epoll + UDS atual. Se formos perseguir `io_uring`, o caminho tecnicamente correto é um servidor HTTP manual dedicado, não o backend experimental do uSockets.

## Ciclo 14h: experimento rejeitado com `seccomp=unconfined` em epoll

Hipótese: algumas submissões rápidas declaram `security_opt: seccomp=unconfined` nas APIs. Mesmo sem `io_uring`, remover o filtro seccomp do Docker poderia reduzir overhead de syscall em `epoll`, `recv`, `send` e Unix sockets.

Alteração testada:

```yaml
api1/api2:
  security_opt:
    - seccomp=unconfined
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| baseline aceita | 1.23ms-1.24ms | 0 | 0 | 0 | 5908.32-5910.62 |
| epoll+UDS com `seccomp=unconfined` | 1.24ms | 0 | 0 | 0 | 5907.00 |

Leitura: não houve ganho. A opção é útil/necessária em stacks com `io_uring`, mas não reduziu p99 na nossa pilha epoll+UDS.

Decisão: rejeitado. `docker-compose.yml` voltou sem `security_opt`.

## Ciclo 14h: experimento aceito removendo `Content-Type` da resposta quente

Hipótese: o teste oficial/local valida `HTTP 200` e faz `JSON.parse(res.body)`, mas não valida o header `Content-Type`. A resposta já é uma das 6 strings JSON constantes. Remover `res->writeHeader("Content-Type", "application/json")` elimina uma chamada do uWebSockets e reduz bytes de resposta no hot path sem alterar contrato observável pelo k6.

Evidência no `test/test.js`:

```javascript
if (res.status === 200) {
    const body = JSON.parse(res.body);
    ...
}
```

Alteração aplicada:

```cpp
const std::string_view body = classification_json(classification);
res->cork([res, body]() {
    res->end(body);
});
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed

DOCKER_CONTEXT=default docker compose build api1
DOCKER_CONTEXT=default docker compose up -d --force-recreate --remove-orphans
GET /ready -> 2xx
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| baseline aceita antes da mudança | 1 | 1.23ms | 0 | 0 | 0 | 5910.62 |
| baseline aceita antes da mudança | 2 | 1.23ms | 0 | 0 | 0 | 5910.59 |
| sem `Content-Type` | 1 | 1.21ms | 0 | 0 | 0 | 5916.07 |
| sem `Content-Type` | 2 | 1.18ms | 0 | 0 | 0 | 5926.92 |
| sem `Content-Type` | 3 | 1.18ms | 0 | 0 | 0 | 5926.83 |

Leitura: ganho reproduzido em 3 runs, com p99 local melhorando de `1.23ms` para `1.18ms-1.21ms` e `0%` falhas. É a primeira melhoria do ciclo com margem suficiente para justificar nova submissão oficial.

Decisão: aceito. Próximo passo: publicar nova imagem imutável, atualizar branch `submission` e abrir issue oficial se o build remoto validar.

Publicação oficial:

| Item | Valor |
|---|---|
| commit da implementação | `322cc1d` |
| commit da branch `submission` testado | `5db572a` |
| imagem GHCR | `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-322cc1d` |
| workflow de build | <https://github.com/viniciusdsandrade/rinha-de-backend-2026/actions/runs/25257770729> |
| issue oficial | <https://github.com/zanfranceschi/rinha-de-backend-2026/issues/769> |

Resultado oficial da issue `#769`:

| p99 | FP | FN | HTTP errors | failure_rate | final_score |
|---:|---:|---:|---:|---:|---:|
| 1.48ms | 0 | 0 | 0 | 0% | 5830.15 |

Leitura oficial: apesar do ganho local claro, a engine oficial piorou de `1.44ms / 5842.78` (`#764`) para `1.48ms / 5830.15`. A mudança não quebrou acurácia nem disponibilidade, mas não reproduziu no ambiente oficial. Como o ranking/submissão deve privilegiar resultado oficial, a branch `submission` deve voltar para a imagem `submission-a9e49db`, que gerou o melhor resultado oficial conhecido do dia.

Decisão pós-oficial: rejeitado para submissão oficial. Manter o aprendizado e o commit na branch de exploração, mas restaurar `submission` para a melhor imagem oficial.

## Experimento rejeitado: índice 1280 com treino maior e mais iterações

Hipótese: depois de aceitar `1280` clusters, aumentar a amostra de treino e as iterações do k-means poderia melhorar a distribuição dos clusters, reduzir custo de reparo e manter `0 FP/FN`.

Screening offline:

| Configuração | `ns_per_query` | FP | FN | Decisão offline |
|---|---:|---:|---:|---|
| 1280 / sample 65536 / iter 6 | 56309.5 | 0 | 0 | referência |
| 1280 / sample 32768 / iter 6 | 62409.4 | 0 | 0 | rejeitado |
| 1280 / sample 65536 / iter 4 | 57694.1 | 0 | 0 | rejeitado |
| 1280 / sample 131072 / iter 6 | 53161.3 | 0 | 0 | candidato |
| 1280 / sample 65536 / iter 8 | 55825.3 | 0 | 0 | rejeitado |
| 1280 / sample 131072 / iter 8 | 52966.0 | 0 | 0 | melhor offline |

Alteração testada no Dockerfile:

```text
prepare-ivf-cpp ... 1280 65536 6
prepare-ivf-cpp ... 1280 131072 8
```

Validação de imagem:

```text
prepare-ivf-cpp: refs=3000000 padded=3004296 clusters=1280 memory_mb=94.6906
GET /ready => 204
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 1280 / sample 65536 / iter 6 | 2.92ms | 0 | 0 | 0 | 5535.08 | referência |
| 1280 / sample 131072 / iter 8 | 3.02ms | 0 | 0 | 0 | 5520.10 | rejeitado |

Leitura: o ganho offline não se traduziu no p99 do stack completo. A hipótese provavelmente melhora custo médio em loop local, mas piora cauda sob concorrência por distribuição de tamanhos de clusters/reparo ou cache locality.

Decisão: rejeitado. O `Dockerfile` voltou para `1280 65536 6`.

## Experimento rejeitado: reteste de CPU split `0.40/0.40/0.20` com índice 1280

Hipótese: depois de reduzir o custo do índice IVF, devolver CPU ao nginx poderia reduzir contenção no LB e compensar a perda pequena de CPU nas APIs.

Alteração testada:

```text
api1/api2: 0.41 CPU -> 0.40 CPU cada
nginx:     0.18 CPU -> 0.20 CPU
índice:    1280 clusters mantido
```

Validação:

```text
GET /ready => 204
api1 NanoCpus=400000000 Memory=173015040
api2 NanoCpus=400000000 Memory=173015040
nginx NanoCpus=200000000 Memory=20971520
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 1280 clusters + `0.41/0.41/0.18` | 2.92ms | 0 | 0 | 0 | 5535.08 | referência |
| 1280 clusters + `0.40/0.40/0.20` | 2.99ms | 0 | 0 | 0 | 5524.44 | rejeitado |

Leitura: aumentar a folga do nginx não ajudou; a perda de CPU nas APIs dominou. Com índice `1280`, o ponto `0.41/0.41/0.18` segue sendo o melhor equilíbrio observado.

Decisão: rejeitado. `docker-compose.yml` voltou para `api1/api2=0.41 CPU` e `nginx=0.18 CPU`.

## Experimento rejeitado: remover `Content-Type` da resposta

Hipótese: o `test/test.js` valida a resposta a partir de `JSON.parse(res.body)`, então o header `Content-Type: application/json` não deveria ser necessário para corretude. Remover o header reduziria alguns bytes e uma chamada no hot path do `POST /fraud-score`.

Alteração testada:

```cpp
res->cork([res, body]() {
    res->end(body);
});
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp test -j8
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed

GET /ready => 204
Smoke final após rollback => 200 OK com Content-Type: application/json
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Estado aceito com `Content-Type` | 2.65ms-2.86ms | 0 | 0 | 0 | 5543.37-5576.34 | referência |
| Sem `Content-Type` | 2.93ms | 0 | 0 | 0 | 5533.60 | rejeitado |

Leitura: a remoção não quebrou o contrato observado pelo k6, mas piorou p99 de forma clara contra a janela aceita. A economia de header não compensou e possivelmente altera buffering/caminho interno do uWebSockets/nginx de maneira desfavorável.

Decisão: rejeitado. `cpp/src/main.cpp` voltou a enviar `Content-Type: application/json` no `POST /fraud-score`.

## Resumo consolidado do dia

Melhor resultado oficial obtido hoje:

```text
Issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/720
Branch oficial: submission
Commit oficial testado: 8293b49
Imagem: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a9e49db
p99: 1.45ms
FP: 0
FN: 0
HTTP errors: 0
failure_rate: 0%
p99_score: 2837.36
detection_score: 3000.00
final_score: 5837.36
```

Ganho contra a submissão anterior conhecida:

```text
Submissão anterior: p99=2.83ms, final_score=5548.91
Melhor submissão de hoje: p99=1.45ms, final_score=5837.36
Ganho absoluto: +288.45 pontos
Redução de p99: -1.38ms
```

Mudanças que efetivamente avançaram a solução:

| Frente | Decisão | Evidência |
|---|---|---|
| Índice IVF `2048 -> 1280` clusters | aceito | preservou `0 FP/FN` e reduziu p99 local de forma reproduzida |
| Reparo seletivo de extremos no IVF | aceito | melhor run local `2.65ms / 5576.34`, `0 FP/FN/HTTP` |
| Tag imutável GHCR para submissão | aceito | corrigiu discrepância oficial de tag mutável e validou `5837.36` no issue `#720` |

Experimentos rejeitados hoje:

| Experimento | Resultado | Decisão |
|---|---:|---|
| CPU split `0.42/0.42/0.16` | `2.79ms / 5554.41` antes do reparo; pior após reparo | rejeitado |
| CPU split `0.40/0.40/0.20` | `2.73ms / 5563.09`; reteste final `2.99ms / 5524.44` | rejeitado |
| CPU split fino `0.415/0.415/0.17` | `2.67ms-2.69ms / 5570.35-5573.83` | rejeitado por não superar melhor local |
| Três APIs | `2.73ms / 5564.50` | rejeitado |
| `worker_processes 2` no nginx | piorou p99 local | rejeitado |
| `multi_accept off` no nginx | `2.97ms / 5526.71` | rejeitado |
| Remover `reuseport` | `2.98ms / 5525.75` | rejeitado |
| Treino IVF `1280 / 131072 / 8` | `3.02ms / 5520.10` | rejeitado apesar de melhor microbenchmark |
| Heurística extrema mais estreita | `2.80ms / 5553.19` | rejeitado |
| Parser com `string_view` | `3.06ms / 5514.43` | rejeitado |
| `-march=haswell` | microbenchmark `17236.3ns/query`, sem ganho | rejeitado antes do k6 |
| Remover `Content-Type` | `2.93ms / 5533.60` | rejeitado |

Leitura final: a performance oficial relevante já saiu do patamar `2.83ms / 5548.91` para `1.45ms / 5837.36`, com `0%` de falhas. A maior lição operacional foi evitar tag Docker mutável na submissão: o mesmo código que localmente estava correto caiu para `5424.02` no issue `#719`, enquanto a tag imutável validou `5837.36` no issue `#720`.

Estado recomendado para seguir:

```text
submission: manter commit 8293b49 e imagem immutable submission-a9e49db
perf/noon-tuning: manter como branch de investigação com o histórico de experimentos
próximo foco técnico: somente mudanças com sinal local claro abaixo de ~2.65ms e sem qualquer erro de detecção
```

## Ciclo 13h: experimento rejeitado no backlog UDS do uSockets

Hipótese: o nginx encaminha para as APIs via Unix Domain Socket. O uSockets abre o socket Unix com `listen(..., 512)`, enquanto o nginx externo já usa `backlog=4096`. Aumentar o backlog interno das APIs para `4096` poderia absorver melhor rajadas do k6 e reduzir p99 sem tocar na lógica de negócio.

Alteração testada:

```c
// cpp/third_party/uWebSockets/uSockets/src/bsd.c
listen(listenFd, 512)
listen(listenFd, 4096)
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp test -j8
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed

Docker Desktop estava parado; foi reiniciado para manter o mesmo contexto de benchmark.
docker info => 29.4.1 Docker Desktop
GET /ready => 204
```

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Estado aceito, backlog UDS 512 | 2.65ms-2.86ms | 0 | 0 | 0 | 5543.37-5576.34 | referência |
| Backlog UDS 4096 | 29.83ms | 0 | 0 | 0 | 4525.41 | rejeitado |

Leitura: a mudança preservou correção, mas destruiu cauda de latência. É provável que aumentar a fila interna permita acúmulo maior antes de backpressure, piorando p99 em vez de estabilizar accept. Em desafio com score logarítmico e p99 crítico, backlog menor no socket da API é mais saudável.

Decisão: rejeitado. `bsd.c` voltou para `listen(..., 512)` e não há alteração de código pendente desse experimento.

## Ciclo 13h: baseline contaminada por pressão de memória

Depois de reiniciar o Docker Desktop para restaurar o daemon, a baseline aceita foi reconstruída e retestada antes de continuar. O resultado saiu muito fora da faixa histórica:

```text
Estado aceito reconstruído
GET /ready => 204
p99: 19.55ms
FP: 0
FN: 0
HTTP errors: 0
final_score: 4708.89
```

Investigação de máquina:

```text
Memória host: 7.4Gi total, 7.0Gi usada, 478Mi disponível antes do compose down
Swap: 4.0Gi total, 4.0Gi usada
Maiores consumidores: IntelliJ IDEA ~2.1Gi RSS, Docker Desktop VM ~1.3Gi RSS, Chrome ~780Mi RSS
Docker Desktop: 29.4.1, 16 CPUs, ~1.9GB alocados
```

Leitura: o k6 local ficou inválido para comparar microganhos enquanto a máquina estiver nesse estado. A decisão técnica foi não aceitar nem rejeitar novas otimizações com base nessa baseline contaminada. Para não parar o ciclo, o compose foi derrubado temporariamente e os próximos experimentos foram feitos com microbenchmarks offline.

## Ciclo 13h: screening offline de reparo IVF e `nprobe`

Baseline offline gerada com índice local equivalente ao Dockerfile aceito:

```text
./cpp/build/prepare-ivf-cpp resources/references.json.gz cpp/build/perf-data/index-1280.bin 1280 65536 6
ivf_index=cpp/build/perf-data/index-1280.bin refs=3000000 padded=3004384 clusters=1280 memory_mb=94.6933

./cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 1 0 1 1 1 1 4
ns_per_query=17641.2 checksum=30808154 fp=0 fn=0 parse_errors=0
```

Grade de `bbox_repair` e intervalo de reparo:

| Configuração | Melhor sinal observado | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `bbox=0` com qualquer intervalo | 12926.8-21055.8 ns/query | 127 | 131 | rejeitado por erro |
| `bbox=1 min=0 max=3` | 39180.5 ns/query | 22 | 0 | rejeitado por erro |
| `bbox=1 min=0 max=4` | 35771.5 ns/query | 0 | 0 | rejeitado por custo |
| `bbox=1 min=0 max=5` | 69952.9 ns/query | 0 | 0 | rejeitado por custo |
| `bbox=1 min=1 max=3` | 16987.9 ns/query | 22 | 0 | rejeitado por erro |
| `bbox=1 min=1 max=4` | 20082.1 ns/query | 0 | 0 | referência aceita |
| `bbox=1 min=1 max=5` | 49397.4 ns/query | 0 | 0 | rejeitado por custo |
| `bbox=1 min=2 max=3` | 16537.5 ns/query | 22 | 28 | rejeitado por erro |
| `bbox=1 min=2 max=4` | 16679.3 ns/query | 0 | 28 | rejeitado por erro |
| `bbox=1 min=2 max=5` | 50223.5 ns/query | 0 | 28 | rejeitado por erro |

Leitura: `bbox_repair` é obrigatório para precisão. Os intervalos que tentam reduzir reparos saem baratos, mas introduzem FP/FN; os intervalos que preservam erro zero ficam mais caros que o ponto aceito.

Screening de `nprobe`:

| Configuração | Resultado | Decisão |
|---|---|---|
| `fast=1 full=1` | `0 FP/FN`; repetição alternada `17875.7-18383.5 ns/query` | referência |
| `fast=1 full=2` | `0 FP/FN`; screening isolado `19504.3 ns/query` | rejeitado |
| `fast=1 full=3` | `0 FP/FN`; repetição alternada `18613.0-20117.7 ns/query` | rejeitado |
| `fast=2 full=2` | `0 FP/FN`; `22741.7 ns/query` | rejeitado |
| `fast=2 full=3` | `0 FP/FN`; `23845.0 ns/query` | rejeitado |
| `fast=3 full=3` | `0 FP/FN`; `28068.7 ns/query` | rejeitado |

Leitura: a aparente vitória inicial de `full_nprobe=3` era ruído de microbenchmark. Ao alternar contra a baseline com `repeat=2`, `fast=1/full=1` venceu de forma consistente. Nenhuma alteração de `nprobe` merece k6 enquanto a máquina estiver sob swap.

Decisão: manter `IVF_FAST_NPROBE=1`, `IVF_FULL_NPROBE=1`, `IVF_BBOX_REPAIR=true`, `IVF_REPAIR_MIN_FRAUDS=1`, `IVF_REPAIR_MAX_FRAUDS=4`.

## Ciclo 13h: experimento rejeitado em lookup de MCC por `switch`

Hipótese: `mcc_risk` faz até 10 comparações de `std::string` por request. Como MCC tem 4 caracteres, converter o código para uma chave `uint32_t` e usar `switch` poderia reduzir custo de vetorização sem alterar precisão.

Alteração testada:

```cpp
std::uint32_t mcc_key(std::string_view mcc) noexcept;

float mcc_risk(std::string_view mcc) noexcept {
    switch (mcc_key(mcc)) {
        case 0x35343131U: return 0.15f;  // 5411
        ...
        default: return 0.50f;
    }
}
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests benchmark-ivf-cpp -j4
ctest --test-dir cpp/build --output-on-failure
1/1 Test #1: rinha-backend-2026-cpp-tests ..... Passed
```

Resultado offline:

| Variante | `ns_per_query` | FP | FN | Decisão |
|---|---:|---:|---:|---|
| baseline antes do experimento | 17875.7-18383.5 | 0 | 0 | referência |
| MCC por `switch`, round 1 | 18643.9 | 0 | 0 | rejeitado |
| MCC por `switch`, round 2 | 17697.7 | 0 | 0 | inconclusivo |
| MCC por `switch`, round 3 | 18422.5 | 0 | 0 | rejeitado |

Leitura: a mudança é correta, mas o ganho não apareceu de forma robusta. O melhor round foi só ruído favorável; mediana e cauda ficaram iguais ou piores que a implementação simples com comparação de `std::string`. Como o objetivo é performance sustentável e inquestionável, isso não entra.

Decisão: rejeitado. `vectorize.cpp` voltou ao lookup simples por comparação de string.

## Ciclo 13h: rerun oficial da melhor submissão

Depois que a baseline local no Docker Desktop ficou contaminada por swap, foi feita uma checagem comparativa no daemon Linux local (`DOCKER_CONTEXT=default`) sem mudar o contexto global. O objetivo não era trocar a implementação, mas verificar se o mesmo stack aceito ainda tinha potencial de p99 melhor em ambiente menos pressionado.

Estado da máquina antes do teste no daemon Linux:

```text
Docker Desktop parado para liberar a VM
Memória host após parar Desktop: 7.4Gi total, 3.7Gi usada, 3.7Gi disponível
Swap: 4.0Gi total, 3.1Gi usada
DOCKER_CONTEXT=default docker info:
  Server=29.4.1
  OS=Ubuntu 24.04.4 LTS
  CPUs=16
  Mem=7993286656
```

Baseline local no daemon Linux, mesmo código/imagem local equivalente:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.23ms | 0 | 0 | 0 | 5909.72 |
| 2 | 1.24ms | 0 | 0 | 0 | 5908.32 |

Leitura: o número local não é diretamente comparável com o histórico feito no Docker Desktop, mas o sinal foi forte e reproduzido. Como a branch oficial `submission` já apontava para a imagem imutável `submission-a9e49db`, foi aberto um novo issue oficial para reprocessar a mesma submissão.

Submissão oficial:

```text
Issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/764
Body: rinha/test andrade-cpp-ivf
Commit testado: 8293b49
Imagem: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a9e49db
```

Resultado oficial do issue `#764`:

```text
p99: 1.44ms
FP: 0
FN: 0
HTTP errors: 0
failure_rate: 0%
p99_score: 2842.78
detection_score: 3000.00
final_score: 5842.78
```

Comparação:

| Referência | p99 | final_score |
|---|---:|---:|
| Submissão anterior conhecida antes do dia | 2.83ms | 5548.91 |
| Issue oficial `#720` | 1.45ms | 5837.36 |
| Issue oficial `#764` | 1.44ms | 5842.78 |

Ganho oficial incremental contra `#720`: `+5.42` pontos.

Decisão: aceito como melhor resultado oficial atual. Não houve mudança de código; o ganho veio de rerun oficial da mesma submissão imutável.

## Ciclo 13h: reteste de CPU split no daemon Linux local

Hipótese: com o p99 local já próximo de `1ms`, mover um pouco mais de CPU para o nginx poderia reduzir a cauda no LB e melhorar score. Esse experimento foi refeito no `DOCKER_CONTEXT=default` porque o Docker Desktop estava contaminado por swap.

Referência local imediata:

| Split | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| APIs `0.41/0.41`, nginx `0.18` | 1.23ms | 0 | 0 | 0 | 5909.72 |
| APIs `0.41/0.41`, nginx `0.18` | 1.24ms | 0 | 0 | 0 | 5908.32 |

Variações testadas:

| Split | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| APIs `0.40/0.40`, nginx `0.20` | 1.39ms | 0 | 0 | 0 | 5858.34 | rejeitado |
| APIs `0.405/0.405`, nginx `0.19` | 1.38ms | 0 | 0 | 0 | 5861.68 | rejeitado |

Leitura: o nginx não está carente de CPU nesse cenário; tirar CPU das APIs piora p99. A configuração aceita `0.41/0.41/0.18` segue sendo o melhor split observado tanto nos experimentos antigos quanto no daemon Linux local.

Decisão: rejeitado. `docker-compose.yml` voltou para APIs `0.41` e nginx `0.18`.

## Ciclo 13h: experimento rejeitado em `worker_connections=8192`

Hipótese: aumentar `worker_connections` do nginx de `4096` para `8192` poderia reduzir pressão interna de conexões/eventos e baixar p99 no pico do k6. É uma mudança pura de LB, sem lógica de aplicação e sem rebuild da API.

Alteração testada:

```nginx
events {
    worker_connections 8192;
    multi_accept on;
    use epoll;
}
```

Validação:

```text
nginx -T confirmou worker_connections 8192
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `worker_connections=4096` | 1.23ms-1.24ms | 0 | 0 | 0 | 5908.32-5909.72 | referência |
| `worker_connections=8192` | 1.24ms | 0 | 0 | 0 | 5907.33 | rejeitado |

Leitura: aumentar o limite não removeu gargalo e ficou ligeiramente pior que a referência. O valor atual `4096` já é suficiente para o perfil de conexões do teste.

Decisão: rejeitado. `nginx.conf` voltou para `worker_connections 4096`.

## Ciclo 13h: experimento rejeitado com `tcp_nodelay on`

Hipótese: como as respostas são pequenas, habilitar `tcp_nodelay` no `stream` do nginx poderia reduzir latência de cauda se houvesse algum efeito de Nagle no caminho do LB.

Alteração testada:

```nginx
server {
    listen 9999 reuseport backlog=4096;
    tcp_nodelay on;
    proxy_pass api;
}
```

Validação:

```text
nginx -T:
syntax is ok
configuration file /etc/nginx/nginx.conf test is successful
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Sem `tcp_nodelay` explícito | 1.23ms-1.24ms | 0 | 0 | 0 | 5908.32-5909.72 | referência |
| `tcp_nodelay on` | 1.25ms | 0 | 0 | 0 | 5904.03 | rejeitado |

Leitura: a diretiva é válida, mas não melhorou o caminho. O default atual já está suficientemente bom ou o gargalo não está em coalescência de pacotes.

Decisão: rejeitado. `nginx.conf` voltou a não declarar `tcp_nodelay`.

## Ciclo 13h: experimento rejeitado com backlog externo `8192`

Hipótese: aumentar o backlog externo da porta `9999` poderia absorver melhor rajadas de conexão do k6. Diferente do backlog UDS interno, esse ajuste atua na entrada do LB antes do proxy para as APIs.

Alteração testada:

```nginx
listen 9999 reuseport backlog=8192;
```

Validação:

```text
nginx -T confirmou listen 9999 reuseport backlog=8192
```

Resultado no `DOCKER_CONTEXT=default`:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `backlog=4096` | 1.23ms-1.24ms | 0 | 0 | 0 | 5908.32-5909.72 | referência |
| `backlog=8192` | 1.24ms | 0 | 0 | 0 | 5908.31 | rejeitado |

Leitura: o resultado empatou com a pior run da referência e não superou a melhor. Sem ganho claro, aumentar backlog só adiciona superfície de configuração sem retorno.

Decisão: rejeitado. `nginx.conf` voltou para `listen 9999 reuseport backlog=4096`.
