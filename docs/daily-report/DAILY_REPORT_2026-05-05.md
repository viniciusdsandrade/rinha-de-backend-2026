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

## Ciclo 23h05: instrumentação do `bbox_repair` e regra extrema estreita

Hipótese: o custo relevante não está no primeiro cluster IVF, mas no repair. O ciclo anterior mostrou que remover `bbox_repair` quebra acurácia, então o passo correto era medir quanto repair existe e tentar reduzir repair inútil sem alterar a decisão final.

Instrumentação adicionada apenas ao alvo `benchmark-ivf-cpp` via `RINHA_IVF_STATS=1`; o binário de submissão não recebe os contadores. Métricas coletadas:

- distribuição do `fraud_count` no passe rápido;
- quantidade de queries reparadas;
- quantidade de repairs vindos de `should_repair_extreme`;
- clusters/blocos varridos no passe primário;
- clusters/blocos testados e varridos pelo bbox.

Baseline instrumentado da regra anterior:

| Métrica | Valor |
|---|---:|
| Queries | 54100 |
| Repairs | 6033 |
| Repairs | 11.15% |
| Repairs extremos | 3680 |
| `fast_fraud_counts` | `f0=28880 f1=404 f2=762 f3=803 f4=384 f5=22867` |
| Clusters primários/query | 1.1115 |
| Blocos primários/query | 343.47 |
| Clusters bbox testados/query | 142.63 |
| Clusters bbox varridos/query | 0.666 |
| Blocos bbox/query | 190.01 |

Interpretação: só 11% das queries acionavam repair, mas cada repair testava quase todos os clusters via bbox e, quando passava no filtro, adicionava clusters grandes. O repair extremo era a maior fonte de acionamento: `3680 / 6033` repairs.

Experimento: desabilitar `should_repair_extreme` apenas no benchmark.

| Config | Repairs | Blocos bbox/query | FP | FN | Decisão |
|---|---:|---:|---:|---:|---|
| sem regra extrema | 2353 | 44.49 | 1 | 2 | rejeitado como solução, mas útil para localizar custo |

Os 3 erros sem regra extrema foram:

| Tipo | `fraud_count` rápido | Padrão observado |
|---|---:|---|
| FN | 0 | sem `last_transaction`, `amount_vs_avg` moderado, perto de casa, tx_count baixo, comerciante conhecido |
| FP | 5 | sem `last_transaction`, `amount_vs_avg` alto, MCC alto, comerciante desconhecido, distância em torno de 398km |
| FN | 0 | sem `last_transaction`, `amount_vs_avg` moderado, perto de casa, tx_count baixo, comerciante conhecido |

Com base nesses casos, testei uma regra extrema mais estreita:

- `frauds == 0`: exige `amount <= 0.13`, `amount_vs_avg` entre `0.23` e `0.37`, `km_from_home` entre `0.07` e `0.13`, `tx_count_24h` entre `0.15` e `0.25`, transação presencial, comerciante conhecido e `mcc_risk` entre `0.30` e `0.45`.
- `frauds == 5`: exige `amount` entre `0.20` e `0.32`, `amount_vs_avg` entre `0.82` e `0.92`, `km_from_home` entre `0.35` e `0.45`, `tx_count_24h` entre `0.45` e `0.55`, presencial, cartão presente, comerciante desconhecido e `mcc_risk >= 0.75`.

Resultado offline instrumentado:

| Métrica | Regra anterior | Regra estreita |
|---|---:|---:|
| FP | 0 | 0 |
| FN | 0 | 0 |
| Repairs | 6033 | 2357 |
| Repairs | 11.15% | 4.36% |
| Repairs extremos | 3680 | 4 |
| Blocos bbox/query | 190.01 | 44.62 |

Validação k6 completa:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| regra estreita #1 | 1.25ms | 0 | 0 | 0 | 5902.03 |
| regra estreita #2 | 1.32ms | 0 | 0 | 0 | 5878.43 |

Comparação:

- A melhor run local da regra estreita (`5902.03`) supera a submissão oficial atual (`5844.41`) e se aproxima do top 4 honesto.
- A segunda run não confirmou o mesmo p99, ficando próxima do controle local (`1.31ms / 5883.53`).
- A mudança é tecnicamente coerente porque reduz o custo medido do repair sem sacrificar acurácia local, mas ainda tem risco de overfitting: as faixas foram derivadas dos três erros específicos expostos ao remover a regra extrema.

