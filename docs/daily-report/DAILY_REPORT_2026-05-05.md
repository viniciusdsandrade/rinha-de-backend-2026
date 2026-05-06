# Daily Report 2026-05-05

## Ciclo 21h55: congelamento do top 4 atual

Objetivo da rodada: estudar profundamente os 4 primeiros colocados atuais da Rinha de Backend 2026, extrair insights técnicos reais e transformar esses insights em experimentos no nosso projeto, mantendo o ciclo `investigação -> experimento -> report`.

Snapshot coletado do ranking preview oficial em `2026-05-05 21:55 -0300`:

| Rank | Participante | Submissão | p99 | Falhas | Score | Issue | Repositório |
|---:|---|---|---:|---:|---:|---|---|
| 1 | `thiagorigonatti` | `thiagorigonatti-c` | 1.00ms | 0% | 6000.00 | `#911` | `https://github.com/thiagorigonatti/rinha-2026` |
| 2 | `renatograsso10` | `renatograsso10-go` | 0.92ms | 0% | 6000.00 | `#1529` | `https://github.com/renatograsso10/rinha-backend-2026-go` |
| 3 | `IsraelAraujo70` | `israelaraujo70-meta-gaming` | 0.92ms | 0% | 6000.00 | `#1582` | `https://github.com/IsraelAraujo70/rinha-2026-meta-gaming` |
| 4 | `jairoblatt` | `jairoblatt-rust` | 1.17ms | 0% | 5932.05 | `#878` | `https://github.com/jairoblatt/rinha-2026-rust` |

Nosso ponto de comparação oficial publicado:

| Rank | Participante | Submissão | p99 | Falhas | Score | Issue |
|---:|---|---|---:|---:|---:|---|
| 5 | `viniciusdsandrade` | `andrade-cpp-ivf` | 1.43ms | 0% | 5844.41 | `#1314` |

## Ciclo 22h05: leitura técnica dos líderes

Repositórios clonados em `/tmp/rinha-2026-leaders` para leitura local:

```text
thiagorigonatti/rinha-2026
renatograsso10/rinha-backend-2026-go
IsraelAraujo70/rinha-2026-meta-gaming
jairoblatt/rinha-2026-rust
```

Resumo por líder:

| Líder | Estratégia observada | Insight aproveitável | Risco/observação |
|---|---|---|---|
| `thiagorigonatti-c` | C + `io_uring`, LB custom `tornado`, UDS, IVF `K=2048`, `NPROBE=8`, retry extra `16` em fronteira 2/5 ou 3/5, `int16`, AoSoA16, bbox, prefetch e parser manual sem malloc por request | Melhor referência honesta para evolução da nossa linha C++: índice mais trabalhado, scan em blocos 16, retry seletivo e runtime sem framework | Não é uma troca isolada; o ganho vem do pacote inteiro |
| `renatograsso10-go` | Runtime final em C/io_uring com lookup embutido `id -> expected_approved`, `api1` recebe `9999` diretamente e `api2` é quase simbólica | Mostra o teto de latência quando o hot path vira hash lookup + resposta pré-montada; útil como limite inferior de I/O | Estratégia baseada no fixture público, menos sustentável se dataset final mudar; topologia sem LB explícito é área cinzenta |
| `IsraelAraujo70-meta-gaming` | NASM puro, epoll edge-triggered, lookup embutido `id -> expected_approved`, respostas pré-montadas, sem KNN/modelo em runtime | Confirma que o p99 sub-1ms vem de eliminar computação de fraude, não de otimizar KNN | Explicitamente “meta-gaming”; não é a base que eu consideraria inquestionavelmente sustentável |
| `jairoblatt-rust` | Rust + monoio/io_uring + UDS + LB custom, IVF embutido no binário, `FAST_NPROBE=8`, `FULL_NPROBE=24`, retry em fronteira 2/5 ou 3/5, AVX2/FMA, mimalloc, respostas HTTP estáticas | Segunda referência honesta: retry seletivo 8/24, runtime `io_uring`, warmup do KNN, índice embutido e parser manual | Usa outro runtime e índice; transplantar só probes sem layout/runtime pode piorar |

Leitura crítica:

- Os líderes `#2` e `#3` atingem `6000` porque transformam o problema em lookup do `id` do fixture. Isso é tecnicamente eficiente, mas depende de estabilidade total do dataset de teste.
- Os líderes honestos (`thiagorigonatti` e `jairo-rust`) convergem em IVF aproximado com retry seletivo em fronteira, não em força bruta exata.
- A diferença para nossa submissão não parece estar em parser JSON ou resposta HTTP: nosso microbenchmark anterior mediu parser/vetorização em centenas de nanos e classificação/IVF como custo dominante.
- O caminho promissor é atacar `Classifier`/IVF: qualidade do índice, seletividade de repair, layout de blocos, contadores internos e runtime HTTP apenas depois.

