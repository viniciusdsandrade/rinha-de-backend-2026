# Daily Report 2026-05-01

Branch: `submission-2`.

Objetivo da rodada: continuar experimentos de melhoria bruta de performance no stack C++ aceito (`uWebSockets + simdjson + AVX2/FMA + nginx stream`), mantendo a regra de promover somente mudanças sustentáveis no k6.

## Baseline de referência

Estado aceito herdado da rodada anterior:

- `api=0.44` por instância e `nginx=0.12`.
- `INTERPROCEDURAL_OPTIMIZATION` apenas no executável C++.
- `res->cork` no response path.
- `benchmark-kernel-cpp` e `benchmark-request-cpp` disponíveis para triagem offline.
- Último k6 aceito registrado: `p99=2.88ms`, `final_score=5540.61`, `0 FP`, `0 FN`, `0 HTTP errors`.

## Experimento 1: sweep de agrupamentos

Hipótese: reduzir trabalho médio por query via agrupamento mais agressivo poderia gerar ganho maior que micro-otimizações de parser/LB.

O sweep completo em `14.500` queries foi interrompido porque ficou caro demais sem saída parcial por mais de dois minutos. Para triagem, foi usada amostra limitada:

```text
benchmark-classifier-cpp resources/references.json.gz test/test-data.json 1 1000 sweep
```

Resultado principal:

| Estratégia | Grupos | Divergências | ns/query | Linhas/query | Grupos/query |
|---|---:|---:|---:|---:|---:|
| `base group_local` | 117 | 0 | 80329.6 | 13469.8 | 2.812 |
| `amount4 group_local` | 250 | 0 | 69129.6 | 11526.5 | 4.214 |
| `amount8 group_local` | 407 | 0 | 76836.0 | 10954.5 | 5.710 |
| `hour global` | 1470 | 0 | 154750.0 | 5621.95 | 11.010 |
| `amount4_hour_day group_local` | 7362 | 0 | 559127.0 | 699.397 | 19.651 |

Leitura: `amount4` pareceu promissor no benchmark escalar, com menos linhas e zero divergência, mas precisava ser validado no kernel AVX2 real.

## Experimento 2: `amount4` na chave de agrupamento real

Mudança temporária: incluir `transaction.amount` em 4 buckets na `group_key` de produção.

Validações:

- Build de `benchmark-kernel-cpp` e testes C++: OK.
- `ctest --test-dir cpp/build --output-on-failure`: `1/1` passou.
- Sem divergência no benchmark de kernel.

Benchmark AVX2:

```text
queries=14500 repeat=5 refs=100000 groups=250 expected_rows_per_query=11973.1 expected_groups_per_query=4.10345
variant=baseline_production fraud_count_mismatches=0 decision_errors=0 ns_per_query=34722.4 rows_per_query=11973.1 groups_per_query=4.10345
variant=baseline_select_min fraud_count_mismatches=0 decision_errors=0 ns_per_query=28378.8 rows_per_query=11973.1 groups_per_query=4.10345
```

Decisão: rejeitado e revertido. Apesar de reduzir linhas, o aumento de grupos piorou o kernel AVX2 real contra a base histórica (`~28.8us/query`).

## Experimento 3: budget aproximado de grupos

Hipótese: limitar o número máximo de grupos visitados poderia reduzir p99 o suficiente para compensar alguns erros de decisão.

Comando:

```text
benchmark-classifier-cpp resources/references.json.gz test/test-data.json 1 0 budget
```

Resultado:

| Budget | Erros de decisão | ns/query | Linhas/query | Grupos/query |
|---:|---:|---:|---:|---:|
| 1 | 176 | 35810.1 | 5160.13 | 1.000 |
| 2 | 136 | 60279.9 | 10288.4 | 1.987 |
| 3 | 80 | 80166.7 | 13694.2 | 2.680 |
| 5 | 19 | 77979.1 | 13788.6 | 2.756 |
| 8 | 0 | 78985.2 | 13833.5 | 2.804 |
| 10 | 0 | 79940.2 | 13836.9 | 2.807 |

Decisão: rejeitado. Os budgets rápidos geram erros demais para compensar no scoring; o primeiro budget sem erro de decisão (`8`) praticamente não reduz trabalho.

## Experimento 4: remover `mcc_risk` da chave de agrupamento

Hipótese: reduzir grupos de `117` para `12` poderia diminuir overhead de lower-bound/sort mesmo aumentando linhas escaneadas.

Benchmark AVX2:

```text
queries=14500 repeat=5 refs=100000 groups=12 expected_rows_per_query=19740.6 expected_groups_per_query=1
variant=baseline_production fraud_count_mismatches=0 decision_errors=0 ns_per_query=39565.7 rows_per_query=19740.6 groups_per_query=1
variant=baseline_select_min fraud_count_mismatches=0 decision_errors=0 ns_per_query=33252.5 rows_per_query=19740.6 groups_per_query=1
```

Decisão: rejeitado e revertido. Menos grupos não compensa o aumento de linhas; AVX2 continua dominado por scan.

## Experimento 5: ordenar linhas dentro de cada grupo

Hipótese: ordenar linhas de cada grupo pela dimensão de maior variância local poderia agrupar chunks AVX2 mais homogêneos, melhorar early-prune por lane e preencher o top-5 mais cedo.

Resultado offline:

```text
queries=14500 repeat=5 refs=100000 groups=117 expected_rows_per_query=13836.9 expected_groups_per_query=2.8069
variant=baseline_production fraud_count_mismatches=0 decision_errors=0 ns_per_query=20266.2 rows_per_query=13836.9 groups_per_query=2.8069

queries=14500 repeat=10 refs=100000 groups=117 expected_rows_per_query=13836.9 expected_groups_per_query=2.8069
variant=baseline_production fraud_count_mismatches=0 decision_errors=0 ns_per_query=21329.2 rows_per_query=13836.9 groups_per_query=2.8069
```

Leitura offline: foi o melhor sinal da rodada, com ganho aparente de ~25% no kernel AVX2 isolado e zero divergência.

k6 após rebuild:

| Run | p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3.97ms | 5401.01 | 1.66ms | 2.21ms | 0 | 0 | 0 |
| 2 | 3.68ms | 5433.64 | 1.67ms | 2.15ms | 0 | 0 | 0 |
| 3 | 3.49ms | 5457.11 | 1.70ms | 2.20ms | 0 | 0 | 0 |

Decisão: rejeitado e revertido. O ganho offline não traduziu para k6; a mediana e o p99 pioraram em todas as amostras.

## Conclusão parcial

Nenhuma hipótese desta rodada superou o estado aceito. O aprendizado técnico mais importante é que o benchmark de kernel isolado pode ser enganoso quando muda a ordem física dos dados: a ordenação intra-grupo melhora o microkernel, mas piora o comportamento end-to-end no container.

Próximas hipóteses com melhor chance:

- Medir custo real de startup/cache após mudança de layout para entender divergência microkernel vs k6, antes de insistir em reorder.
- Criar benchmark end-to-end host-only que faça parse + vectorize + classify em corpo JSON real com a mesma ordem do k6.
- Investigar uma estrutura exata diferente, como árvore com lower-bound por blocos ou blocos fixos por dimensão, mas só se o benchmark end-to-end indicar que ainda há espaço fora de ruído.

## Validação pós-reversão

Depois dos experimentos rejeitados, o código de produção foi revertido para o estado aceito anterior. A checagem de diff nos arquivos críticos não mostrou mudanças em:

```text
cpp/src/refs.cpp
docker-compose.yml
nginx.conf
cpp/src/request.cpp
cpp/src/main.cpp
```

Validações executadas:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-kernel-cpp benchmark-classifier-cpp -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --force-recreate --pull never
curl http://localhost:9999/ready
```

Resultado:

- Build C++: OK.
- `ctest`: `1/1` passou.
- `/ready`: `204`.
- `git diff --check`: OK.

k6 pós-reversão:

| Run | p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3.62ms | 5441.57 | 1.75ms | 2.27ms | 0 | 0 | 0 |
| 2 | 3.61ms | 5442.99 | 1.74ms | 2.24ms | 0 | 0 | 0 |

Leitura: o score ficou abaixo do melhor estado aceito histórico (`p99=2.88ms`, `final_score=5540.61`), mas o diff de produção está limpo. A hipótese mais provável é ruído de ambiente local: `docker stats` mostrou containers externos ativos (`payment-processor-*` e `ecv-document-portal-mailhog-1`) além da submissão C++. Portanto, essas duas medições não devem ser tratadas como regressão de código sem isolar a máquina.

## Experimento 6: seleção iterativa de grupos no classificador

Hipótese: substituir `std::sort` dos `117` lower-bounds de grupo por seleção iterativa do menor grupo visitável reduziria overhead no hot path, porque o classificador visita em média apenas `~2.8` grupos por query. Na mesma rodada também foi hoistado `std::isfinite(threshold)` para uma vez por chunk AVX2.

Validação local:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-request-cpp -j2
ctest --test-dir cpp/build --output-on-failure
benchmark-request-cpp test/test-data.json resources/references.json.gz 10 0
```