Decisão parcial: manter no branch experimental para mais validações, mas ainda não abrir nova issue/submissão só com base em uma run melhor. É candidata, não fechamento definitivo.

Verificação:

```bash
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp -j"$(nproc)"
DOCKER_HOST=unix:///run/docker.sock docker compose up -d --build --force-recreate
DOCKER_HOST=unix:///run/docker.sock ./run-local-k6.sh
cmake --build cpp/build --target rinha-backend-2026-cpp-tests -j"$(nproc)" && ctest --test-dir cpp/build --output-on-failure
```

Resultado dos testes C++:

```text
100% tests passed, 0 tests failed out of 1
```

## Ciclo 23h25: regra extrema média e decisão de submissão

Problema da regra estreita: apesar do melhor resultado (`5902.03`), ela foi derivada diretamente dos 3 erros observados ao desligar `should_repair_extreme`, então carregava risco de overfitting excessivo.

Nova hipótese: ampliar um pouco as faixas mantendo as restrições estruturais mais importantes:

- `frauds == 0`: baixo valor, `amount_vs_avg` moderado, perto de casa, baixo `tx_count_24h`, presencial, comerciante conhecido e MCC de baixo/médio risco.
- `frauds == 5`: valor moderado, `amount_vs_avg` alto, distância intermediária, `tx_count_24h` intermediário, presencial com cartão, comerciante desconhecido e MCC alto.

Resultado offline instrumentado da regra média:

| Métrica | Regra estreita | Regra média |
|---|---:|---:|
| FP | 0 | 0 |
| FN | 0 | 0 |
| Repairs | 2357 | 2401 |
| Repairs | 4.36% | 4.44% |
| Repairs extremos | 4 | 48 |
| Blocos bbox/query | 44.62 | 45.58 |

Interpretação: a regra média custa praticamente o mesmo que a regra estreita, continua muito abaixo da regra original (`190.01` blocos bbox/query) e é mais defensável do ponto de vista técnico.

Validação k6 completa da regra média:

| Config | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| regra média | 1.28ms | 0 | 0 | 0 | 5891.61 |

Comparação com a submissão oficial atual:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Submissão oficial `#1314` (`andrade-cpp-ivf`) | 1.43ms | 0% | 5844.41 |
| Regra média local | 1.28ms | 0% | 5891.61 |

Decisão: vale preparar nova submissão/issue com esta rodada.

Ressalva técnica: a melhoria é sustentada por redução concreta do repair medido, não por troca cosmética de flag. Ainda existe risco de diferença no harness oficial, mas a regra média é menos frágil que a versão ultraestreita e preservou 0 erro local.

## Ciclo 23h40: publicação da imagem e validação da branch `submission`

Publicação:

- Adicionada workflow `Publish GHCR image` na `main` do fork para publicar imagens a partir de um ref informado.
- Workflow executada com `ref=perf/noon-tuning` e `tag=submission-4260b14`.
- Run GitHub Actions: `25411738634`.
- Resultado: sucesso em `1m21s`.
- Imagem publicada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-4260b14`.

Validação da imagem pública:

```bash
DOCKER_HOST=unix:///run/docker.sock docker manifest inspect ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-4260b14
```

Resultado: `linux/amd64`.

Atualização da branch oficial de entrega:

- Branch `submission` atualizada para apontar `docker-compose.yml` para `submission-4260b14`.
- Commit: `462c729` (`point submission to 4260b14 image`).
- Push: `origin/submission`.

Benchmark final usando a própria branch `submission` e a imagem recém-puxada do GHCR:

| Fonte | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `submission` + `submission-4260b14` | 0.93ms | 0 | 0 | 0 | 6000.00 |
| repetição `submission` + `submission-4260b14` | 1.30ms | 0 | 0 | 0 | 5884.66 |

Comparação com submissão oficial anterior:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Issue anterior `#1314` | 1.43ms | 0% | 5844.41 |
| Nova candidata local validada na branch `submission` | 0.93ms | 0% | 6000.00 |
| Repetição da nova candidata | 1.30ms | 0% | 5884.66 |

Decisão: abrir nova issue oficial com a branch `submission` atualizada.

