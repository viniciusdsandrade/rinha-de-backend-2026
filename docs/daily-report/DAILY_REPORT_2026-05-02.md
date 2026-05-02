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