Resultado:

- `ctest`: `1/1` passou.
- Baseline pareado do `benchmark-request-cpp`: `parse_classify_ns_per_query=29363.1`.
- Variação: `parse_classify_ns_per_query=26706.4`.
- Checksum igual: `577480`.

k6 no compose oficial local:

| p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|
| 4.26ms | 5370.09 | 1.75ms | 2.32ms | 0 | 0 | 0 |

Decisão: rejeitado e revertido. A melhora no benchmark host-only não traduziu para o k6; o p99 piorou claramente contra o pós-reversão local (`3.61-3.62ms`) e contra o melhor estado aceito histórico (`2.88ms`). Aprendizado: até mesmo `parse_classify` host-only ainda é gate insuficiente para mudanças de ordem/travessia de grupos; k6 segue como critério decisivo.

## Incidente de ambiente: Docker Desktop parado

Durante o experimento seguinte, o Docker falhou antes de executar k6:

```text
failed to connect to the docker API at unix:///home/andrade/.docker/desktop/docker.sock
docker-desktop.service: inactive (dead)
```

A causa imediata foi o serviço `docker-desktop.service` parado. O serviço foi reiniciado com:

```text
systemctl --user start docker-desktop
```

Validação após restart:

- Docker Desktop Server: `4.70.0`.
- `docker version`: OK.
- `docker stats`: apenas `ecv-document-portal-mailhog-1` externo permaneceu ativo; os containers `payment-processor-*` não estavam mais rodando.

Leitura: a qualidade dos benchmarks k6 posteriores tende a ser melhor que a rodada contaminada anterior, mas qualquer comparação com runs pré-restart precisa considerar essa quebra de ambiente.

## Experimento 7: mais CPU para APIs, menos CPU para nginx

Hipótese: se o gargalo principal fosse classificação nas APIs, mover CPU do nginx para as duas APIs poderia melhorar p99:

```text
api1/api2: 0.47 CPU cada
nginx:     0.06 CPU
total:     1.00 CPU
```

k6:

| p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|
| 63.58ms | 4196.66 | 1.79ms | 25.94ms | 0 | 0 | 0 |

Decisão: rejeitado e revertido. O p90/p99 explodiu sem erros HTTP nem erros de detecção, o que aponta para throttling do nginx/LB. O split aceito `0.44/0.44/0.12` continua tecnicamente justificado.

## Experimento 8: mais CPU para nginx

Hipótese: como reduzir nginx para `0.06` explodiu p99, talvez o LB ainda estivesse levemente pressionado no split aceito. Foi testado:

```text
api1/api2: 0.43 CPU cada
nginx:     0.14 CPU
total:     1.00 CPU
```

Primeiro k6:

| p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|
| 3.36ms | 5473.66 | 1.75ms | 2.21ms | 0 | 0 | 0 |

Baseline aceito medido no mesmo ambiente pós-restart:

| p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|
| 3.46ms | 5461.39 | 1.71ms | 2.19ms | 0 | 0 | 0 |

Uma repetição do split `0.43/0.43/0.14` foi inválida:

| p99 | final_score | HTTP errors | failure_rate |
|---:|---:|---:|---:|
| 0.00ms | -3000.00 | 14343 | 99.94% |

Investigação: após a falha, `docker-desktop.service` tinha reiniciado novamente e containers externos `payment-processor-*` voltaram a subir. A variação foi revertida por falta de reprodutibilidade. O sinal de `+12.27` pontos no primeiro run é pequeno demais para aceitar sob instabilidade do Docker.

Ação de ambiente: os containers externos `payment-processor-default`, `payment-processor-fallback` e respectivos bancos foram parados para reduzir ruído. O `docker stats` depois disso mostrou apenas a submissão 2026 e `ecv-document-portal-mailhog-1`.

## Baseline aceito limpo após parar containers externos

Com `docker-compose.yml` restaurado para `api=0.44+0.44` e `nginx=0.12`, e com os containers `payment-processor-*` parados, foi medido um novo baseline local:

| p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|
| 3.33ms | 5477.67 | 1.75ms | 2.24ms | 0 | 0 | 0 |

Leitura: o ambiente limpo melhora o resultado frente aos runs contaminados (`3.61-3.62ms`), mas ainda não reproduz o melhor histórico (`2.88ms`). A partir daqui, qualquer hipótese precisa bater pelo menos esse baseline local limpo.

## Experimento 9: `worker_processes 2` no nginx stream

Hipótese: dois workers no nginx poderiam reduzir fila no LB durante o ramp de `650 RPS`.

k6:

| p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|
| 3.45ms | 5461.71 | 1.71ms | 2.18ms | 0 | 0 | 0 |

Decisão: rejeitado e revertido. Apesar de mediana/p90 similares, o p99 piorou contra o baseline limpo (`3.33ms`). Com `0.12 CPU` no LB, múltiplos workers parecem introduzir overhead/competição sem ganho.

## Experimento 10: nginx `http` proxy em vez de `stream`

Hipótese: trocar o LB L4 (`stream`) por proxy HTTP com upstream em unix socket poderia melhorar distribuição por request caso houvesse desbalanceamento entre conexões persistentes do k6 e as duas APIs.

Configuração temporária:

```text
nginx http {}
upstream api com unix:/sockets/api1.sock e unix:/sockets/api2.sock
proxy_http_version 1.1
keepalive 128
```

k6:

| p99 | final_score | med | p90 | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|---:|
| 45.80ms | 4339.16 | 1.79ms | 7.23ms | 0 | 0 | 0 |

Decisão: rejeitado e revertido. A camada HTTP no nginx adicionou overhead relevante e piorou a cauda. O `stream` L4 continua sendo a escolha correta para esta submissão.

## Releitura do upstream oficial em 2026-05-01

Foi feito `git fetch` do repositório oficial `zanfranceschi/rinha-de-backend-2026`, branch `main`, no commit `d501ddc1e941b24014c3ce5a6b41ccc3054ec1a0`.

Mudanças materiais encontradas:

- `docs/br/SUBMISSAO.md`: a branch `submission` agora é descrita como branch contendo apenas os arquivos necessários para executar o teste; o texto explicita que o código-fonte não pode estar nessa branch.
- `docs/br/DATASET.md`: `resources/references.json.gz` agora é documentado como ~16MB gzipado / ~284MB descompactado com `3.000.000` vetores, não mais 100.000 vetores.
- `test/test.js`: cenário local mudou para `120s` em `900 RPS`, `preAllocatedVUs=100`, `maxVUs=250`, timeout HTTP `2001ms`, e removeu o bloco antigo de contract check no `setup`.
- `test/test-data.json`: massa local mudou para `54.100` requisições e estrutura `expected_approved`/`expected_fraud_score`.
- `config.json`: passou a declarar `post_test_script`, `poll_interval_ms=30000`, `submission_health_check_retries=20` e `max_cpu` numérico.
- `.github/pull_request_template.md`: checklist atualizado mantém os itens essenciais: 1 CPU/350MB, porta 9999, linux/amd64, bridge, sem host/privileged, pelo menos 1 LB + 2 APIs, branches `main` e `submission`, `docker-compose.yml` e `info.json` na raiz da branch `submission`.

Regras que permanecem iguais para a implementação:

- Exatamente dois endpoints de sucesso: `GET /ready` e `POST /fraud-score`.
- Porta externa `9999` no load balancer.
- Pelo menos 1 load balancer e 2 APIs.
- Load balancer sem lógica de negócio.
- `k=5`, distância euclidiana como referência de rotulagem, `fraud_score = fraudes/5`, `approved = fraud_score < 0.6`.
- Limite total de `1 CPU` e `350MB`.

## Impacto do dataset oficial novo

A imagem C++ atual foi testada contra o `test.js` e `test-data.json` atuais do upstream, mas ainda usando o dataset antigo embutido na imagem.