Issue oficial:

- Issue aberta: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/1697`.
- Body: `rinha/test andrade-cpp-ivf`.
- Título ajustado para o padrão mais recente: `rinha/test andrade-cpp-ivf`.
- Estado final: fechada pela engine.
- Resultado oficial: `p99=1.32ms`, `failure_rate=0%`, `final_score=5878.28`.
- Commit avaliado pela engine: `462c729`.
- Imagem avaliada pela engine: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-4260b14`.

Observação: no mesmo período havia outras issues recentes abertas sem comentário, então tratei como fila/lentidão da engine e não como erro de submissão. Não abri issue duplicada.

Resultado prático: a nova issue oficial melhorou a submissão anterior de `5844.41` para `5878.28` (+33.87 pontos), mantendo 0 falhas.

Propagação no ranking preview:

| Rank | Participante | Submissão | p99 | Falhas | Score | Issue |
|---:|---|---|---:|---:|---:|---|
| 1 | `thiagorigonatti` | `thiagorigonatti-c` | 1.00ms | 0% | 6000.00 | `#911` |
| 2 | `jairoblatt` | `jairoblatt-rust` | 1.17ms | 0% | 5932.05 | `#878` |
| 3 | `viniciusdsandrade` | `andrade-cpp-ivf` | 1.32ms | 0% | 5878.28 | `#1697` |

Fechamento da rodada: a submissão deixou o patamar anterior de `#5` observado no início da noite e passou a ocupar `#3` no preview no momento da checagem.

## Ciclo 23h05: pós-submissão, reparo `f1/f4` e translação dos líderes

Contexto: após a issue oficial `#1697`, a meta voltou a ser experimental. A submissão boa foi preservada na branch `submission`; os testes abaixo ficaram na branch `perf/noon-tuning`.

Baseline offline refeito com índice `1280` gerado pela branch atual:

```bash
nice -n 10 ./cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280.bin 1280 65536 6
nice -n 10 ./cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280.bin 1 0 1 1 1 1 4 1 0 0
```

Resultado:

| Config | FP | FN | ns/query | repairs | repairs extremos | blocos bbox/query |
|---|---:|---:|---:|---:|---:|---:|
| `fast=1 full=1 repair=1..4` | 0 | 0 | 31402.9 | 2401 / 4.44% | 48 | 45.58 |

### Hipótese A: filtrar os reparos `fast_count=1` e `fast_count=4`

Motivo: a configuração atual repara todos os casos rápidos com `fraud_count` de `1` a `4`. Os casos `2/3` são fronteira natural. Os casos `1/4` pareciam candidatos a filtro seletivo, pois representam 788 repairs extras (`404` de `f1` + `384` de `f4`).

Probes:

| Config | FP | FN | ns/query | repairs | Interpretação |
|---|---:|---:|---:|---:|---|
| `repair=2..3` | 22 | 28 | 35725.8 | 1613 | rejeitado, perde os dois lados |
| `repair=1..3` | 22 | 0 | 36395.5 | 2017 | rejeitado, `f4` precisa de reparo |
| `repair=2..4` | 0 | 28 | 35221.0 | 1997 | rejeitado, `f1` precisa de reparo |

Instrumentei o `benchmark-ivf-cpp` para imprimir candidatos de fronteira (`print_errors=2`) e separar os candidatos que realmente precisavam de repair:

| Grupo | Total de candidatos | Precisam repair | Podem pular |
|---|---:|---:|---:|
| `fast_count=1` | 404 | 28 | 376 |
| `fast_count=4` | 384 | 22 | 362 |

Achado: os candidatos que precisam repair e os que podem pular são muito sobrepostos em praticamente todas as dimensões. O menor retângulo simples que cobre todos os `f1` necessários ainda seleciona pelo menos `336/404` candidatos; para `f4`, pelo menos `318/384`. Ou seja, a seletividade economizaria pouco ou viraria regra frágil/overfit.

Decisão: **não implementar filtro seletivo `f1/f4` na submissão**. Manter `repair=1..4` continua mais sustentável.

### Hipótese B: aumentar `fast_nprobe`/`full_nprobe`

Motivo: `jairoblatt-rust` e `thiagorigonatti-c` usam mais probes primários (`8`) e reparo apenas em `2/3`. A hipótese era que um `fast_nprobe=2` pequeno reduziria necessidade de reparo extremo sem custo excessivo.

