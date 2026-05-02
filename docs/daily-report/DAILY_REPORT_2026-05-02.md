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