Amostra temporária com 5.000 primeiras entradas e ramp reduzido (`20s`, alvo `400 RPS`):

| p99 | final_score | failure_rate | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 3.34ms | 3179.48 | 2.15% | 44 | 42 | 0 |

Teste oficial local completo atualizado (`54.100` entradas, `120s`, `900 RPS`):

| p99 | final_score | failure_rate | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 16.62ms | 2117.12 | 2.27% | 642 | 587 | 0 |

Leitura: a solução continua estável e sem erro HTTP, mas a acurácia caiu porque o dataset embutido é o antigo. A queda de score vem mais de detecção (`detection_score=337.83`) do que de latência (`p99_score=1779.29`).

Também foi feito teste host-only carregando o `references.json.gz` oficial novo diretamente no classificador exato atual:

```text
benchmark-request-cpp /tmp/.../test-data.json /tmp/.../references.json.gz 1 100
parse_classify_ns_per_query=753386
maxrss_kb=2300988
```

Decisão: não promover troca simples para o dataset de 3M vetores. O desenho atual de KNN exato carregando referências em memória não cabe no orçamento de `2 x 165MB` das APIs e ficaria lento demais. Para a regra atual, o próximo salto real precisa ser índice/modelo para o dataset de 3M, não micro-otimização do stack atual.

## Comparação com ranking parcial informado

Ranking parcial recebido durante a rodada:

| Posição | Participante | p99 | Falhas | Score |
|---:|---|---:|---:|---:|
| 1 | `thiagorigonatti-c` | 1.25ms | 0% | 5901.92 |
| 2 | `jairoblatt-rust` | 1.45ms | 0% | 5838.50 |
| 3 | `joojf` | 1.50ms | 0% | 5823.94 |
| 4 | `murilochianfa-cpp-fraud-detection-rinha-2026` | 2.84ms | 0% | 5546.41 |
| 5 | `hisrael-xgboost-go` | 2.60ms | 0% | 5404.29 |
| 6 | `dotnet` | 3.86ms | 0% | 5323.56 |
| 7 | `lothyriel-rust` | 39.27ms | 0% | 4225.36 |
| 8 | `vitortvale-rust` | 39.45ms | 0% | 4170.45 |
| 9 | `davidalecrim1-rust-extreme` | 240.74ms | 1.39% | 1214.12 |

Comparação:

- Melhor histórico local antigo da nossa C++: `p99=2.88ms`, `0%`, `final_score=5540.61`. Se fosse comparável, ficaria logo abaixo do 4º colocado (`5546.41`) e acima do 5º (`5404.29`).
- Baseline limpo antigo de hoje: `p99=3.33ms`, `0%`, `final_score=5477.67`. Se fosse comparável, ficaria entre 4º e 5º.
- Teste oficial local atualizado com dataset antigo embutido: `p99=16.62ms`, `2.27%`, `final_score=2117.12`. No ranking informado, ficaria entre o 8º e o 9º, mas a comparação ainda é imperfeita porque foi local e não executada pela engine oficial.

Conclusão: a stack C++/nginx atual estava competitiva no cenário antigo, mas a atualização para 3M referências deslocou o problema para estratégia de detecção/índice. A melhor submissão executável atual será preparada para conformidade, mas não deve ser tratada como candidata forte ao topo até resolver o dataset novo.

## Preparação da branch `submission`

A branch `submission` foi reduzida para a estrutura minimalista exigida pela documentação atualizada:

```text
docker-compose.yml
info.json
nginx.conf
```

Commit publicado:

```text
846f7ca prepare minimal submission
```

O `docker-compose.yml` aponta para:

```text
ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission
```

Validação local da branch `submission`:

- `docker compose up -d --force-recreate --pull never`: OK usando a imagem local já tagueada.
- `GET /ready`: `204`.
- `git ls-tree -r --name-only origin/submission`: contém apenas `docker-compose.yml`, `info.json`, `nginx.conf`.

Benchmark oficial local atualizado rodando exatamente a branch `submission` minimalista:

| p99 | final_score | failure_rate | FP | FN | HTTP |
|---:|---:|---:|---:|---:|---:|
| 20.12ms | 2034.28 | 2.27% | 642 | 587 | 0 |

Comparação com o ranking parcial informado: `2034.28` pontos ficaria entre o 8º colocado (`4170.45`) e o 9º (`1214.12`). O melhor run local atualizado anterior da mesma imagem (`2117.12`) também ficaria nesse intervalo. O gargalo competitivo atual é acurácia contra o dataset novo, não erro HTTP.

Bloqueio operacional: a imagem foi construída localmente como `linux/amd64`, mas o push para GHCR falhou:

```text
failed to push ghcr.io/viniciusdsandrade/rinha-de-backend-2026:cpp-submission-20260501:
denied: permission_denied: The token provided does not match expected scopes.
```

Diagnóstico: o token autenticado no `gh` possui `read:packages`, mas não `write:packages`. A tentativa de `gh auth refresh -s write:packages` entrou em fluxo interativo de browser e expirou. Portanto, a branch `submission` está preparada, mas a submissão ainda não deve ser enviada à engine oficial até a imagem pública ser publicada ou o compose apontar para outro registry público válido.

## Rodada IVF oficial para 3M referências

Objetivo: substituir o classificador exato em memória float, inviável para `3.000.000` referências, por um índice IVF quantizado em `int16` com busca AVX2 e repair exato por bounding boxes. A implementação ficou em branch isolada `perf/ivf-index` para evitar contaminar a branch `submission` até o ganho ser medido.

### Implementação adicionada

- `cpp/include/rinha/ivf.hpp` e `cpp/src/ivf.cpp`: índice IVF binário com vetores quantizados `int16`, blocos de 8 lanes, labels, ids originais para desempate, centróides, offsets e bounding boxes por cluster.
- `cpp/tools/prepare_ivf.cpp`: gera `index.bin` a partir de `references.json.gz`.
- `cpp/tools/benchmark_ivf.cpp`: benchmark offline contra `test-data.json` oficial novo, medindo divergências, checksum e ns/query.
- `cpp/src/main.cpp`: `IVF_INDEX_PATH` ativa o classificador IVF; sem essa variável, mantém fallback para o classificador antigo.
- `Dockerfile`: gera `index.bin` no build a partir do dataset oficial fixado no commit upstream `d501ddc1e941b24014c3ce5a6b41ccc3054ec1a0`.