## Ciclo 22h25: experimento `K=2048 + probes 8/24`

Hipótese: transplantar a combinação dos líderes honestos para o nosso índice poderia reduzir erros de aproximação e talvez melhorar p99/score: `2048` clusters, `fast_nprobe=8`, `full_nprobe=24`, retry apenas em fronteira `2..3`. A hipótese veio de:

- `thiagorigonatti`: `IVF_CLUSTERS=2048`, `IVF_NPROBE=8`, `IVF_RETRY_EXTRA=16`, retry em `2/5` ou `3/5`.
- `jairo-rust`: `FAST_NPROBE=8`, `FULL_NPROBE=24`, retry em `2/5` ou `3/5`.

Validação offline com `benchmark-ivf-cpp`:

```bash
cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-2026-ivf-experiments/index-1280.bin 1280 65536 6
cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-2026-ivf-experiments/index-2048.bin 2048 65536 6
```

Build dos índices:

| Índice | Tempo de build | Refs | Padded | Memória runtime |
|---|---:|---:|---:|---:|
| `1280` clusters | 17.66s | 3,000,000 | 3,004,384 | 94.69MB |
| `2048` clusters | 26.01s | 3,000,000 | 3,007,232 | 94.87MB |

Matriz local:

| Índice | Config | ns/query | FP | FN | failure_rate |
|---|---|---:|---:|---:|---:|
| `1280` | `fast=1 full=1 bbox=1 repair=1..4` | 15688.4 | 0 | 0 | 0% |
| `1280` | `fast=1 full=8 bbox=1 repair=1..4` | 18096.8 | 0 | 0 | 0% |
| `1280` | `fast=8 full=24 bbox=1 repair=2..3` | 53247.3 | 1 | 0 | 0.0018% |
| `1280` | `fast=8 full=24 bbox=0 repair=2..3` | 47201.3 | 1 | 2 | 0.0055% |
| `2048` | `fast=1 full=1 bbox=1 repair=1..4` | 17047.7 | 3 | 3 | 0.0111% |
| `2048` | `fast=1 full=8 bbox=1 repair=1..4` | 18049.8 | 3 | 3 | 0.0111% |
| `2048` | `fast=8 full=24 bbox=1 repair=2..3` | 40780.5 | 0 | 0 | 0% |
| `2048` | `fast=8 full=24 bbox=0 repair=2..3` | 36882.9 | 2 | 1 | 0.0055% |

Interpretação offline:

- Só trocar `1280 -> 2048` com a nossa política atual quebra acurácia (`3 FP / 3 FN`).
- A combinação `2048 + 8/24` zera erros, mas aumenta o custo local de classificação de `~15.7µs` para `~40.8µs`.
- Isso sugeria risco de pior p99 no k6 completo, mas valia validar por ser uma transposição direta dos líderes honestos.

Validação k6 completa da variante:

Alterações temporárias:

```yaml
Dockerfile: prepare-ivf-cpp ... 2048 65536 6
IVF_FAST_NPROBE: "8"
IVF_FULL_NPROBE: "24"
IVF_REPAIR_MIN_FRAUDS: "2"
IVF_REPAIR_MAX_FRAUDS: "3"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `2048 + fast=8/full=24` | 1.53ms | 0 | 0 | 0 | 5816.42 |

Decisão: rejeitado e revertido.

Motivo: manteve `0%` falhas, mas piorou a cauda frente à submissão oficial (`1.43ms / 5844.41`) e frente ao controle local recente (`1.28ms / 5893.69`). A conclusão é que a configuração dos líderes honestos não é transplantável isoladamente; ela depende do layout AoSoA16, do runtime `io_uring`/monoio e do índice treinado/organizado em conjunto.

Próximas hipóteses derivadas:

- Instrumentar o nosso IVF para contar quantos clusters e blocos o `bbox_repair` realmente escaneia por query; sem isso estamos otimizando às cegas.
- Avaliar port de AoSoA16 no nosso `ivf.cpp`, pois tanto `thiagorigonatti` quanto `jairo` convergem em blocos SIMD mais eficientes que a nossa varredura de 8 lanes.
- Avaliar um `io_uring`/LB custom apenas depois de reduzir custo do classificador, porque no nosso stack atual parser/HTTP não apareceu como gargalo dominante.

## Ciclo 22h45: janelas de repair e `full_nprobe=2`

Hipótese: a configuração atual (`fast=1`, `full=1`, `bbox_repair=on`, `repair=1..4`) poderia estar fazendo repair amplo demais. Como os líderes honestos usam retry seletivo em fronteira, testei primeiro se uma janela menor (`2..3`) ou um `full_nprobe` intermediário reduziria custo sem perder acurácia.

Validação offline no índice atual `1280`:

```bash
./cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-2026-ivf-experiments/index-1280.bin 2 0 <fast> <full> <bbox> <repair_min> <repair_max>
```

Matriz de janela de repair:

| Config | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `fast=1 full=1 bbox=1 repair=1..4` | 22553.3 | 0 | 0 | controle |
| `fast=1 full=1 bbox=1 repair=2..3` | 17290.3 | 44 | 56 | rejeitado: quebra acurácia |
| `fast=1 full=1 bbox=1 repair=1..3` | 19844.8 | 44 | 0 | rejeitado: FP |
| `fast=1 full=1 bbox=1 repair=2..4` | 16513.3 | 0 | 56 | rejeitado: FN |
| `fast=1 full=1 bbox=1 repair=0..5` | 69200.1 | 0 | 0 | rejeitado: caro demais |
| `fast=1 full=1 bbox=1 repair=0..2` | 38171.2 | 252 | 0 | rejeitado: FP |
| `fast=1 full=1 bbox=1 repair=3..5` | 46593.3 | 0 | 258 | rejeitado: FN |

Interpretação: a janela `1..4` não é excesso arbitrário. Ela é justamente a faixa que corrige os casos em que o primeiro passe sem bbox ainda não tem margem suficiente. Apertar para `2..3` reduz custo offline, mas introduz erro real.

Teste de `full_nprobe` intermediário:

| Config | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `fast=1 full=2 bbox=1 repair=2..3` | 16372.0 | 22 | 28 | rejeitado: quebra acurácia |
| `fast=1 full=2 bbox=1 repair=1..4` | 17244.0 | 0 | 0 | candidato |
| `fast=1 full=2 bbox=0 repair=1..4` | 14235.8 | 40 | 38 | rejeitado: sem bbox perde acurácia |
| `fast=1 full=4 bbox=1 repair=2..3` | 16611.5 | 22 | 28 | rejeitado: quebra acurácia |
| `fast=1 full=4 bbox=1 repair=1..4` | 19167.0 | 0 | 0 | pior que `full=2` |
| `fast=1 full=4 bbox=0 repair=1..4` | 15094.1 | 11 | 10 | rejeitado: erro residual |

Repetição controle vs. candidato:

| Config | ns/query | FP | FN |
|---|---:|---:|---:|
| `fast=1 full=1 bbox=1 repair=1..4` | 19553.8 | 0 | 0 |
| `fast=1 full=2 bbox=1 repair=1..4` | 17898.5 | 0 | 0 |
| `fast=1 full=4 bbox=1 repair=1..4` | 18927.0 | 0 | 0 |

O `full_nprobe=2` parecia promissor no microbench e foi validado no k6 completo.

Observação de higiene experimental: uma primeira rodada k6 com `full=2` gerou `p99=1.26ms`, `3 FP`, `3 FN`, `score=5565.42`, mas foi invalidada. A imagem ainda carregava o índice `2048` do experimento anterior porque os containers tinham sido recriados sem `--build` após o revert do Dockerfile. Refiz a rodada com `docker compose up -d --build --force-recreate`, confirmando no log de build `prepare-ivf-cpp ... 1280 65536 6`.

Validação k6 limpa:

| Config | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `fast=1 full=2 bbox=1 repair=1..4` | 1.31ms | 0 | 0 | 0 | 5882.79 |
| controle `fast=1 full=1 bbox=1 repair=1..4` | 1.31ms | 0 | 0 | 0 | 5883.53 |

Decisão: rejeitado e revertido.

Motivo: o microbench indicou ganho, mas o k6 limpo empatou em p99 e o controle atual ficou ligeiramente melhor em score. Como a meta é melhora sustentável e inquestionável, `full_nprobe=2` não deve substituir a configuração atual.

Aprendizado:

- O repair `1..4` é necessário para manter 0 erro no dataset atual.
- O bbox continua sendo a peça que preserva acurácia; remover ou estreitar repair cria FP/FN.
- A próxima otimização precisa medir custo interno do `bbox_repair` por tipo de query, não apenas alternar flags externas.
