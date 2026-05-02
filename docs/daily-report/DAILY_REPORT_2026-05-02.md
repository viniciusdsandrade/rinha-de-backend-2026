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