Validações iniciais:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-ivf-cpp prepare-ivf-cpp -j2
ctest --test-dir cpp/build --output-on-failure
git diff --check
```

Resultado:

- Build C++: OK.
- `ctest`: `1/1` passou.
- `git diff --check`: OK.

### Triagem offline de índices

Dataset oficial atual:

```text
references.json.gz: 48MB gzipado
test-data.json: 54.100 entradas
```

Builds testados:

| Índice | Build | Memória do índice | Observação |
|---|---:|---:|---|
| IVF256 | 5.53s | 94.47MB | exato com repair, mas mais lento |
| IVF512 | 8.55s | 94.53MB | exato com repair, melhor que 256 |
| IVF1024 | 14.17s | 94.64MB | exato com repair, melhor que 512 |
| IVF2048 | 26.32s | 94.87MB | melhor ponto exato offline |
| IVF4096 | 49.96s | 95.32MB | piorou; mais centróides não compensaram |

Benchmark offline completo (`54.100` entradas):

| Configuração | FP | FN | failure_rate | ns/query | Decisão |
|---|---:|---:|---:|---:|---|
| IVF256 sem repair | 92 | 89 | 0.335% | 37.997 | rejeitado por erros |
| IVF256 com repair | 0 | 0 | 0% | 161.488 | correto, mais lento |
| IVF512 com repair | 0 | 0 | 0% | 132.478 | correto |
| IVF1024 com repair | 0 | 0 | 0% | 108.778 | correto |
| IVF2048 com repair | 0 | 0 | 0% | 101.873 | melhor exato offline |
| IVF4096 com repair | 0 | 0 | 0% | 146.776 | rejeitado |

Também foi testado modo híbrido: busca aproximada sem repair e repair apenas para votos próximos da fronteira. Melhor híbrido offline:

| Configuração | FP | FN | failure_rate | ns/query |
|---|---:|---:|---:|---:|
| IVF2048, repair para votos `1..4` | 3 | 4 | 0.0129% | 17.856 |

Leitura: o híbrido é muito mais rápido, mas carrega penalidade de detecção. Como a fórmula dá +3000 para detecção perfeita, era necessário validar no k6 se a queda de p99 compensaria os 7 erros.

### Benchmarks oficiais locais em container

O benchmark foi executado com o `test.js` atual do upstream (`120s`, alvo `900 RPS`, `preAllocatedVUs=100`, `maxVUs=250`, timeout `2001ms`) em diretório temporário:

```text
/tmp/rinha-2026-official-run/test.js
/tmp/rinha-2026-official-data/test-data.json
```

Resultados principais:

| Stack/config | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 2 APIs, nginx `0.12`, IVF2048 exato | 32.06ms | 0 | 0 | 0 | 4494.04 | correto, LB subdimensionado |
| 2 APIs, nginx `0.12`, híbrido `1..4` | 13.45ms | 3 | 4 | 0 | 4510.05 | melhora marginal, ainda ruim |
| 2 APIs, nginx `0.12`, sem repair | 12.23ms | 143 | 147 | 0 | 3048.84 | rejeitado por detecção |
| HAProxy HTTP, sem repair | 676.90ms | 141 | 142 | 0 | 1314.89 | rejeitado |
| HAProxy TCP, sem repair | 216.54ms | 143 | 147 | 0 | 1800.62 | rejeitado |
| 2 APIs, nginx `0.20`, sem repair | 2.72ms | 143 | 147 | 0 | 3701.53 | provou gargalo de LB |
| 2 APIs, nginx `0.20`, híbrido `1..4` | 2.70ms | 3 | 4 | 0 | 5206.77 | bom, mas perde para exato |
| 2 APIs, nginx `0.20`, exato | 3.29ms | 0 | 0 | 0 | 5482.76 | melhor 2 APIs nesse split |
| 2 APIs, nginx `0.18`, exato | 3.25ms | 0 | 0 | 0 | 5487.92 | melhor 2 APIs |
| 2 APIs, nginx `0.16`, exato | 3.44ms | 0 | 0 | 0 | 5463.58 | rejeitado |
| 3 APIs, nginx `0.19`, exato | 3.24ms | 0 | 0 | 0 | 5488.99 | melhor run local antes da branch final |
| 3 APIs, nginx `0.22`, exato | 3.27ms | 0 | 0 | 0 | 5484.79 | rejeitado |
| 3 APIs, nginx `0.19`, exato, repetição | 3.33ms | 0 | 0 | 0 | 5477.45 | confirma faixa, mas mostra ruído |
| Branch `submission` final, imagem pública GHCR | 3.24ms | 0 | 0 | 0 | 5489.47 | melhor run da rodada |

Melhor run obtida na rodada, já usando a branch `submission` minimalista e a imagem pública `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission` puxada do GHCR:

```json
{
  "p99": "3.24ms",
  "scoring": {
    "breakdown": {
      "false_positive_detections": 0,
      "false_negative_detections": 0,
      "true_positive_detections": 24037,
      "true_negative_detections": 30022,
      "http_errors": 0
    },
    "failure_rate": "0%",
    "weighted_errors_E": 0,
    "p99_score": { "value": 2489.47, "cut_triggered": false },
    "detection_score": {
      "value": 3000,
      "rate_component": 3000,
      "absolute_penalty": 0,
      "cut_triggered": false
    },
    "final_score": 5489.47
  }
}
```

Comparação com o ranking parcial informado:

- Melhor run local nova (`5489.47`, `p99=3.24ms`, `0%`) ficaria entre o 4º colocado (`5546.41`) e o 5º (`5404.29`).
- Para alcançar o 4º colocado mantendo `0%` falhas, o p99 precisa cair de `~3.24ms` para perto de `2.84ms`.
- O salto contra a submissão minimalista anterior é material: de `2034.28` para `5489.47` na melhor run local, ganho de `+3455.19` pontos.

### Decisão técnica

Candidato final desta rodada:

```text
3 APIs + nginx stream
api1/api2/api3: 0.27 CPU / 110MB cada
nginx: 0.19 CPU / 20MB
IVF2048 exato com bbox repair em todos os votos (`repair_min=0`, `repair_max=5`)
```

Justificativa:

- Mantém `0 FP`, `0 FN`, `0 HTTP` no dataset oficial local.
- Cabe no limite declarado: `1.00 CPU` e `350MB`.
- Memória observada em idle: ~`96MB` por API dentro do limite de `110MB`.
- Melhor score local observado: `5489.47`.

Risco residual:

- A repetição do melhor candidato caiu para `5477.45`; a diferença parece ruído local de p99, não regressão funcional.
- A configuração 3 APIs é mais apertada em memória que 2 APIs. Se o ambiente oficial contabilizar memória de forma mais severa, o plano B seguro é 2 APIs com `api=0.41`, `nginx=0.18`, score local observado `5487.92`.

## Rodada pós-submissão em branch experimental `perf/noon-tuning`

Após a abertura e merge do PR oficial de participante (`zanfranceschi/rinha-de-backend-2026#593`), a investigação continuou fora da branch `submission`, em uma worktree isolada baseada em `origin/perf/ivf-index`.

Objetivo desta etapa:

- Manter `submission` intacta.
- Buscar ganho concreto e sustentável de score local.
- Registrar tanto hipóteses rejeitadas quanto hipóteses aceitas.
- Usar o mesmo benchmark oficial local atualizado (`54.100` entradas, alvo `900 RPS`, timeout `2001ms`).

### Baseline congelado da branch experimental

Configuração inicial:

```text
3 APIs + nginx stream
api1/api2/api3: 0.27 CPU / 110MB cada
nginx: 0.19 CPU / 20MB
IVF2048 com fast_nprobe=1, full_nprobe=2, boundary_full=true, bbox_repair=true, repair=0..5
```

Validações antes dos experimentos:

```text
cmake --build ...: OK
ctest --test-dir cpp/build --output-on-failure: 1/1 passed
docker compose config -q: OK
GET /ready: 204
memória idle: ~95.8MiB por API / 110MB
```

Baseline k6 desta worktree:

| Configuração | p99 | FP | FN | HTTP | Score |
|---|---:|---:|---:|---:|---:|
| Baseline `perf/noon-tuning` | 3.31ms | 0 | 0 | 0 | 5480.16 |

Leitura: reproduziu a mesma faixa da melhor submissão (`~3.24ms` a `~3.33ms`), com ruído relevante de p99 entre execuções.

### Experimentos de nginx/LB

| Hipótese | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `worker_processes=2`, mantendo `reuseport` | 3.28ms | 0 | 0 | 0 | 5484.77 | inconclusivo, ganho pequeno |
| Repetição `2 workers + reuseport` | 3.28ms | 0 | 0 | 0 | 5484.54 | confirma faixa, mas não supera melhor histórico |
| `worker_processes=2`, sem `reuseport` | 3.27ms | 0 | 0 | 0 | 5485.36 | promissor inicialmente |
| `worker_processes=1`, sem `reuseport` | 3.29ms | 0 | 0 | 0 | 5482.37 | rejeitado |
| `worker_processes=2`, sem `reuseport`, `multi_accept off` | 3.30ms | 0 | 0 | 0 | 5481.01 | rejeitado |
| APIs `0.28` CPU, nginx `0.16` CPU | 3.40ms | 0 | 0 | 0 | 5468.25 | rejeitado |
| APIs `0.26` CPU, nginx `0.22` CPU | 3.28ms | 0 | 0 | 0 | 5483.63 | rejeitado |
| Rebuild + confirmação de `2 workers` sem `reuseport` | 3.34ms | 0 | 0 | 0 | 5476.37 | rejeitado por não reproduzir |
| Configuração original no mesmo estado pós-rebuild | 3.28ms | 0 | 0 | 0 | 5484.09 | mantida como referência |

Conclusão: nenhuma mudança de nginx/LB mostrou ganho sustentável. A melhor leitura é que a cauda local está dominada por ruído de agendamento e proxy, não por uma flag específica de nginx.

### Experimentos de parser/hot path HTTP

| Hipótese | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Parse direto do chunk quando request chega em chunk único | 3.73ms | 0 | 0 | 0 | 5428.00 | rejeitado |
| Repetição do parse direto do chunk | 3.32ms | 0 | 0 | 0 | 5478.58 | rejeitado; não bate baseline |
| `merchant.id` e `known_merchants` como `string_view` temporário | 3.46ms | 0 | 0 | 0 | 5460.36 | rejeitado |

Conclusão: micro-otimizações no parser não melhoraram a cauda do k6. O parser atual com cópia simples para `RequestContext::body` continua sendo a escolha mais estável.

### Experimentos de reparo IVF