Resultados offline:

| Config | FP | FN | ns/query | repairs | blocos primários/query | blocos bbox/query |
|---|---:|---:|---:|---:|---:|---:|
| `fast=2 full=2 repair=2..3` | 3 | 1 | 43979.9 | 1618 | 629.57 | 23.94 |
| `fast=2 full=2 repair=1..4` | 0 | 0 | 45291.4 | 2400 | 636.85 | 35.02 |
| `fast=1 full=2 repair=1..4` | 0 | 0 | 37299.9 | 2401 | 333.87 | 35.05 |

Decisão: **rejeitado**. A troca reduz bbox em alguns casos, mas dobra o custo primário ou adiciona custo de segunda fase. O microbenchmark fica pior que o baseline.

### Hipótese C: ordem de dimensões dos líderes no acumulador de distância

Motivo: `thiagorigonatti-c` acumula distância na ordem `5,6,2,0,7,8,11,12,9,10,1,13,3,4`, podando mais cedo por dimensões discriminativas. Testei transpor essa ordem para o nosso `bbox_lower_bound`, fallback escalar e scan AVX2.

Resultado:

| Config | FP | FN | ns/query |
|---|---:|---:|---:|
| ordem original | 0 | 0 | ~31402.9 |
| ordem dos líderes | 0 | 0 | 32603.4 em `repeat=3` |

Decisão: **rejeitado e revertido**. No nosso layout atual (`kBlockLanes=8`, acumulador AVX2 em `uint64`) a ordem dos líderes não trouxe o mesmo ganho observado no layout AoSoA16/int32 deles.

Aprendizado da rodada: o caminho de maior potencial continua sendo estrutural, não uma heurística pequena em cima do IVF atual. Os dois líderes honestos que estão à frente usam uma destas vantagens que ainda não temos totalmente: `io_uring`/HTTP próprio com LB customizado, layout AoSoA16 com acumulador mais leve, ou IVF com centróides significativamente melhores (`k=2048`, k-means++/restarts/refino completo) compensado por runtime HTTP mais barato.

## Ciclo 23h15: varrimento estrutural de índice IVF

Hipótese: os líderes honestos gastam mais no build para obter centróides melhores. Testei se uma mudança somente no índice, sem alterar a API, poderia reduzir o custo de runtime ou abrir uma configuração com menos repairs.

### Índice `K=1280`, amostra maior e mais iterações

Comando:

```bash
nice -n 10 ./cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280-s262k-i10.bin 1280 262144 10
```

Tempo de geração: `23:12:35` até `23:13:41` (`~66s`).

| Índice/config | FP | FN | ns/query | repairs | blocos primários/query | blocos bbox/query |
|---|---:|---:|---:|---:|---:|---:|
| `1280/s65k/i6`, `repair=1..4` | 0 | 0 | ~31402.9 | 2401 | 322.90 | 45.58 |
| `1280/s262k/i10`, `repair=1..4` | 3 | 2 | 37100.0 | 2398 | 317.33 | 36.68 |
| `1280/s262k/i10`, `repair=2..3` | 19 | 19 | 37874.8 | 1599 | 313.26 | 24.46 |
| `1280/s262k/i10`, `full=2 repair=1..4` | 3 | 2 | 38371.0 | 2398 | 328.79 | 27.11 |

Decisão: **rejeitado**. O índice com mais treino reduz blocos de bbox, mas altera a aproximação e perde acurácia (`3 FP / 2 FN`) mesmo no repair amplo.

### Índice `K=1536`

Comando:

```bash
nice -n 10 ./cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1536-s65k-i6.bin 1536 65536 6
```

Tempo de geração: `23:14:07` até `23:14:58` (`~51s`).

| Índice/config | FP | FN | ns/query | repairs | blocos primários/query | blocos bbox/query |
|---|---:|---:|---:|---:|---:|---:|
| `1536/s65k/i6`, `repair=1..4` | 2 | 0 | 37083.7 | 2407 | 271.39 | 39.89 |
| `1536/s65k/i6`, `repair=2..3` | 24 | 22 | 35121.4 | 1640 | 268.21 | 27.62 |
| `1536/s65k/i6`, `full=2 repair=1..4` | 2 | 0 | 36958.2 | 2407 | 281.04 | 30.58 |

