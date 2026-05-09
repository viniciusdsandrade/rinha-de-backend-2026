# Daily Report - 2026-05-09

## Contexto operacional

Objetivo do ciclo: continuar investigação -> hipótese -> experimento -> report até `15h00` de `2026-05-09`, buscando melhora sustentável de performance para a submissão da Rinha de Backend 2026.

Estado seguro de submissão no início do dia:

```text
branch: submission
commit: f2a5b98 restore best official submission image
imagem: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a477d55
melhor evidência oficial histórica: issue #2316, p99 1.20ms, 0% falhas, final_score 5921.80
última reexecução oficial: issue #2338, p99 1.23ms, 0% falhas, final_score 5910.58
```

Decisão de segurança: não mexer na branch `submission` sem uma evidência nova que supere claramente a melhor submissão oficial.

## Ciclo 10h55: retomada e baseline de contexto

Validações iniciais:

```text
data/hora local: 2026-05-09 10:55:38 -03
branch experimental: perf/noon-tuning
último commit experimental: 019ea08 report overnight tuning results
branch submission: submission
último commit submission: f2a5b98 restore best official submission image
```

Resultado: ambiente limpo para novas hipóteses, sem container pendurado e sem patch de código ativo.

## Ciclo 11h00: varredura de clusters IVF intermediários

Hipótese: o ponto ótimo de clusters poderia estar entre `1024`, `1280`, `1536` e `2048`; clusters maiores reduzem blocos por cluster, mas podem aumentar custo de centroides/bbox e afetar acurácia.

Comandos executados:

```text
prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1536.bin 1536 65536 6
benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1536.bin 3 0 1 1 1 1 4 1 0 0 0

prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1024.bin 1024 65536 6
benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1024.bin 3 0 1 1 1 1 4 1 0 0 0
```

Resultados:

| Clusters | ns/query | FP | FN | Falhas | avg primary blocks | Decisão |
|---:|---:|---:|---:|---:|---:|---|
| 1024 | 18054.10 | 3 | 6 | 0.0055% | 408.823 | rejeitado |
| 1280 | ~8604-16696 | 0 | 0 | 0% | 322.897 | mantido como referência |
| 1536 | 17632.30 | 6 | 0 | 0.0037% | 271.393 | rejeitado |
| 2048 | 7058.16 com nprobe 1; 22503.20 com nprobe 4 | 9/0 | 9/0 | 0.0111%/0% | 204.981+ | rejeitado |

Aprendizado: reduzir/aumentar clusters fora de `1280` piora a fronteira acurácia/performance no desenho atual. `2048` parece rápido com `nprobe=1`, mas erra; quando corrige com `nprobe=4`, fica muito mais lento.

## Ciclo 11h15: ordem de dimensões no `bbox_lower_bound`

Hipótese: testar dimensões mais variáveis primeiro no `bbox_lower_bound` poderia reduzir o custo de abortar clusters cujo lower bound já excede o top-5 atual, sem mudar o resultado matemático.

Ordem calculada por variância global do `references.json.gz`:

```text
[6, 10, 9, 5, 11, 2, 4, 7, 0, 1, 8, 12, 3, 13]
```

Patch temporário:

```cpp
constexpr std::array<std::uint8_t, kDimensions> kBboxDimensionOrder{
    6, 10, 9, 5, 11, 2, 4, 7, 0, 1, 8, 12, 3, 13
};

for (const std::size_t dim : kBboxDimensionOrder) {
    ...
}
```

Validação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultado offline:

| Variante | ns/query | FP | FN | Falhas |
|---|---:|---:|---:|---:|
| bbox em ordem por variância | 18435.30 | 0 | 0 | 0% |
| bbox em ordem natural, mesmo índice | 16696.00 | 0 | 0 | 0% |

Decisão: **rejeitado e revertido**.

Aprendizado: a ordem por variância não melhorou o early abort; provavelmente prejudicou localidade/predição do caminho quente e não reduziu trabalho suficiente. Manter ordem natural.