| Hipótese | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Reparar apenas votos `0..4` (`MAX=4`) | 3.01ms | 3 | 0 | 0 | 5341.07 | rejeitado; 3 FP |
| Reparar apenas votos `1..5` (`MIN=1`) | 3.21ms | 0 | 4 | 0 | 5159.45 | rejeitado; 4 FN |

Conclusão: o reparo exato em todo o intervalo `0..5` é necessário para manter `0` erro no dataset oficial local. A fórmula de score pune mais os erros do que recompensa a queda marginal de p99.

### Experimento aceito: IVF single-pass equivalente

Achado técnico: a configuração anterior fazia:

```text
fast_nprobe=1
full_nprobe=2
boundary_full=true
repair_min=0
repair_max=5
```

Como qualquer resultado de `fraud_count` está sempre em `0..5`, o `boundary_full=true` com `repair=0..5` executava sempre a busca rápida e, em seguida, a busca completa. A primeira busca era redundante para o resultado final.

Nova configuração experimental:

```text
IVF_FAST_NPROBE=2
IVF_FULL_NPROBE=2
IVF_BOUNDARY_FULL=false
IVF_BBOX_REPAIR=true
IVF_REPAIR_MIN_FRAUDS=0
IVF_REPAIR_MAX_FRAUDS=5
```

Com isso a API executa diretamente a busca efetiva final (`nprobe=2` + `bbox_repair`) uma única vez.

Microbenchmark isolado do classificador:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Caminho anterior: `fast=1`, `full=2`, `boundary=true`, `repair=0..5` | 115.692 | 0 | 0 | 0 | baseline |
| Single-pass: `fast=2`, `full=2`, `boundary=false` | 103.368 | 0 | 0 | 0 | aceito |

Ganho isolado: cerca de `10.6%` menos tempo por query no classificador, sem mudar acurácia.

Validação k6 oficial local:

| Configuração | p99 | FP | FN | HTTP | Score |
|---|---:|---:|---:|---:|---:|
| Single-pass IVF, run 1 | 3.30ms | 0 | 0 | 0 | 5481.44 |
| Single-pass IVF, run 2 | 3.27ms | 0 | 0 | 0 | 5484.83 |

Decisão: manter em branch experimental porque é uma melhoria técnica real e preserva `0` erro. No k6, o ganho aparece como neutralidade/leve melhora dentro do ruído, não como salto decisivo de score. Ainda assim, remove trabalho redundante do hot path e aumenta margem de CPU.

### Estado final da branch experimental

Mudança mantida:

```text
docker-compose.yml
- adiciona IVF_FAST_NPROBE=2 nas 3 APIs
- muda IVF_BOUNDARY_FULL de true para false
- mantém IVF_FULL_NPROBE=2, IVF_BBOX_REPAIR=true e repair=0..5
```

Mudanças rejeitadas e revertidas:

- Alterações de nginx (`worker_processes`, `reuseport`, `multi_accept`).
- Redistribuição de CPU entre nginx e APIs.
- Otimizações de parser com chunk direto.
- Otimizações de parser usando `string_view` para merchant temporário.
- Redução parcial do intervalo de reparo IVF.

Próximas hipóteses com melhor relação risco/retorno:

- Criar benchmark local focado em cauda p95/p99 por etapa dentro da API para separar parse, vectorize, IVF e resposta HTTP.
- Testar uma versão do IVF que remova a passada rápida diretamente no código, em vez de depender apenas de ENV, para reduzir condicionais no hot path.
- Investigar uma estratégia de índice menor/mais cache-friendly mantendo `0` erro, mas só com validação offline completa antes do k6.
- Avaliar se uma submissão com o single-pass deve substituir a imagem pública atual depois de 3 runs k6 consecutivas mostrarem média igual ou melhor que a branch `submission` atual.

## Rodada pós-checkpoint `perf/noon-tuning` - 10h11

Contexto: após publicar o checkpoint `1aefc5d` em `origin/perf/noon-tuning`, continuei a investigação em branch não-submission. O objetivo desta rodada foi atacar o custo do repair exato do IVF sem aceitar aproximações que introduzam FP/FN.

### Screening offline de configurações IVF

Comando-base:

```bash
cpp/build/benchmark-ivf-cpp /tmp/rinha-2026-official-data/test-data.json /tmp/rinha-2026-index.bin ...
```

Resultados relevantes antes de mexer no código:

| Configuração | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `nprobe=1`, bbox repair direto | 104.536 | 0 | 0 | correto, mas mais lento que `nprobe=2` no estado anterior |
| `nprobe=2`, bbox repair direto | 102.306 | 0 | 0 | melhor configuração exata pré-patch |
| `nprobe=3`, bbox repair direto | 108.438 | 0 | 0 | rejeitado |
| `nprobe=4`, bbox repair direto | 107.460 | 0 | 0 | rejeitado |
| `nprobe=1`, sem bbox repair | 12.757 | 429 | 444 | rejeitado por detecção |
| `nprobe=2`, sem bbox repair | 16.806 | 156 | 150 | rejeitado por detecção |
| Híbrido `fast=1`, `full=2`, repair `2..3` | 17.071 | 63 | 90 | rejeitado por detecção |
| Híbrido `fast=1`, `full=2`, repair `1..4` | 16.641 | 9 | 12 | rejeitado por detecção |
| Híbrido `fast=1`, `full=2`, repair `0..4` | 57.866 | 9 | 0 | rejeitado por detecção |
| Híbrido `fast=1`, `full=2`, repair `1..5` | 71.781 | 0 | 12 | rejeitado por detecção |
| Híbrido `fast=1`, `full=2`, repair `0..5` | 116.744 | 0 | 0 | correto, mas redundante/lento |

Conclusão do screening: os modos aproximados são muito rápidos, mas qualquer FP/FN derruba o score de forma pior do que o ganho de p99. O caminho útil continua sendo reduzir custo do modo exato.

### Experimento aceito: early-exit no lower bound das bounding boxes

Hipótese: durante o repair, `bbox_lower_bound` calculava as 14 dimensões mesmo quando a soma parcial já excedia `top.worst_distance()`. Como esses clusters nunca podem conter um candidato melhor, a função pode parar assim que `sum > worst`, preservando exatamente a mesma decisão.

Mudança aplicada:

```text
cpp/src/ivf.cpp
- bbox_lower_bound agora recebe stop_after
- retorna assim que a soma parcial excede o pior vizinho atual
- a chamada cacheia top.worst_distance() por cluster antes da comparação
```

Validação offline pós-patch:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| `nprobe=2`, bbox repair direto, repeat 5 | 70.096 | 0 | 0 | 0 | aceito |
| `nprobe=1`, bbox repair direto, repeat 5 | 69.691 | 0 | 0 | 0 | aceito e ligeiramente melhor |

Ganho isolado: o modo exato caiu de aproximadamente `102.306 ns/query` para `69.691 ns/query`, cerca de `31.9%` menos tempo por query no classificador.

Justificativa técnica para `nprobe=1`: com bbox repair habilitado, `nprobe` só define os clusters iniciais usados para preencher o top-5. Depois disso, qualquer cluster cujo lower bound ainda possa vencer o pior vizinho atual é escaneado. Como o lower bound da bounding box é conservador, essa poda preserva a busca exata no índice.

### Validação k6 oficial local

Configuração testada:

```text
IVF_FAST_NPROBE=1
IVF_FULL_NPROBE=1
IVF_BOUNDARY_FULL=false
IVF_BBOX_REPAIR=true
IVF_REPAIR_MIN_FRAUDS=0
IVF_REPAIR_MAX_FRAUDS=5
```

Resultados:

| Run | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| early-exit + `nprobe=1`, run 1 | 3.18ms | 0 | 0 | 0 | 5497.76 | aceito |
| early-exit + `nprobe=1`, run 2 | 3.12ms | 0 | 0 | 0 | 5505.63 | aceito; melhor run da branch |

Comparação contra o melhor estado anterior desta branch:

| Estado | Melhor p99 | Melhor score | FP | FN | HTTP |
|---|---:|---:|---:|---:|---:|
| Single-pass IVF pré-patch | 3.27ms | 5484.83 | 0 | 0 | 0 |
| Early-exit bbox + `nprobe=1` | 3.12ms | 5505.63 | 0 | 0 | 0 |

Ganho observado no melhor k6: `+20.80` pontos e `-0.15ms` de p99 contra o melhor checkpoint anterior da branch. Contra a submissão pública final registrada antes da rodada (`3.24ms`, `5489.47`), a melhor run experimental melhora `+16.16` pontos e `-0.12ms` de p99.