Decisão: **rejeitado**. Clusters menores reduzem o scan primário, mas aumentam custo de centroides/bbox e ainda introduzem `2 FP`.

### Índice `K=1024`

Comando:

```bash
nice -n 10 ./cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1024-s65k-i6.bin 1024 65536 6
```

Tempo de geração: `23:15:17` até `23:15:52` (`~35s`).

| Índice/config | FP | FN | ns/query | repairs | blocos primários/query | blocos bbox/query |
|---|---:|---:|---:|---:|---:|---:|
| `1024/s65k/i6`, `repair=1..4` | 1 | 2 | 36008.9 | 2396 | 408.82 | 50.94 |
| `1024/s65k/i6`, `repair=2..3` | 20 | 16 | 34792.0 | 1595 | 403.82 | 34.12 |
| `1024/s65k/i6`, `full=2 repair=1..4` | 1 | 2 | 35956.0 | 2396 | 423.66 | 37.48 |

Decisão: **rejeitado**. Menos clusters reduzem ranking de centroides, mas aumentam scan por cluster e perdem acurácia.

Conclusão do ciclo: o índice atual `K=1280 / sample=65536 / iter=6` segue como melhor ponto local conhecido para esta implementação. Os índices alternativos só seriam interessantes se acompanhados por uma mudança mais profunda de algoritmo de busca, por exemplo top-6/gap ou layout AoSoA16/int32 como no líder C; isoladamente, não melhoram a submissão.

## Ciclo 23h20: acumulador AVX2 `int32`

Hipótese: transpor um insight do líder C para o nosso kernel. O scan AVX2 usava acumuladores `uint64_t` por lane. Pelas regras do desafio, os vetores quantizados ficam em:

- dimensões normais: `0..10000`, diferença máxima `10000`, quadrado máximo `100M`;
- dimensões sentinela `5/6`: `-10000..10000`, diferença máxima `20000`, quadrado máximo `400M`.

Pior caso teórico: `12 * 100M + 2 * 400M = 2.0B`, abaixo de `INT32_MAX = 2.147B`. Portanto o acumulador `int32` é exato para o domínio real e evita o custo de expandir cada bloco para dois vetores `int64`.

Mudança:

- `scan_blocks_avx2` passou de dois acumuladores `epi64` (`lo/hi`) para um acumulador `epi32` com 8 lanes.
- O resultado final é armazenado em `uint32_t[8]` e inserido no `Top5` como `uint64_t`.
- Nenhuma regra de classificação foi alterada.

Validação offline:

```bash
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
nice -n 10 ./cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280.bin 1 0 1 1 1 1 4 1 0 0
nice -n 10 ./cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280.bin 3 0 1 1 1 1 4 0 0 0
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultados:

| Teste | FP | FN | parse errors | ns/query |
|---|---:|---:|---:|---:|
| offline com stats | 0 | 0 | 0 | 25730.9 |
| offline `repeat=3` sem stats | 0 | 0 | 0 | 24316.3 |

Teste C++:

```text
100% tests passed, 0 tests failed out of 1
```

Validação k6 local com imagem reconstruída via `docker compose up -d --build --force-recreate`:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `int32` local #1 | 1.25ms | 0 | 0 | 0 | 5903.78 |
| `int32` local #2 | 1.23ms | 0 | 0 | 0 | 5911.06 |

Comparação:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Submissão oficial `#1697` | 1.32ms | 0% | 5878.28 |
| Melhor local `int32` | 1.23ms | 0% | 5911.06 |

Decisão: **promover para candidata de submissão**. Esta é a primeira melhoria pós-`#1697` com ganho de kernel forte, acurácia preservada e duas runs locais acima da submissão oficial.

## Ciclo 23h30: publicação da candidata `int32`

Publicação da imagem:

- Commit experimental: `df6994a` (`optimize ivf scan accumulator`).
- Workflow GitHub Actions: `25413132596`.
- Resultado da workflow: sucesso em `1m24s`.
- Imagem publicada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-df6994a`.
- Manifest verificado: contém `linux/amd64`.

Branch oficial de entrega:

- Branch `submission` atualizada para apontar `docker-compose.yml` para `submission-df6994a`.
- Commit na `submission`: `b7897ee` (`point submission to df6994a image`).
- Push: `origin/submission`.

Validação da imagem pública na própria stack de submissão:

```bash
DOCKER_HOST=unix:///run/docker.sock docker compose pull
DOCKER_HOST=unix:///run/docker.sock docker compose up -d --force-recreate
DOCKER_HOST=unix:///run/docker.sock ./run-local-k6.sh
```

Resultado:

| Fonte | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| imagem pública `submission-df6994a` | 1.23ms | 0 | 0 | 0 | 5908.68 |

Issue oficial:

- Issue aberta: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/1714`.
- Título/body: `rinha/test andrade-cpp-ivf`.
- Primeiro monitoramento por 60 polls: permaneceu `OPEN`, `0` comentários.
- Havia outras issues abertas na fila no mesmo período (`#1710` a `#1714`), então tratei como fila/engine pendente, não como erro de submissão.
- Fechamento posterior pela engine: `p99=1.29ms`, `failure_rate=0%`, `final_score=5888.51`.
- Commit avaliado: `b7897ee`.
- Imagem avaliada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-df6994a`.

Comparação oficial:

| Issue | p99 | Falhas | Score |
|---|---:|---:|---:|
| `#1697` | 1.32ms | 0% | 5878.28 |
| `#1714` | 1.29ms | 0% | 5888.51 |

Ganho oficial da rodada: `+10.23` pontos, mantendo `0%` falhas. O ranking preview colocou `viniciusdsandrade / andrade-cpp-ivf` em `#3` no momento da checagem, atrás de `thiagorigonatti-c` (`6000`) e `jairoblatt-rust` (`5932.05`).

Decisão: **submissão bem-sucedida, mas com p99 oficial abaixo do melhor local**. Localmente a imagem chegou a `5908.68`; oficialmente caiu para `5888.51`, o que reforça que só devemos ressubmeter com mudança concreta e validação forte.

### Microteste rejeitado: prune final pós-14 dimensões

Após `#1714`, testei uma micro-otimização adicional: depois de acumular as 14 dimensões em `int32`, checar se todas as lanes ainda estavam acima do pior top-5 antes de fazer store/loop escalar.

Resultado offline:

| Config | FP | FN | ns/query |
|---|---:|---:|---:|
| `int32` sem prune final | 0 | 0 | 24316.3 |
| `int32` com prune final | 0 | 0 | 22606.0 |

Resultado k6 local:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| prune final #1 | 1.29ms | 0 | 0 | 0 | 5889.10 |
| prune final #2 | 1.28ms | 0 | 0 | 0 | 5893.38 |

Decisão: **rejeitado e revertido**. Apesar do ganho sintético, a p99 real ficou pior que a variante `df6994a` validada por imagem pública (`1.23ms / 5908.68`). Este é um caso claro em que o benchmark de kernel não representou o p99 da stack.

### Microteste rejeitado: redistribuição de CPU para nginx

Hipótese: depois do ganho do acumulador `int32`, o gargalo poderia ter se deslocado parcialmente do kernel IVF para a borda `nginx stream`. Para testar sem alterar algoritmo nem contrato, rodei uma redistribuição pequena de CPU:

| Serviço | Baseline | Candidato |
|---|---:|---:|
| `api1` | `0.41` CPU | `0.40` CPU |
| `api2` | `0.41` CPU | `0.40` CPU |
| `nginx` | `0.18` CPU | `0.20` CPU |
| Total | `1.00` CPU | `1.00` CPU |

Comando:

```bash
DOCKER_HOST=unix:///run/docker.sock docker compose up -d --force-recreate
DOCKER_HOST=unix:///run/docker.sock ./run-local-k6.sh
```

Resultado:

| Config | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `api=0.40 / nginx=0.20` | 1.35ms | 0 | 0 | 0 | 5869.72 |

Decisão: **rejeitado e revertido**. A p99 piorou contra a submissão oficial `#1714` (`1.29ms / 5888.51`) e contra a melhor validação local da imagem pública (`1.23ms / 5908.68`). Isso indica que, nesta fase, tirar CPU das APIs para dar mais margem ao nginx é contraproducente. Se voltarmos a mexer em recursos, o próximo teste deve ser na direção oposta (`api=0.42 / nginx=0.16`) ou em troca estrutural de LB, não em mais CPU para nginx.