### Decisão

Manter o patch de early-exit e reduzir `IVF_FAST_NPROBE`/`IVF_FULL_NPROBE` para `1` nesta branch experimental. A mudança é sustentável porque:

- preserva a busca exata por argumento de lower bound conservador;
- manteve `0` FP/FN no benchmark offline e em duas execuções k6 completas;
- reduz CPU do classificador de forma material;
- melhora o score end-to-end de forma reproduzida.

Próximo passo investigativo: procurar outra poda exata no hot path do IVF, preferencialmente evitando trabalho em `already_scanned` ou melhorando a representação das bounding boxes, mas sem aceitar modos aproximados com erro.

### Experimento rejeitado: remover checagem de cluster vazio no repair

Hipótese: remover o branch `offsets_[cluster] == offsets_[cluster + 1]` poderia reduzir uma checagem por cluster no repair. A semântica seria preservada porque `scan_blocks` com intervalo vazio não faz trabalho.

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Sem checagem explícita de cluster vazio | 70.854 | 0 | 0 | 0 | rejeitado |
| Checkpoint aceito anterior | 69.691 | 0 | 0 | 0 | manter |

Decisão: revertido. A checagem explícita é mais barata do que chamar o restante do caminho para clusters vazios ou piora o perfil de branch/cache nesta carga.

### Experimento rejeitado: especializar `already_scanned` para `nprobe=1`

Hipótese: como a configuração aceita usa `nprobe=1`, trocar o loop genérico por comparação direta contra `best_clusters[0]` poderia reduzir branches no repair.

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| `already_scanned` especializado para `nprobe=1` | 70.077 | 0 | 0 | 0 | rejeitado |
| Checkpoint aceito anterior | 69.691 | 0 | 0 | 0 | manter |

Decisão: revertido. A diferença ficou dentro de micro-ruído e não justifica deixar código mais ramificado.

### Experimento rejeitado: ponteiros base em `bbox_lower_bound`

Hipótese: trocar `bbox_min[base + dim]` e `bbox_max[base + dim]` por ponteiros base locais poderia reduzir aritmética de índice no loop de 14 dimensões.

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Ponteiros base locais para min/max | 70.230 | 0 | 0 | 0 | rejeitado |
| Checkpoint aceito anterior | 69.691 | 0 | 0 | 0 | manter |

Decisão: revertido. O compilador já gera código suficientemente bom para a forma indexada; a alteração não trouxe ganho mensurável.

### Experimento rejeitado: ordem customizada das dimensões do bbox

Hipótese: como `bbox_lower_bound` agora tem early-exit, somar primeiro dimensões de maior variância poderia estourar `top.worst_distance()` mais cedo. A ordem testada foi derivada da variância global dos `3.000.000` vetores de referência:

```text
6, 5, 10, 9, 11, 2, 4, 7, 0, 1, 8, 12, 3, 13
```

Screening offline:

| Ordem | ns/query | FP | FN | parse_errors | Decisão offline |
|---|---:|---:|---:|---:|---|
| Ordem original `0..13` com early-exit | 69.691 | 0 | 0 | 0 | baseline aceito |
| Variância global `6,10,9,5,...` | 67.838 | 0 | 0 | 0 | promissor |
| Sentinelas primeiro `6,5,10,9,...` | 67.832 | 0 | 0 | 0 | melhor offline |
| Binárias primeiro `10,9,6,5,...` | 71.012 | 0 | 0 | 0 | rejeitado |
| Inversão `5,6,10,9,...` | 71.253 | 0 | 0 | 0 | rejeitado |

Validação k6 da melhor ordem offline:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Ordem customizada `6,5,10,9,...` | 3.38ms | 0 | 0 | 0 | 5470.47 | rejeitado |
| Checkpoint aceito anterior | 3.12ms | 0 | 0 | 0 | 5505.63 | manter |

Decisão: revertido. A ordem customizada melhora o microbenchmark do classificador, mas piora a cauda end-to-end no k6. Nesta stack, k6 continua sendo gate soberano.

### Experimento rejeitado: reservar body por `content-length`

Hipótese: ler o header `content-length` e chamar `context->body.reserve(size)` poderia evitar realocações do `std::string` no recebimento do payload.

Validações:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `content-length` + `body.reserve` | 3.69ms | 0 | 0 | 0 | 5432.44 | rejeitado |
| Checkpoint aceito anterior | 3.12ms | 0 | 0 | 0 | 5505.63 | manter |

Decisão: revertido. O custo de buscar/parsear header no hot path é maior do que qualquer economia de alocação para payloads desse tamanho.

### Experimento rejeitado: voltar para 2 APIs com early-exit

Hipótese: com o classificador IVF mais barato após early-exit, uma topologia de 2 APIs poderia ganhar por dar mais CPU para cada instância e reduzir contenção de processos.

Configuração testada:

```text
api1/api2: 0.41 CPU / 165MB cada
nginx:     0.18 CPU / 20MB
total:     1.00 CPU / 350MB
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 2 APIs + nginx `0.18`, early-exit | 4.88ms | 0 | 0 | 0 | 5311.54 | rejeitado |
| 3 APIs + nginx `0.19`, checkpoint aceito | 3.12ms | 0 | 0 | 0 | 5505.63 | manter |

Decisão: revertido. Mesmo com classificador mais barato, 2 APIs piora a cauda local. A topologia de 3 APIs segue melhor para absorver o ramp de 900 RPS.

### Experimento rejeitado: `UWS_HTTPRESPONSE_NO_WRITEMARK`

Hipótese: remover os headers automáticos `Date` e `uWebSockets` gerados pelo uWebSockets em cada resposta reduziria bytes e escritas no hot path. A API não exige esses headers, então a mudança seria compatível se melhorasse p99.

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `UWS_HTTPRESPONSE_NO_WRITEMARK` | 3.24ms | 0 | 0 | 0 | 5489.23 | rejeitado |
| Checkpoint aceito anterior | 3.12ms | 0 | 0 | 0 | 5505.63 | manter |

Decisão: revertido. A remoção de headers é funcionalmente segura, mas não melhorou a cauda no k6 local.

### Run de controle após reversões

Depois de reverter os experimentos rejeitados (`content-length reserve`, 2 APIs e `NO_WRITEMARK`), subi novamente o estado aceito da branch para garantir que o runtime não ficou contaminado por imagens/containers dos testes anteriores.

Configuração de controle:

```text
3 APIs + nginx stream
IVF_FAST_NPROBE=1
IVF_FULL_NPROBE=1
IVF_BOUNDARY_FULL=false
IVF_BBOX_REPAIR=true
repair=0..5
```

Resultado:

| Configuração | p99 | FP | FN | HTTP | Score | Observação |
|---|---:|---:|---:|---:|---:|---|
| Controle pós-reversões | 3.03ms | 0 | 0 | 0 | 5518.47 | melhor run da branch até agora |

Conclusão: o melhor estado técnico permanece `early-exit bbox + nprobe=1`. A melhor run local da branch subiu para `5518.47`, com `0` erro de detecção e `p99=3.03ms`.

### Experimento rejeitado: resposta direta por bucket

Hipótese: no caminho IVF, retornar diretamente o bucket `0..5` de fraude evitaria construir `Classification`, multiplicar `fraud_count * 0.2f` e recalcular o bucket com `floor` no hot path de resposta.

Validações locais antes do k6:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Bucket direto no `main.cpp` | 4.43ms | 0 | 0 | 0 | 5353.67 | rejeitado |
| Controle aceito anterior | 3.03ms | 0 | 0 | 0 | 5518.47 | manter |

Decisão: revertido. A alteração é funcionalmente correta, mas piora a cauda de forma relevante. A hipótese provável é que a mudança de assinatura/ramificação não reduz o custo dominante e atrapalha a otimização do compilador no caminho atual.

### Experimento rejeitado: centróide AVX2 especializado para `nprobe=1`

Hipótese: como os centróides são armazenados em layout transposto (`dim * clusters + cluster`) e a configuração aceita usa `nprobe=1`, uma busca do centróide mais próximo em blocos AVX2 de 8 clusters poderia trocar acessos com stride por leituras contíguas e reduzir o custo antes do repair.

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
./cpp/build/benchmark-ivf-cpp /tmp/rinha-2026-official-data/test-data.json /tmp/rinha-2026-index.bin 5 0 1 1 1
```

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Centróide AVX2 `nprobe=1` | 140.727 | 0 | 0 | 0 | rejeitado |
| Baseline da rodada | 133.130 | 0 | 0 | 0 | manter |

Decisão: revertido sem k6. A hipótese preservou a métrica e a classificação, mas a redução de locality não compensou o custo extra de acumular/storar/reduzir blocos AVX2; o caminho escalar atual continua melhor para a etapa de seleção de centróide.

### Experimento rejeitado: HAProxy HTTP sobre Unix socket

Hipótese: como a melhor submissão parcial pública em C usa HAProxy HTTP com Unix Domain Socket, testar HAProxy como load balancer da nossa stack poderia reduzir overhead de proxy em relação ao nginx `stream`.

Configuração testada:

```text
3 APIs C++/uWebSockets
HAProxy 3.3
backend via unix@/sockets/api{1,2,3}.sock
api:     0.27 CPU / 110MB cada
haproxy: 0.19 CPU / 20MB
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| HAProxy HTTP + 3 APIs | 18.53ms | 0 | 0 | 0 | 4732.12 | rejeitado |
| nginx `stream` + 3 APIs, controle aceito | 3.03ms | 0 | 0 | 0 | 5518.47 | manter |

Decisão: revertido. O HAProxy funciona e mantém a precisão, mas adicionou cauda muito maior na nossa combinação com uWebSockets/UDS. A vantagem observada no líder parece estar acoplada ao servidor C/io_uring e não se transfere diretamente para esta stack C++.

### Run de controle após retorno para nginx

Depois do teste com HAProxy, subi novamente o compose com nginx `stream` e removi órfãos do serviço anterior para evitar porta contaminada.

Resultado:

| Configuração | p99 | FP | FN | HTTP | Score | Observação |
|---|---:|---:|---:|---:|---:|---|
| Controle nginx pós-HAProxy | 3.17ms | 0 | 0 | 0 | 5498.46 | runtime limpo, abaixo da melhor run local |

Conclusão: o controle continua correto, mas com variação normal de cauda. O melhor local da branch segue `3.03ms / 5518.47`; a melhor prévia oficial da submissão foi `2.83ms / 5548.91`.

### Experimento rejeitado: quantização sem `std::lround`

Hipótese: substituir `std::lround(value * 10000)` por arredondamento manual equivalente reduziria custo de libm no caminho quente, já que cada requisição quantiza 14 dimensões.

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
./cpp/build/benchmark-ivf-cpp /tmp/rinha-2026-official-data/test-data.json /tmp/rinha-2026-index.bin 5 0 1 1 1
```

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Arredondamento manual | 158.110 | 0 | 0 | 0 | rejeitado |
| Baseline da rodada | 133.130 | 0 | 0 | 0 | manter |

Decisão: revertido sem k6. A alteração preserva classificação, mas piora o tempo. A hipótese provável é que `std::lround` já está bem otimizado no build atual e a expressão manual introduz branch/conversão menos favorável.

### Experimento rejeitado: flags `haswell`

Hipótese: como o ambiente oficial é um Mac Mini Late 2014 com CPU Intel Haswell e a melhor submissão C pública compila com `-march=haswell -mtune=haswell -flto -fomit-frame-pointer`, trocar o alvo genérico `x86-64-v3` por Haswell poderia melhorar o código gerado para a máquina oficial.

Alteração testada:

```text
-mavx2 -mfma -march=x86-64-v3
para
-march=haswell -mtune=haswell -fomit-frame-pointer
```

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Flags `haswell` | 159.991 | 0 | 0 | 0 | rejeitado |
| Baseline da rodada | 133.130 | 0 | 0 | 0 | manter |

Decisão: revertido sem k6. Apesar de ser coerente com a CPU oficial, a troca piorou muito no microbenchmark local da nossa base C++/simdjson/uWebSockets. Sem sinal local positivo, não vale arriscar o binário da submissão.

### Experimento rejeitado: centróides row-major no índice IVF

Hipótese: o índice atual armazena centróides em layout transposto (`dim * clusters + cluster`), mas o hot path escalar percorre `cluster -> dim`. Trocar o arquivo binário para layout row-major (`cluster * 14 + dim`) poderia reduzir acessos com stride durante a escolha do centróide mais próximo.

Alterações testadas:

```text
kMagic IVF8 -> IVF9 para evitar carregar índice antigo incompatível
centroids_[cluster * 14 + dim] no build
centroids_[cluster * 14 + dim] na consulta
```

Validações:

```text
cmake --build cpp/build --target prepare-ivf-cpp benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
cpp/build/prepare-ivf-cpp /tmp/rinha-2026-official-data/references.json.gz /tmp/rinha-2026-index-rowmajor.bin 2048 65536 6
./cpp/build/benchmark-ivf-cpp /tmp/rinha-2026-official-data/test-data.json /tmp/rinha-2026-index-rowmajor.bin 5 0 1 1 1
```

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Centróides row-major | 149.714 | 0 | 0 | 0 | rejeitado |
| Baseline da rodada | 133.130 | 0 | 0 | 0 | manter |

Decisão: revertido sem k6. A correção foi preservada, mas o hot path ficou mais lento. A interpretação provável é que o custo dominante não é o stride dos 14 floats por centróide, ou que o layout transposto atual interage melhor com cache/prefetch no conjunto real de consultas.

### Experimento rejeitado: remover `Content-Type` da resposta

Hipótese: o contrato do teste valida `status` e faz `JSON.parse(res.body)`, mas não exige header de resposta. Remover `res->writeHeader("Content-Type", "application/json")` poderia reduzir uma chamada no uWebSockets e alguns bytes por resposta.

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Sem `Content-Type` | 3.35ms | 0 | 0 | 0 | 5475.29 | rejeitado |
| Controle aceito anterior | 3.03ms | 0 | 0 | 0 | 5518.47 | manter |

Decisão: revertido. A alteração é compatível com o contrato observado e mantém acurácia, mas piora a cauda do k6. O header explícito atual continua sendo a opção mais estável nesta stack.

### Experimento rejeitado: MCC por `switch` numérico

Hipótese: substituir a cadeia de comparações `std::string == "5411"` etc. por decodificação fixa dos 4 dígitos e `switch` numérico reduziria custo de vetorização sem alterar a regra oficial nem o default `0.5`.

Resultado offline pareado:

| Configuração | ns/query | FP | FN | parse_errors |
|---|---:|---:|---:|---:|
| Baseline antes da mudança | 156.673 | 0 | 0 | 0 |
| MCC por `switch` | 153.945 | 0 | 0 | 0 |

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| MCC por `switch` | 3.37ms | 0 | 0 | 0 | 5472.84 | rejeitado |
| Controle aceito anterior | 3.03ms | 0 | 0 | 0 | 5518.47 | manter |

Decisão: revertido. Apesar do ganho offline de aproximadamente 1,7%, a cauda no k6 piorou. Este reforça que microganhos de CPU abaixo de poucos microssegundos não são suficientes se mudam layout/branching do binário de forma desfavorável para o runtime sob proxy e throttling.

### Experimento rejeitado: `RequestContext` com ponteiro cru

Hipótese: substituir `std::make_shared<RequestContext>` por `new/delete` explícito e limpar `onAborted` ao finalizar evitaria refcount atômico por requisição e removeria o branch `context->aborted` do caminho normal.

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `RequestContext` com ponteiro cru | 3.34ms | 0 | 0 | 0 | 5476.35 | rejeitado |
| Controle aceito anterior | 3.03ms | 0 | 0 | 0 | 5518.47 | manter |

Decisão: revertido. A mudança não melhorou p99 e ainda aumenta a superfície de risco de lifetime em aborts. O `shared_ptr` atual fica mantido por ser mais seguro e mais estável no k6.

### Run de controle após rejeições HTTP/parser

Depois dos experimentos rejeitados de `Content-Type`, MCC por `switch` e `RequestContext` cru, reconstruí a imagem no estado aceito para separar regressão real de variação do ambiente.

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Observação |
|---|---:|---:|---:|---:|---:|---|
| Controle limpo pós-rejeições | 3.19ms | 0 | 0 | 0 | 5496.81 | faixa atual da máquina |
| Melhor run local da branch | 3.03ms | 0 | 0 | 0 | 5518.47 | melhor histórico local |
| Prévia oficial da submissão | 2.83ms | 0 | 0 | 0 | 5548.91 | melhor evidência oficial |

Leitura: a máquina local está mais próxima do controle pós-HAProxy (`3.17ms / 5498.46`) do que da melhor run histórica (`3.03ms / 5518.47`). Mesmo assim, os experimentos recentes em `3.34-3.37ms` ficaram abaixo desse controle limpo, então permanecem rejeitados.

### Experimento rejeitado: centróide com query quantizada

Hipótese: a implementação C líder calcula o centróide mais próximo usando a query quantizada e reescalada (`q_i16 / 10000`), enquanto nossa busca usava o vetor float original nessa etapa. Como o scan e o bbox repair já operam no espaço quantizado, alinhar a seleção inicial ao mesmo grid poderia reduzir trabalho médio.

Resultado offline pareado:

| Configuração | ns/query | FP | FN | parse_errors |
|---|---:|---:|---:|---:|
| Baseline antes da mudança | 156.673 | 0 | 0 | 0 |
| Query quantizada para centróide | 156.063 | 0 | 0 | 0 |

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Query quantizada para centróide | 3.39ms | 0 | 0 | 0 | 5470.08 | rejeitado |
| Controle limpo pós-rejeições | 3.19ms | 0 | 0 | 0 | 5496.81 | manter |

Decisão: revertido. A técnica do líder é coerente no C/io_uring dele, mas no nosso C++/uWebSockets o pequeno ganho offline virou pior cauda no k6.

### Experimento rejeitado: HAProxy TCP/L4 sobre Unix socket

Hipótese: o HAProxy HTTP já havia sido rejeitado, mas ainda faltava testar HAProxy em modo TCP/L4, equivalente conceitual ao nginx `stream`, para separar custo de proxy HTTP de custo do balanceador.

Configuração testada:

```text
HAProxy 3.3
mode tcp
3 APIs C++/uWebSockets via /sockets/api{1,2,3}.sock
api:     0.27 CPU / 110MB cada
haproxy: 0.19 CPU / 20MB
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| HAProxy TCP/L4 + 3 APIs | 3.25ms | 0 | 0 | 0 | 5488.38 | rejeitado |
| Controle limpo nginx `stream` | 3.19ms | 0 | 0 | 0 | 5496.81 | manter |

Decisão: revertido. O HAProxy TCP funciona e é muito melhor que HAProxy HTTP nesta stack, mas ainda perde para nginx `stream` no controle fresco. O LB principal permanece nginx.

### Experimento rejeitado: parser com `padded_string_view`

Hipótese: reservar capacidade fixa no corpo HTTP e parsear com `simdjson::padded_string_view` evitaria a cópia interna feita por `simdjson::padded_string`, reduzindo custo no hot path do `POST /fraud-score`.

Contexto medido do dataset de teste:

```text
payload size: min=358 bytes, max=469 bytes, avg=434.544 bytes
reserva testada: 1024 bytes por RequestContext
fallback: se capacity < size + SIMDJSON_PADDING, usa parse_payload original
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `padded_string_view` + reserva 1024 | 3.33ms | 0 | 0 | 0 | 5477.44 | rejeitado |
| Controle limpo nginx `stream` | 3.19ms | 0 | 0 | 0 | 5496.81 | manter |

Decisão: revertido. A mudança preservou correção, mas piorou a cauda. A cópia evitada pelo simdjson não é o gargalo dominante no compose oficial local; a reserva por request e o caminho adicional de parser não compensaram sob k6.

### Experimento rejeitado: retunar quantidade de clusters IVF

Hipótese: o ponto ótimo do IVF poderia estar levemente fora de `2048` clusters. Menos clusters reduzem custo de seleção de centróide/bbox, mas aumentam o número médio de vetores escaneados por bucket; mais clusters fazem o inverso. Como isso muda o índice gerado, a validação inicial foi offline contra o dataset oficial local antes de qualquer alteração no `Dockerfile`.

Comando-base:

```text
prepare-ivf-cpp /tmp/rinha-2026-official-data/references.json.gz /tmp/rinha-ivf-official-<clusters>.bin <clusters> 65536 6
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-<clusters>.bin <repeat> 0 1 1 1
```

Varredura inicial:

| Clusters | ns/query | FP | FN | parse_errors |
|---:|---:|---:|---:|---:|
| 256 | 343633 | 0 | 0 | 0 |
| 512 | 243761 | 0 | 0 | 0 |
| 1024 | 175780 | 0 | 0 | 0 |
| 1536 | 162293 | 0 | 0 | 0 |
| 1792 | 160102 | 0 | 0 | 0 |
| 2048 | 160804 | 0 | 0 | 0 |
| 2304 | 161012 | 0 | 0 | 0 |
| 2560 | 162993 | 0 | 0 | 0 |
| 4096 | 191418 | 0 | 0 | 0 |

Revalidação pareada do melhor candidato contra o baseline:

| Ordem | Clusters | ns/query | FP | FN | parse_errors |
|---:|---:|---:|---:|---:|---:|
| 1 | 2048 | 156575 | 0 | 0 | 0 |
| 2 | 1792 | 158942 | 0 | 0 | 0 |
| 3 | 2048 | 158264 | 0 | 0 | 0 |
| 4 | 1792 | 160563 | 0 | 0 | 0 |

Decisão: rejeitado. `1792` pareceu competitivo na primeira varredura, mas perdeu nas execuções pareadas. `2048` continua sendo o ponto mais robusto entre os tamanhos testados, então o índice do `Dockerfile` permanece inalterado.

### Tentativa interrompida: benchmark de request genérico

Objetivo: medir isoladamente se o parser DOM do simdjson ainda era gargalo relevante antes de iniciar uma reescrita manual do parser.

Comando iniciado:

```text
benchmark-request-cpp /tmp/rinha-2026-official-run/test-data.json resources/references.json.gz 20
```

Resultado: interrompido manualmente. O benchmark existente também executa uma etapa final de classificador exato, o que torna o comando pesado demais para esta rodada e pouco representativo da stack atual baseada em IVF. A conclusão operacional é não usar esse binário como gate para parser sem antes criar um modo leve específico para parse/vectorize.

### Experimento rejeitado: early-skip no scan AVX2 por bloco

Hipótese: no `scan_blocks_avx2`, calcular as primeiras 7 dimensões e pular as 7 restantes quando todas as 8 lanes do bloco já excedem o pior top-5 atual reduziria bastante CPU no repair do IVF sem alterar a distância exata dos candidatos que continuam.

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-2048.bin 3 0 1 1 1
docker compose up -d --build --remove-orphans
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado offline:

| Configuração | ns/query | FP | FN | parse_errors |
|---|---:|---:|---:|---:|
| Early-skip AVX2, run 1 | 123066 | 0 | 0 | 0 |
| Early-skip AVX2, run 2 | 119977 | 0 | 0 | 0 |
| Early-skip AVX2, run 3 | 122376 | 0 | 0 | 0 |
| Controle pareado recente | 156575-158264 | 0 | 0 | 0 |

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Early-skip AVX2 por bloco | 3.33ms | 0 | 0 | 0 | 5477.05 | rejeitado |
| Controle limpo nginx `stream` | 3.19ms | 0 | 0 | 0 | 5496.81 | manter |

Decisão: revertido. O ganho offline foi real, mas não transferiu para o compose oficial local. A hipótese provável é que o hot path ficou mais branchy e menos previsível, enquanto a cauda do k6 continua dominada por proxy/throttling/scheduler. Como o score piorou, o scan AVX2 full-pass permanece.

### Experimento rejeitado: ordem de dimensões do scan inspirada no líder C

Hipótese: a implementação C líder usa ordem de dimensões voltada para maior poda (`5,6,2,0,7,8,11,12,9,10,1,13,3,4`). Testei a mesma ordem no nosso scan scalar e AVX2, sem o branch de early-skip por bloco, para verificar se o ganho vinha só da ordem de acumulação.

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-2048.bin 3 0 1 1 1
docker compose up -d --build --remove-orphans
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado offline:

| Run | ns/query | FP | FN | parse_errors |
|---:|---:|---:|---:|---:|
| 1 | 152827 | 0 | 0 | 0 |
| 2 | 162064 | 0 | 0 | 0 |
| 3 | 155516 | 0 | 0 | 0 |

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Ordem de scan do líder C | 3.46ms | 0 | 0 | 0 | 5460.54 | rejeitado |
| Controle limpo nginx `stream` | 3.19ms | 0 | 0 | 0 | 5496.81 | manter |

Decisão: revertido. A ordem do líder C faz sentido no layout SoA linear dele e no scalar com poda por dimensão. No nosso layout de blocos `dim * lanes`, a ordem natural preserva melhor localidade e vence no compose.
