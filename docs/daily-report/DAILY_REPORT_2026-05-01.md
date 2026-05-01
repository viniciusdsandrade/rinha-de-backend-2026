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

### Experimento rejeitado: fast path de timestamp para março/2026

Hipótese: os payloads oficiais locais usam timestamps em `2026-03`, então um fast path validado para esse mês poderia evitar parte do parsing genérico de data/hora, mantendo fallback completo para qualquer outro timestamp.

Checagem do dataset:

```text
transaction.requested_at e last_transaction.timestamp: 97328 ocorrências em 2026-03
epoch 2026-03-01T00:00:00Z: 1772323200
weekday de 2026-03-01: domingo
```

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-2048.bin 3 0 1 1 1
```

Resultado offline:

| Run | ns/query | FP | FN | parse_errors |
|---:|---:|---:|---:|---:|
| 1 | 154720 | 0 | 0 | 0 |
| 2 | 158390 | 0 | 0 | 0 |
| 3 | 160153 | 0 | 0 | 0 |
| Controle pareado recente | 156575-158264 | 0 | 0 | 0 |

Decisão: revertido sem k6. A otimização é correta e preservou os testes, mas não mostrou ganho offline sustentável. O custo de timestamp não é dominante frente ao IVF/proxy nesta stack.

### Experimento rejeitado: `-ffast-math` no runtime IVF

Hipótese: relaxar regras de ponto flutuante no binário da API e no benchmark IVF poderia acelerar cálculo de query/centróide sem alterar o índice gerado. O `prepare-ivf-cpp` foi mantido sem `-ffast-math` para isolar o runtime.

Escopo testado:

```text
rinha-backend-2026-cpp: -ffast-math
benchmark-ivf-cpp:      -ffast-math
prepare-ivf-cpp:        inalterado
```

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-2048.bin 3 0 1 1 1
```

Resultado offline:

| Run | ns/query | FP | FN | parse_errors |
|---:|---:|---:|---:|---:|
| 1 | 158824 | 0 | 0 | 0 |
| 2 | 159939 | 0 | 0 | 0 |
| 3 | 157271 | 0 | 0 | 0 |
| Controle pareado recente | 156575-158264 | 0 | 0 | 0 |

Decisão: revertido sem k6. Não houve ganho offline claro, e manter `-ffast-math` aumenta risco sem retorno mensurável.

### Experimento rejeitado: remover `res->cork` na resposta HTTP

Hipótese: como cada resposta já cabe em um payload JSON pequeno e pré-formatado, escrever header e body diretamente poderia reduzir overhead no hot path do uWebSockets. O teste removeu apenas o wrapper `res->cork`, mantendo `Content-Type` e corpo idênticos.

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
| Sem `res->cork` | 3.20ms | 0 | 0 | 0 | 5494.61 | rejeitado |
| Controle recente | 3.19ms | 0 | 0 | 0 | 5496.81 | manter |

Decisão: revertido. A diferença ficou dentro da zona de ruído e levemente pior que o controle recente, então não há evidência sustentável para remover `res->cork`. O caminho atual permanece mais seguro por preservar o agrupamento de escrita recomendado pelo uWebSockets.

### Experimento rejeitado: `ulimits nofile` e `seccomp=unconfined` nas APIs

Hipótese: a implementação C líder usa ajustes de runtime do container para reduzir overhead de syscalls/event-loop. Testei apenas `security_opt: seccomp=unconfined` e `ulimits.nofile=1048576` nas três APIs, sem alterar CPU, memória, nginx, imagem ou código.

Validações:

```text
docker compose up -d --no-build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
k6 run /tmp/rinha-2026-official-run/test.js
docker compose up -d --no-build --remove-orphans  # após reverter para estado aceito
```

Resultado k6:

| Configuração | Run | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| `ulimits` + `seccomp=unconfined` | 1 | 3.16ms | 0 | 0 | 0 | 5499.67 | inconclusivo |
| `ulimits` + `seccomp=unconfined` | 2 | 3.23ms | 0 | 0 | 0 | 5490.14 | rejeitado |
| Controle recente | 1 | 3.19ms | 0 | 0 | 0 | 5496.81 | manter |

Decisão: revertido. A primeira execução parecia ligeiramente melhor, mas a repetição perdeu desempenho. Como a mudança aumenta a superfície operacional e o ganho não reproduziu, ela não é sustentável para submissão.

### Controle da janela até 15h

Antes de iniciar novos experimentos da janela, rodei um controle fresco no estado aceito da branch para não comparar contra medições de outra condição do host.

Validação:

```text
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado:

| Configuração | p99 | FP | FN | HTTP | Score |
|---|---:|---:|---:|---:|---:|
| Estado aceito, controle 12:44 | 3.66ms | 0 | 0 | 0 | 5436.83 |

Leitura: a janela atual começou mais ruidosa que a melhor execução local e que a submissão oficial anterior (`2.83ms / 5548.91`). Portanto, qualquer aceitação nesta janela precisa de repetição; ganho isolado pequeno será tratado como ruído.

### Experimento rejeitado: reduzir `LIBUS_RECV_BUFFER_LENGTH` para 16KB

Hipótese: o uSockets usa um buffer de receive compartilhado de 512KB por loop. Como os payloads e headers do teste são pequenos, reduzir esse buffer para 16KB poderia melhorar cache/memória e diminuir cauda sem alterar contrato, classificação ou compose.

Mudança temporária:

```cmake
target_compile_definitions(usockets PUBLIC LIBUS_NO_SSL LIBUS_RECV_BUFFER_LENGTH=16384)
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `LIBUS_RECV_BUFFER_LENGTH=16384` | 5.02ms | 0 | 0 | 0 | 5299.56 | rejeitado |
| Controle fresco da janela | 3.66ms | 0 | 0 | 0 | 5436.83 | manter |

Decisão: revertido. A redução de buffer degradou fortemente a cauda. O buffer padrão de 512KB do uSockets permanece melhor para esse perfil, provavelmente por evitar ciclos de leitura/fragmentação interna mesmo com payloads pequenos.

### Experimento aceito: especializar o template IVF para `nprobe=1`

Hipótese: a configuração de produção usa `IVF_FAST_NPROBE=1` e `IVF_FULL_NPROBE=1`. Mesmo assim, o caminho `fraud_count_once` instanciava `fraud_count_once_fixed<8>` para qualquer `nprobe <= 8`, criando arrays e loops dimensionados para oito probes no caminho real. Instanciar `fraud_count_once_fixed<1>` quando `nprobe == 1` preserva exatamente a mesma busca, mas reduz overhead de stack/fill/comparações.

Mudança aplicada:

```cpp
if (nprobe == 1U) {
    return fraud_count_once_fixed<1>(query_i16, query_float, nprobe, repair);
}
```

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-2048.bin 3 0 1 1 1
cmake --build cpp/build --target rinha-backend-2026-cpp -j2
docker compose up -d --build --remove-orphans
k6 run /tmp/rinha-2026-official-run/test.js
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado offline:

| Run | ns/query | FP | FN | parse_errors |
|---:|---:|---:|---:|---:|
| 1 | 65118.6 | 0 | 0 | 0 |
| 2 | 66817.3 | 0 | 0 | 0 |
| Referência histórica aceita | 69691-70096 | 0 | 0 | 0 |

Resultado k6:

| Configuração | Run | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| `nprobe=1` especializado | 1 | 3.37ms | 0 | 0 | 0 | 5472.90 | melhor que controle da janela |
| `nprobe=1` especializado | 2 | 3.10ms | 0 | 0 | 0 | 5508.92 | aceito |
| Controle fresco da janela | 1 | 3.66ms | 0 | 0 | 0 | 5436.83 | superado |

Decisão: aceito na branch experimental. O ganho não supera a submissão oficial já processada (`2.83ms / 5548.91`), mas é um ganho técnico sustentável sobre o estado aceito da janela: mantém detecção perfeita, reduz custo offline do IVF e melhora o p99 local em duas execuções consecutivas contra o controle fresco.

### Experimento rejeitado: caminho interno dedicado para `fraud_count_once_fixed<1>`

Hipótese: depois de aceitar a instanciação `MaxNprobe=1`, um caminho interno ainda mais direto poderia remover `std::array`, `fill`, `insert_probe` e o loop genérico de `already_scanned`, usando apenas `best_cluster` e `best_distance` no caso de um único probe.

Validações:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-2048.bin 3 0 1 1 1
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-2048.bin 3 0 1 1 1
```

Resultado offline:

| Run | ns/query | FP | FN | parse_errors | Decisão |
|---:|---:|---:|---:|---:|---|
| 1 | 65626.3 | 0 | 0 | 0 | equivalente |
| 2 | 70511.6 | 0 | 0 | 0 | pior |
| Estado aceito anterior (`MaxNprobe=1` simples) | 65118.6-66817.3 | 0 | 0 | 0 | manter |

Decisão: revertido sem k6. A simplificação manual não melhorou de forma estável e provavelmente atrapalhou o perfil gerado pelo compilador. O caminho aceito continua sendo apenas instanciar `fraud_count_once_fixed<1>` e manter o corpo genérico.

### Experimento rejeitado: flags `-fno-exceptions` / `-fno-rtti`

Hipótese: como o hot path de produção não depende de exceções nem RTTI, remover esse suporte poderia reduzir tamanho/overhead do binário. A primeira tentativa aplicou `-fno-exceptions -fno-rtti` no target da API.

Resultado de build:

```text
main.cpp:84:14: error: exception handling disabled, use '-fexceptions' to enable
} catch (...) {
```

A causa é o parser de variáveis de ambiente (`std::stoul`) em `main.cpp`. Para manter escopo mínimo, não reescrevi esse trecho só para testar flag de compilação. A hipótese foi reduzida para `-fno-rtti` apenas.

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp -j2
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `-fno-rtti` | 3.49ms | 0 | 0 | 0 | 5457.09 | rejeitado |
| `nprobe=1` especializado, aceito | 3.10-3.37ms | 0 | 0 | 0 | 5472.90-5508.92 | manter |

Decisão: revertido. `-fno-exceptions` não compila sem refatorar parsing de env, e `-fno-rtti` piorou a cauda frente ao melhor estado da branch. Não vale aumentar complexidade de build por ganho inexistente.

### Experimento rejeitado: trocar `AppState` de `std::variant` para ponteiros explícitos

Hipótese: a aplicação roda em modo IVF durante os benchmarks. Substituir `std::variant<Classifier, IvfIndex>` por ponteiros explícitos (`std::unique_ptr<Classifier>` e `std::unique_ptr<IvfIndex>`) poderia remover `std::get_if` / `std::get` do caminho de classificação e reduzir um pequeno custo de dispatch por requisição.

Mudança temporária:

```cpp
std::unique_ptr<rinha::Classifier> exact_classifier;
std::unique_ptr<rinha::IvfIndex> ivf_index;
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `AppState` com ponteiros explícitos | 3.46ms | 0 | 0 | 0 | 5460.84 | rejeitado |
| `nprobe=1` especializado, aceito | 3.10-3.37ms | 0 | 0 | 0 | 5472.90-5508.92 | manter |

Decisão: revertido. A troca remove um dispatch trivial, mas introduz indireção por ponteiro e alocação heap no estado da aplicação. Na prática, a cauda ficou pior que a otimização aceita de `nprobe=1`; o `std::variant` continua suficientemente barato e mais simples.

### Experimento rejeitado: habilitar `-fno-exceptions` com parsing de env sem exceção

Hipótese: a tentativa anterior de `-fno-exceptions` falhou porque `main.cpp` ainda usava `std::stoul` / `std::stoi` com `catch (...)`. Substituir temporariamente esse parsing por uma rotina manual sem exceção permitiria medir a flag de forma justa. Como parsing de env não está no hot path, a única chance de ganho seria redução de tamanho/overhead do binário.

Mudanças temporárias:

```cpp
std::optional<std::uint32_t> parse_u32(std::string_view value);
```

```cmake
target_compile_options(rinha-backend-2026-cpp PRIVATE -fno-exceptions)
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| parsing manual + `-fno-exceptions` | 3.64ms | 0 | 0 | 0 | 5439.49 | rejeitado |
| `nprobe=1` especializado, aceito | 3.10-3.37ms | 0 | 0 | 0 | 5472.90-5508.92 | manter |

Decisão: revertido. A flag compila quando o parsing é ajustado, mas não entrega ganho mensurável e piora a cauda local. Não vale trocar código simples de bootstrap por parsing manual apenas para uma flag sem retorno.

### Experimento rejeitado: carregar `fraud_count` até a serialização da resposta

Hipótese: no modo IVF o número de fraudes dos 5 vizinhos já existe como inteiro. Guardar esse valor em `Classification` e fazer `classification_json` por `switch` inteiro evitaria `fraud_score * 5`, `std::floor` e `std::clamp` no final de cada request.

Mudanças temporárias:

```cpp
struct Classification {
    bool approved = true;
    float fraud_score = 0.0f;
    std::uint8_t fraud_count = 0;
};
```

```cpp
switch (classification.fraud_count) { ... }
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `fraud_count` no `Classification` | 3.54ms | 0 | 0 | 0 | 5450.84 | rejeitado |
| `nprobe=1` especializado, aceito | 3.10-3.37ms | 0 | 0 | 0 | 5472.90-5508.92 | manter |

Decisão: revertido. A micro-remoção de operações float não compensou o novo campo no layout de `Classification` / código gerado. O caminho anterior com `fraud_score` continua melhor na prática.

### Experimento aceito: remover `shared_ptr<RequestContext>` por POST

Hipótese: o endpoint `POST /fraud-score` alocava um `std::shared_ptr<RequestContext>` por requisição apenas para compartilhar `aborted` e `body` entre `onAborted` e `onData`. Como o fluxo normal responde sincronamente no `onData` final, manter o corpo dentro da própria closure de `onData` e usar `onAborted` vazio elimina uma alocação e contadores atômicos por POST sem alterar contrato de resposta.

Mudança aplicada:

```cpp
res->onAborted([]() {});
res->onData([res, state, body = std::string{}](std::string_view chunk, bool is_last) mutable {
    body.append(chunk.data(), chunk.size());
    ...
});
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
k6 run /tmp/rinha-2026-official-run/test.js
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | Run | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| body na closure `onData` | 1 | 3.28ms | 0 | 0 | 0 | 5483.58 | candidato |
| body na closure `onData` | 2 | 3.14ms | 0 | 0 | 0 | 5503.54 | candidato |
| body na closure `onData` | 3 | 3.12ms | 0 | 0 | 0 | 5505.61 | aceito |
| `nprobe=1` especializado, aceito anterior | 1 | 3.37ms | 0 | 0 | 0 | 5472.90 | referência |
| `nprobe=1` especializado, aceito anterior | 2 | 3.10ms | 0 | 0 | 0 | 5508.92 | referência |

Decisão: aceito na branch experimental. A melhor run ainda não supera a submissão oficial já processada (`2.83ms / 5548.91`), mas a sequência de três rodadas ficou estável e melhora a média local sobre o estado aceito anterior, preservando detecção perfeita.

### Experimento rejeitado: `body.reserve(512)` na closure `onData`

Hipótese: os payloads oficiais locais têm tamanho entre 358 e 469 bytes (`p99=468`). Reservar 512 bytes no `std::string` da closure poderia evitar crescimento incremental se o corpo chegasse fragmentado.

Medição prévia:

```text
jq '.entries | map(.request | tostring | length) | ...' /tmp/rinha-2026-official-run/test-data.json
min=358 max=469 avg=434.54 p50=442 p90=464 p99=468
```

Mudança temporária:

```cpp
res->onData([res, state, body = [] {
    std::string value;
    value.reserve(512);
    return value;
}()](...) mutable { ... });
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `body.reserve(512)` | 3.24ms | 0 | 0 | 0 | 5489.21 | rejeitado |
| body na closure sem reserva, aceito | 3.12-3.28ms | 0 | 0 | 0 | 5483.58-5505.61 | manter |

Decisão: revertido. O resultado é aceitável, mas não melhora a série sem reserva e deixa o código mais pesado. Provavelmente o corpo chega em chunk único na maioria das requisições, então a reserva antecipada só desloca a alocação.

### Experimento rejeitado: remover o handler `onAborted`

Hipótese: depois de mover o body para a closure de `onData`, o `onAborted([](){})` vazio poderia ser removido para reduzir mais um handler por requisição.

Mudança temporária:

```cpp
// removido:
res->onAborted([]() {});
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Failure rate | Score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| sem `onAborted` | 4.05ms | 0 | 0 | 54058 | 100% | -607.73 | rejeitado |
| body na closure com `onAborted` vazio, aceito | 3.12-3.28ms | 0 | 0 | 0 | 0% | 5483.58-5505.61 | manter |

Evidência do k6:

```text
Request Failed error="Post \"http://localhost:9999/fraud-score\": EOF"
```

Decisão: revertido. O handler vazio é necessário para o ciclo de vida do uWebSockets neste fluxo; removê-lo causa EOF em praticamente todas as requisições e aciona corte de detecção.

### Experimento rejeitado: `thread_local` para `known_merchants`

Hipótese: `parse_payload` cria um `std::vector<std::string>` local por request para armazenar `customer.known_merchants` até ler `merchant.id`. Como o parser simdjson já é `thread_local`, reutilizar também a capacidade do vector poderia reduzir alocação por request sem alterar a semântica.

Mudança temporária:

```cpp
thread_local std::vector<std::string> known_merchants;
known_merchants.clear();
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `thread_local known_merchants` | 3.50ms | 0 | 0 | 0 | 5455.41 | rejeitado |
| body na closure, aceito | 3.12-3.28ms | 0 | 0 | 0 | 5483.58-5505.61 | manter |

Decisão: revertido. A alocação do vector não apareceu como gargalo real; o TLS provavelmente aumentou custo de acesso/pressão de cache frente ao vector local pequeno.

### Experimento rejeitado: redistribuir CPU para APIs (`api=0.28`, `nginx=0.16`)

Hipótese: depois de estabilizar três APIs, mover CPU do nginx para as APIs poderia reduzir throttling no hot path de classificação. O split testado manteve o orçamento total em `1.00 CPU`, alterando cada API de `0.27` para `0.28` e o nginx de `0.19` para `0.16`.

Mudança temporária:

```yaml
api1/api2/api3:
  cpus: "0.28"
nginx:
  cpus: "0.16"
```

Validações:

```text
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `api=0.28`, `nginx=0.16` | 4.52ms | 0 | 0 | 0 | 5344.43 | rejeitado |
| split aceito `api=0.27`, `nginx=0.19` | 3.12-3.28ms | 0 | 0 | 0 | 5483.58-5505.61 | manter |

Decisão: revertido. O nginx continua sensível a CPU neste stack; reduzir o limite dele piorou p99 de forma clara sem alterar detecção.

### Experimento aceito: redistribuir CPU para nginx (`api=0.26`, `nginx=0.22`)

Controle pós-recriação do estado aceito anterior:

| Configuração | p99 | FP | FN | HTTP | Score |
|---|---:|---:|---:|---:|---:|
| `api=0.27`, `nginx=0.19` | 3.04ms | 0 | 0 | 0 | 5517.18 |

Hipótese: o experimento anterior mostrou que reduzir CPU do nginx para `0.16` piora muito o p99. O teste inverso aumenta o nginx para `0.22` e reduz as APIs para `0.26`, ainda respeitando o teto total de `1.00 CPU`.

Mudança:

```yaml
api1/api2/api3:
  cpus: "0.26"
nginx:
  cpus: "0.22"
```

Validações:

```text
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
k6 run /tmp/rinha-2026-official-run/test.js
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | Run | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| `api=0.26`, `nginx=0.22` | 1 | 3.02ms | 0 | 0 | 0 | 5520.04 | candidato |
| `api=0.26`, `nginx=0.22` | 2 | 2.98ms | 0 | 0 | 0 | 5526.49 | candidato |
| `api=0.26`, `nginx=0.22` | 3 | 3.02ms | 0 | 0 | 0 | 5519.71 | aceito |

Decisão: aceito. O ganho é pequeno, mas reproduziu em três rodadas sequenciais sem impacto de detecção, e a mudança é apenas redistribuição de CPU dentro do mesmo orçamento. A leitura prática é que o nginx ainda precisa de mais folga que `0.19 CPU` nesta topologia de três APIs.

### Experimento rejeitado: redistribuir CPU em partes iguais (`api=0.25`, `nginx=0.25`)

Hipótese: se o ganho do split `api=0.26/nginx=0.22` veio de gargalo no LB, aumentar o nginx para `0.25 CPU` poderia reduzir mais o p99. O custo seria reduzir cada API para `0.25 CPU`, mantendo o total em `1.00 CPU`.

Mudança temporária:

```yaml
api1/api2/api3:
  cpus: "0.25"
nginx:
  cpus: "0.25"
```

Validações:

```text
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | Run | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| `api=0.25`, `nginx=0.25` | 1 | 3.01ms | 0 | 0 | 0 | 5521.50 | candidato fraco |
| `api=0.25`, `nginx=0.25` | 2 | 3.07ms | 0 | 0 | 0 | 5513.12 | rejeitado |
| `api=0.26`, `nginx=0.22`, aceito | 1-3 | 2.98-3.02ms | 0 | 0 | 0 | 5519.71-5526.49 | manter |

Decisão: revertido. O split igualado não melhora a média e aumenta dispersão. A hipótese mais provável é que `0.22 CPU` já dá folga suficiente ao nginx, enquanto `0.25 CPU` começa a roubar CPU útil das APIs.

### Experimento rejeitado: split intermediário com CPU decimal (`api=0.255`, `nginx=0.235`)

Hipótese: testar um ponto intermediário entre o split aceito (`api=0.26/nginx=0.22`) e o split igualado rejeitado (`api=0.25/nginx=0.25`) poderia capturar um ponto ótimo de LB sem retirar CPU demais das APIs.

Mudança temporária:

```yaml
api1/api2/api3:
  cpus: "0.255"
nginx:
  cpus: "0.235"
```

Validações:

```text
docker compose config --quiet
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `api=0.255`, `nginx=0.235` | 3.06ms | 0 | 0 | 0 | 5514.90 | rejeitado |
| `api=0.26`, `nginx=0.22`, aceito | 2.98-3.02ms | 0 | 0 | 0 | 5519.71-5526.49 | manter |

Decisão: revertido. Além de piorar p99, o uso de limites decimais mais finos não se justifica sem ganho claro; `api=0.26/nginx=0.22` permanece o ponto mais defensável desta família.

### Experimento rejeitado: capturar `AppState*` no hot path

Hipótese: o callback `onData` capturava `std::shared_ptr<AppState>` por valor a cada request. Capturar um ponteiro cru para `AppState`, com lifetime garantido pelo `shared_ptr` em `main` durante `app.run()`, poderia remover incremento/decremento atômico do hot path.

Mudança temporária:

```cpp
const AppState* state_ptr = state.get();
app.post("/fraud-score", [state_ptr](auto* res, auto*) {
    res->onData([res, state_ptr, body = std::string{}](...) mutable {
        // ...
        state_ptr->classify(payload, classification, error);
    });
});
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `AppState*` no callback | 3.15ms | 0 | 0 | 0 | 5502.09 | rejeitado |
| `std::shared_ptr<AppState>` atual + split aceito | 2.98-3.02ms | 0 | 0 | 0 | 5519.71-5526.49 | manter |

Decisão: revertido. A hipótese era tecnicamente plausível, mas o k6 indicou piora. O efeito provável é alteração de layout/código gerado do callback maior que qualquer economia de referência atômica.

### Experimento rejeitado: `-fomit-frame-pointer` isolado

Hipótese: o teste anterior com flags `haswell` misturou `-march=haswell`, `-mtune=haswell` e `-fomit-frame-pointer`. Esta rodada isolou apenas `-fomit-frame-pointer` no target da API, mantendo `-march=x86-64-v3`, para medir se liberar o registrador de frame pointer ajudaria o hot path.

Mudança temporária:

```cmake
target_compile_options(rinha-backend-2026-cpp PRIVATE
    ...
    -fomit-frame-pointer
)
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `-fomit-frame-pointer` isolado | 3.18ms | 0 | 0 | 0 | 5497.56 | rejeitado |
| build aceito atual | 2.98-3.02ms | 0 | 0 | 0 | 5519.71-5526.49 | manter |

Decisão: revertido. A flag é tecnicamente segura, mas piorou a cauda no stack completo. O build atual com `x86-64-v3` sem `-fomit-frame-pointer` permanece mais competitivo.

### Experimento rejeitado: `MALLOC_ARENA_MAX=1`

Hipótese: cada API processa o hot path em um único event loop. Limitar o glibc malloc a uma arena poderia reduzir ruído/overhead de alocação em `std::string`, `simdjson::padded_string` e temporários pequenos.

Mudança temporária:

```yaml
environment:
  MALLOC_ARENA_MAX: "1"
```

Validações:

```text
docker compose config --quiet
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `MALLOC_ARENA_MAX=1` | 3.07ms | 0 | 0 | 0 | 5512.31 | rejeitado |
| alocador glibc padrão + split aceito | 2.98-3.02ms | 0 | 0 | 0 | 5519.71-5526.49 | manter |

Decisão: revertido. O alocador padrão ficou melhor neste workload; limitar arenas não reduziu a cauda e adicionaria configuração operacional sem retorno.

### Experimento rejeitado: backlog do Unix socket da API em `4096`

Hipótese: o nginx expõe `listen 9999 reuseport backlog=4096`, mas o uSockets usa backlog fixo `512` ao criar o Unix socket da API. Aumentar o backlog interno para `4096` poderia evitar fila curta entre nginx e APIs durante ramp de conexão.

Mudança temporária em `cpp/third_party/uWebSockets/uSockets/src/bsd.c`:

```c
listen(listenFd, 4096)
```

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp -j2
ctest --test-dir cpp/build --output-on-failure
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| UDS backlog `4096` | 3.32ms | 0 | 0 | 0 | 5479.27 | rejeitado |
| UDS backlog padrão `512` + split aceito | 2.98-3.02ms | 0 | 0 | 0 | 5519.71-5526.49 | manter |

Decisão: revertido. O backlog padrão do uSockets é melhor neste workload; aumentar a fila interna não reduz cauda e provavelmente aumenta buffering/latência entre nginx e APIs.

### Experimento rejeitado: `proxy_next_upstream off` no nginx stream

Hipótese: as APIs ficam estáveis durante o teste; portanto, desabilitar retry de upstream no nginx stream poderia reduzir lógica no caminho do proxy e evitar tentativa de failover desnecessária.

Mudança temporária:

```nginx
proxy_next_upstream off;
```

Validações:

```text
docker compose up -d --build --remove-orphans
docker compose exec -T nginx nginx -t
docker compose restart nginx
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `proxy_next_upstream off` | 3.05ms | 0 | 0 | 0 | 5515.08 | rejeitado |
| `proxy_next_upstream` padrão + split aceito | 2.98-3.02ms | 0 | 0 | 0 | 5519.71-5526.49 | manter |

Decisão: revertido. O comportamento padrão do nginx ficou melhor. Remover failover não trouxe ganho e ainda reduziria resiliência se alguma API fechasse conexão durante o teste.

## Fechamento da janela até 15h

Estado efetivamente mantido na branch `perf/noon-tuning`:

- `docker-compose.yml`: split de CPU ajustado para três APIs com `0.26 CPU / 110MB` cada e nginx com `0.22 CPU / 20MB`, totalizando `1.00 CPU / 350MB`.
- `cpp/src/main.cpp`: body HTTP armazenado diretamente na closure do `onData`, removendo a alocação do `RequestContext` por request.
- `cpp/src/ivf.cpp`: especialização do caminho `nprobe=1` mantida de rodadas anteriores.

Experimentos rejeitados nesta janela final:

- `api=0.28/nginx=0.16`: piorou para `4.52ms / 5344.43`.
- `api=0.25/nginx=0.25`: oscilou entre `3.01ms / 5521.50` e `3.07ms / 5513.12`.
- `api=0.255/nginx=0.235`: piorou para `3.06ms / 5514.90`.
- `AppState*` no hot path: piorou para `3.15ms / 5502.09`.
- `-fomit-frame-pointer`: piorou para `3.18ms / 5497.56`.
- `MALLOC_ARENA_MAX=1`: piorou para `3.07ms / 5512.31`.
- backlog UDS `4096` no uSockets: piorou para `3.32ms / 5479.27`.
- `proxy_next_upstream off`: piorou para `3.05ms / 5515.08`.

Melhor run obtida na janela:

| Configuração | p99 | FP | FN | HTTP | Score |
|---|---:|---:|---:|---:|---:|
| `api=0.26`, `nginx=0.22` | 2.98ms | 0 | 0 | 0 | 5526.49 |

Controle final no estado mantido:

| Run | p99 | FP | FN | HTTP | Score |
|---:|---:|---:|---:|---:|---:|
| 1 | 3.02ms | 0 | 0 | 0 | 5520.43 |
| 2 | 3.05ms | 0 | 0 | 0 | 5516.00 |
| 3 | 3.03ms | 0 | 0 | 0 | 5519.22 |

Média do controle final: `p99 ~3.03ms`, score médio `5518.55`, `0` falhas.

Comparação com o início da rodada de hoje:

| Referência | p99 | Score | Falhas | Observação |
|---|---:|---:|---:|---|
| Controle fresco inicial | 3.66ms | 5436.83 | 0 | Antes dos ajustes aceitos da janela |
| Melhor run da janela | 2.98ms | 5526.49 | 0 | Ganho de `+89.66` pontos sobre o controle inicial |
| Controle final médio | ~3.03ms | 5518.55 | 0 | Ganho médio de `+81.72` pontos sobre o controle inicial |
| Submissão oficial anterior | 2.83ms | 5548.91 | 0 | Ainda melhor que a melhor run local da janela por `+22.42` pontos |

Leitura técnica: a melhora sustentável desta janela veio de balancear CPU para o nginx sem retirar CPU demais das APIs. As demais hipóteses mexeram em hot path, alocador, build flags ou proxy, mas não superaram a série aceita. O próximo salto material provavelmente não está em knobs marginais de C++/nginx; deve vir de uma mudança estrutural no modelo de serving, no formato de parsing ou em reduzir ainda mais trabalho de classificação por request.

### Experimento rejeitado pós-fechamento: `proxy_timeout 5s`

Hipótese: reduzir o timeout de proxy do nginx stream de `30s` para `5s` poderia diminuir manutenção de estado de conexões presas sem afetar requisições normais.

Mudança temporária:

```nginx
proxy_timeout 5s;
```

Validações:

```text
docker compose exec -T nginx nginx -t
docker compose restart nginx
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `proxy_timeout 5s` | 3.04ms | 0 | 0 | 0 | 5516.55 | rejeitado |
| `proxy_timeout 30s` + split aceito | 2.98-3.03ms | 0 | 0 | 0 | 5519.22-5526.49 | manter |

Decisão: revertido. A mudança é operacionalmente razoável, mas não melhora score local; manter o default já validado evita mexer na semântica de conexões longas sem retorno mensurável.

### Sanity check pós-restore do nginx

Após reverter `proxy_timeout 5s` para `proxy_timeout 30s`, o nginx foi validado e reiniciado:

```text
docker compose exec -T nginx nginx -t
docker compose restart nginx
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado:

| Configuração | p99 | FP | FN | HTTP | Score | Leitura |
|---|---:|---:|---:|---:|---:|---|
| estado aceito restaurado, pós-restore | 3.13ms | 0 | 0 | 0 | 5503.86 | drift de fim de janela |
| série final anterior no mesmo estado versionado | 3.02-3.05ms | 0 | 0 | 0 | 5516.00-5520.43 | referência principal |

Leitura: sem falhas de detecção ou HTTP. A piora isolada parece variação ambiental após muitas rodadas sequenciais de build/k6, não mudança de configuração mantida. O estado versionado continua sendo `api=0.26/nginx=0.22`, `proxy_timeout 30s`, backlog UDS padrão e alocador padrão.

## Janela investigativa 15h-18h: leitura dos líderes e experimento de parser manual

### Achado externo: líderes reduziram custo fixo de servidor/parser

Fontes consultadas:

- `https://github.com/thiagorigonatti/rinha-2026` (`thiagorigonatti-c`, ranking parcial informado: `1.25ms`, `0%`, `5901.92`).
- `https://github.com/jairoblatt/rinha-2026-rust` (`jairoblatt-rust`, `1.45ms`, `0%`, `5838.50`).
- `https://github.com/joojf/rinha-2026` (`joojf`, `1.50ms`, `0%`, `5823.94`).
- `https://github.com/MuriloChianfa/cpp-fraud-detection-rinha-2026` (`murilochianfa-cpp-fraud-detection-rinha-2026`, `2.84ms`, `0%`, `5546.41`).
- `https://github.com/devRaelBraga/rinha-2026-xgboost` (`hisrael-xgboost-go`, `2.60ms`, `0%`, `5404.29`).

Resumo técnico do achado:

- O líder em C usa servidor HTTP manual com `io_uring`, UDS, HAProxy, índice binário IVF/K-Means, vetores `int16`, AVX2, top-5 determinístico e respostas HTTP pré-montadas.
- As soluções Rust de topo usam `monoio`/`io_uring`, parser HTTP/JSON manual, UDS, respostas constantes e busca IVF quantizada.
- As soluções acima de `~5820` pontos não parecem ganhar por trocar apenas nginx/HAProxy ou por microflag de compilação. O padrão recorrente é remover framework/parsing genérico do caminho quente.

Hipótese derivada: antes de reescrever servidor, testar a menor fatia reaproveitável no stack atual: parser manual direto para `QueryVector`, mantendo uWebSockets e IVF atuais.

### Experimento rejeitado: parser manual direto para `QueryVector`

Mudança temporária testada:

- Adição de `parse_query_vector(std::string_view, QueryVector&, std::string&)` em C++.
- Uso temporário desse parser no caminho IVF para evitar `simdjson::dom`, `Payload`, `std::string` de timestamps/MCC e `known_merchants`.
- Teste TDD de equivalência contra `parse_payload + vectorize` nos payloads: legítimo, fraude, clamp/MCC default/merchant desconhecido e merchant duplicado.

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-request-cpp -j2
ctest --test-dir cpp/build --output-on-failure
./cpp/build/benchmark-request-cpp test/test-data.json resources/references.json.gz 3 5000
```

Resultado funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultado offline:

| Caminho | ns/query | Checksum | Decisão |
|---|---:|---:|---|
| `parse_payload` | 605.48 | `1010687232893484` | referência |
| `parse_payload + vectorize` | 673.399 | `10261833810798` | referência efetiva atual |
| `parse_query_vector` manual | 1175.91 | `10261833810798` | rejeitado |

Leitura: embora o parser manual tenha gerado o mesmo vetor, ele foi `~75%` mais lento que o caminho atual `simdjson + vectorize`. A implementação ingênua baseada em múltiplos `std::string_view::find` e parser numérico manual não reproduz o ganho dos líderes; o ganho deles vem de um parser mais radical, sequencial/fixo, integrado ao servidor e ao layout de resposta. Rodar k6 seria desperdício: a hipótese já falhou no microbenchmark mecânico.

Decisão: protótipo revertido integralmente. Nenhuma mudança de produção foi mantida.

Próxima hipótese com melhor relação risco/retorno: avaliar HAProxy TCP/UDS com a topologia atual somente se a configuração dos líderes trouxer diferença concreta frente ao nginx stream; caso contrário, o próximo salto material exige servidor HTTP próprio ou monoio/io_uring, que deve ser tratado como branch/experimento estrutural separado.

### Experimento rejeitado: HAProxy TCP/UDS no lugar do nginx stream

Hipótese: os líderes `thiagorigonatti-c` e `jairoblatt-rust` usam HAProxy com Unix Domain Socket; portanto, talvez parte do gap viesse do nginx stream atual.

Mudança temporária:

- `nginx:1.27-alpine` substituído por `haproxy:3.0-alpine`.
- HAProxy em `mode tcp`, `balance roundrobin`, três backends UDS (`api1`, `api2`, `api3`).
- CPU/memória mantidas no mesmo orçamento do LB atual (`0.22 CPU / 20MB`) para isolar a variável proxy.
- APIs, IVF, parser, UDS e split `api=0.26` mantidos iguais.

Validações:

```text
docker compose up -d --build --remove-orphans
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| HAProxy TCP/UDS, 3 APIs | 3.21ms | 0 | 0 | 0 | 5493.72 | rejeitado |
| nginx stream aceito | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Leitura: HAProxy isolado piorou a latência local. A vantagem dos líderes não é o HAProxy em si; ela vem do conjunto servidor próprio/monoio/io_uring + parser integrado + layout quantizado. A configuração foi revertida para nginx stream.

### Experimento rejeitado: duas APIs maiores com nginx stream

Hipótese: os líderes concentram CPU em duas APIs grandes (`~0.40 CPU` cada) em vez de três APIs menores; talvez o nosso stack estivesse pagando overhead de uma terceira instância e subalocando CPU para cada processo.

Mudança temporária:

- Removida `api3` do `docker-compose.yml` e do upstream nginx.
- `api1/api2`: `0.39 CPU / 165MB` cada.
- nginx mantido em `0.22 CPU / 20MB`.
- Total preservado: `1.00 CPU / 350MB`.

Validações:

```text
docker compose up -d --build --remove-orphans
docker compose exec -T nginx nginx -t
curl http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `2 APIs x 0.39 CPU` + nginx `0.22` | 3.12ms | 0 | 0 | 0 | 5505.62 | rejeitado |
| `3 APIs x 0.26 CPU` + nginx `0.22` | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Leitura: no nosso stack uWebSockets/nginx, a terceira API ainda ajuda mais do que concentrar CPU em duas APIs. A topologia de duas APIs só parece vantajosa quando o servidor por API é muito mais barato, como nos líderes com C/io_uring ou Rust/monoio. Configuração revertida para três APIs.

### Screening rejeitado: IVF aproximado sem reparo e `nprobe=2` com reparo

Hipótese: os Rust de topo usam IVF aproximado com múltiplos probes e sem reparo exato; talvez fosse possível trocar um pequeno erro de detecção por grande redução de latência, ou usar mais probes com `bbox_repair=true` para reduzir o custo do reparo exato.

Validação offline:

```text
cmake --build cpp/build --target benchmark-ivf-cpp -j2
docker cp perf-noon-tuning-api1-1:/app/data/index.bin /tmp/rinha-2026-research/current/index.bin
./cpp/build/benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-2026-research/current/index.bin 1 0 <fast> <full> <bbox_repair>
```

Resultados offline sem `bbox_repair`:

| Configuração | ns/query | FP | FN | Failure rate | Decisão |
|---|---:|---:|---:|---:|---|
| `nprobe=1`, sem repair | 13929.8 | 143 | 148 | 0.5379% | rejeitado |
| `nprobe=2`, sem repair | 16024.6 | 52 | 50 | 0.1885% | rejeitado |
| `nprobe=4`, sem repair | 23326.3 | 10 | 12 | 0.0407% | rejeitado |
| `nprobe=8`, sem repair | 38254.8 | 6 | 3 | 0.0166% | rejeitado |
| `nprobe=12`, sem repair | 57842.1 | 4 | 2 | 0.0111% | rejeitado |
| `nprobe=16`, sem repair | 70164.0 | 3 | 1 | 0.0074% | rejeitado |
| `nprobe=24`, sem repair | 97791.0 | 2 | 1 | 0.0055% | rejeitado |

Leitura: mesmo erros pequenos custam caro na fórmula. Exemplo: `nprobe=4` tem `E = 10*1 + 12*3 = 46`, o que gera penalidade absoluta de aproximadamente `-501` pontos; mesmo que a latência saturasse em `1ms`, a troca ficaria no limite e não é sustentável.

Resultados offline com `bbox_repair=true`:

| Configuração | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `nprobe=1`, repair | 72640.0 | 0 | 0 | referência |
| `nprobe=2`, repair | 70404.0 | 0 | 0 | testar em k6 |
| `nprobe=4`, repair | 74292.8 | 0 | 0 | rejeitado |
| `nprobe=8`, repair | 83350.7 | 0 | 0 | rejeitado |
| `nprobe=12`, repair | 100915.0 | 0 | 0 | rejeitado |

Validação k6 da única variante promissora:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `nprobe=2`, `bbox_repair=true` | 3.16ms | 0 | 0 | 0 | 5499.83 | rejeitado |
| `nprobe=1`, `bbox_repair=true` | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Decisão: manter `IVF_FAST_NPROBE=1`, `IVF_FULL_NPROBE=1`, `IVF_BBOX_REPAIR=true`. O ganho offline de `nprobe=2` foi pequeno e não sobreviveu ao benchmark completo.

### Experimento rejeitado: `seccomp=unconfined` isolado nas APIs

Hipótese: os repositórios líderes que usam `io_uring` declaram `security_opt: seccomp=unconfined`; antes de mexer no event loop, valia isolar se só remover o filtro seccomp já reduzia overhead no nosso stack uWebSockets/nginx atual.

Mudança temporária:

- Adicionado `security_opt: [seccomp=unconfined]` no anchor comum das APIs.
- Nenhuma mudança de código, imagem, recursos, nginx, número de APIs ou parâmetros IVF.
- Total de recursos preservado em `1.00 CPU / 350MB`.

Fontes que motivaram a hipótese:

- `thiagorigonatti/rinha-2026`: C + `io_uring`, UDS, HAProxy, `seccomp=unconfined`.
- `jairoblatt/rinha-2026-rust`: Rust + `monoio`, UDS, HAProxy, `seccomp=unconfined`.
- `joojf/rinha-2026`: Rust + `monoio`, UDS, nginx, `seccomp=unconfined`.

Validação:

```text
docker compose up -d --force-recreate
curl -fsS http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| APIs com `seccomp=unconfined` | 3.16ms | 0 | 0 | 0 | 5500.67 | rejeitado |
| Configuração aceita sem `security_opt` | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Conclusão: `seccomp=unconfined` não melhora o stack atual de forma isolada. O valor dessa opção nos líderes provavelmente vem de destravar `io_uring`/runtime específico, não de reduzir overhead no caminho epoll/uWebSockets atual. A mudança foi revertida.

### Experimento rejeitado: `LIBUS_USE_IO_URING` no uSockets/uWebSockets

Hipótese: parte do gap para o top 3 vem do custo fixo do servidor HTTP/event loop. Como os líderes usam servidor C com `io_uring` ou Rust `monoio`, testei habilitar o backend `io_uring` do próprio uSockets para preservar a aplicação atual e trocar apenas o event loop.

Mudança temporária:

- `Dockerfile`: adicionados `liburing-dev` no builder e `liburing2` no runtime.
- `cpp/CMakeLists.txt`: `usockets` compilado com `LIBUS_USE_IO_URING` e linkado com `uring`.
- `docker-compose.yml`: `security_opt: [seccomp=unconfined]` nas APIs, necessário para reduzir risco de bloqueio do syscall.

Fontes que motivaram a hipótese:

- `uNetworking/uSockets`: o Makefile declara `WITH_IO_URING=1` como build com `-DLIBUS_USE_IO_URING` e link adicional com `liburing`.
- `uNetworking/uSockets`: o README ainda descreve `io_uring` como work-in-progress.
- `thiagorigonatti/rinha-2026`: C + `io_uring` manual no líder parcial.
- `jairoblatt/rinha-2026-rust` e `joojf/rinha-2026`: Rust + `monoio`/`io_uring` no top 3 parcial.

Validação:

```text
docker compose build api1
docker compose up -d --force-recreate
docker run --rm --security-opt seccomp=unconfined \
  -e UNIX_SOCKET_PATH=/tmp/test.sock \
  rinha-backend-2026-cpp-api:local
```

Resultado:

| Etapa | Resultado | Decisão |
|---|---|---|
| Build Docker com `liburing` | compilou | prosseguir para runtime |
| Startup API | falhou com exit 1 antes de abrir UDS | rejeitado |
| Mensagem observada | `io_uring_init_failed... : Success` | sem k6 |

Conclusão: o backend `io_uring` vendorizado no uSockets não é um caminho sustentável para esta submissão. Ele compila, mas não inicializa de forma confiável no ambiente Docker atual; além disso, o próprio upstream marca esse caminho como work-in-progress. A alteração foi revertida e a imagem aceita foi reconstruída com epoll/uWebSockets.

### Experimento rejeitado: IVF `12/24` com repair apenas na fronteira

Hipótese: os Rust do top 3 parcial usam índice com `K=4096` e reprocessamento só quando a votação inicial cai perto do threshold (`2` ou `3` fraudes). A nossa configuração aceita usa `K=2048`, `nprobe=1` e `bbox_repair=true` em todas as consultas. Testei uma variação intermediária: manter o índice atual `2048`, fazer primeiro passe aproximado com `12` probes, e executar `bbox_repair` completo só quando o primeiro voto fosse `2` ou `3`.

Fontes que motivaram a hipótese:

- `joojf/rinha-2026`: `K=4096`, `FAST_NPROBE=12`, `FULL_NPROBE=24`, retry quando `fast == 2 || fast == 3`.
- `jairoblatt/rinha-2026-rust`: `K=4096`, `FAST_NPROBE=16`, `FULL_NPROBE=24`, retry quando `fast == 2 || fast == 3`.
- Nosso benchmark anterior mostrou que `nprobe` sem repair introduz poucos erros, mas qualquer FP/FN derruba bastante o score; por isso o teste manteve repair na fronteira.

Screening offline:

```text
benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json <index> 2/3 0 <fast> <full> <bbox_repair> <min> <max>
```

| Configuração | ns/query | FP | FN | Failure rate | Decisão |
|---|---:|---:|---:|---:|---|
| `2048`, atual `1/1`, repair todas | 82364 | 0 | 0 | 0% | referência |
| `2048`, `12/24`, sem repair | 58079.9 | 6 | 3 | 0.0055% | rejeitado por erro |
| `2048`, `16/24`, sem repair | 74684.9 | 6 | 3 | 0.0055% | rejeitado por erro |
| `4096`, `12/24`, sem repair | 43389.7 | 6 | 9 | 0.0092% | rejeitado por erro |
| `4096`, `16/24`, sem repair | 53146.5 | 6 | 9 | 0.0092% | rejeitado por erro |
| `4096`, `64/64`, sem repair | 306900 | 4 | 2 | 0.0055% | rejeitado por erro e custo |
| `2048`, `1/1`, repair só `2..3` | 14455.3 | 42 | 60 | 0.0943% | rejeitado por erro |
| `2048`, `12/24`, repair só `2..3` | 59379.2 | 0 | 0 | 0% | testar em k6 |
| `4096`, `12/24`, repair só `2..3` | 49005 | 4 | 0 | 0.0037% | rejeitado por erro |

Validação k6 da única variante sem erro:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `2048`, `12/24`, repair só `2..3`, run 1 | 3.02ms | 0 | 0 | 0 | 5520.61 | inconclusivo |
| `2048`, `12/24`, repair só `2..3`, run 2 | 3.09ms | 0 | 0 | 0 | 5510.33 | rejeitado |
| Configuração aceita `1/1`, repair todas | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Conclusão: o microbenchmark mostrou redução real de CPU no classificador, mas esse ganho não virou p99 melhor no compose oficial local. Como a segunda run ficou fora da melhor faixa recente, a mudança foi revertida. O aprendizado é que o gargalo de p99 atual não é apenas custo médio do IVF; variações que reduzem ns/query ainda precisam reduzir cauda end-to-end para serem aceitas.

### Experimento rejeitado: 2 APIs com alocação API-heavy `0.45/0.45/0.10`

Hipótese: os líderes Rust usam duas APIs maiores e nginx pequeno (`0.45 CPU` por API, `0.10 CPU` no nginx). O teste anterior com duas APIs havia usado `0.39/0.39/0.22`, então ainda faltava medir a alocação API-heavy mais próxima do top 3.

Mudança temporária:

- Removida `api3` do `docker-compose.yml`.
- Removida `api3.sock` do upstream nginx.
- `api1/api2`: `0.45 CPU / 165MB`.
- nginx: `0.10 CPU / 20MB`.
- Total preservado: `1.00 CPU / 350MB`.

Validação:

```text
docker compose up -d --force-recreate --remove-orphans
curl -fsS http://localhost:9999/ready
k6 run /tmp/rinha-2026-official-run/test.js
```

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 2 APIs `0.45` + nginx `0.10` | 48.48ms | 0 | 0 | 0 | 4314.46 | rejeitado |
| 2 APIs `0.39` + nginx `0.22` | 3.12ms | 0 | 0 | 0 | 5505.62 | rejeitado anterior |
| 3 APIs `0.26` + nginx `0.22` | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Conclusão: a alocação API-heavy não se transfere para nosso stack. Com apenas duas APIs, o ramp final acumulou VUs e explodiu a cauda apesar de manter zero erro de detecção. A terceira API continua necessária para estabilidade de p99 nesta implementação C++/uWebSockets.

### Experimento rejeitado: 3 APIs com nginx reduzido para `0.10 CPU`

Hipótese: mantendo três APIs, talvez o nginx estivesse superalocado em `0.22 CPU`. Como os líderes Rust usam nginx menor, testei realocar CPU para as APIs sem mudar topologia.

Mudança temporária:

- `api1/api2/api3`: `0.30 CPU / 110MB` cada.
- nginx: `0.10 CPU / 20MB`.
- Total preservado: `1.00 CPU / 350MB`.

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 3 APIs `0.30` + nginx `0.10` | 49.96ms | 0 | 0 | 0 | 4301.39 | rejeitado |
| 3 APIs `0.26` + nginx `0.22` | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Conclusão: nesta stack, o nginx precisa de margem de CPU para não formar fila no final do ramp. Realocar CPU do LB para APIs piora drasticamente a cauda, mesmo com zero falhas funcionais. Configuração revertida para `nginx=0.22` e APIs `0.26`.

### Experimento rejeitado: nginx aumentado para `0.25 CPU`

Hipótese: como reduzir o nginx para `0.10 CPU` explodiu a cauda, talvez o p99 ainda estivesse limitado pelo LB e pudesse melhorar com mais CPU no nginx, sacrificando pouco das APIs.

Mudança temporária:

- `api1/api2/api3`: `0.25 CPU / 110MB` cada.
- nginx: `0.25 CPU / 20MB`.
- Total preservado: `1.00 CPU / 350MB`.

Resultado k6:

| Configuração | p99 | FP | FN | HTTP | Score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| 3 APIs `0.25` + nginx `0.25` | 3.13ms | 0 | 0 | 0 | 5504.97 | rejeitado |
| 3 APIs `0.26` + nginx `0.22` | 2.98-3.05ms | 0 | 0 | 0 | 5516.00-5526.49 | manter |

Conclusão: aumentar o nginx também não ajuda. O ponto atual `0.26/0.26/0.26 + 0.22` segue como split mais robusto: `0.10` falta CPU para o LB; `0.25` tira CPU demais das APIs e piora p99.
### Experimento rejeitado: servidor C++ epoll/UDS sem uWebSockets

Hipótese: os repositórios líderes indicam que parte relevante do gap está no custo fixo de servidor/framework. O líder em C (`https://github.com/thiagorigonatti/rinha-2026`) usa HTTP manual com `io_uring`, UDS, IVF quantizado `int16`, AVX2 e respostas HTTP pré-montadas; os líderes Rust (`https://github.com/jairoblatt/rinha-2026-rust` e `https://github.com/joojf/rinha-2026`) usam `monoio`/UDS, parser HTTP/JSON manual e respostas constantes. Para isolar apenas a camada de servidor, foi criado temporariamente um binário C++ `epoll`/UDS mantendo exatamente o parser `simdjson`, a vetorização e o IVF atuais.

Mudança temporária testada:

- Novo `cpp/src/epoll_main.cpp` com servidor HTTP mínimo sobre `epoll`, socket UDS, `GET /ready`, `POST /fraud-score`, keep-alive e seis respostas HTTP completas pré-montadas.
- Novo target CMake `rinha-backend-2026-cpp-epoll`.
- Dockerfile temporariamente apontado para o binário epoll.
- Primeiro screening com a topologia atual de `3 APIs + nginx`; segundo screening com a topologia dos líderes de `2 APIs` mais fortes.

Validações funcionais:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-epoll rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
curl /ready => 204
POST real do dataset => {"approved":false,"fraud_score":1.0}
```

Resultado funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultados k6 oficiais locais da janela:

| Variante | Topologia/recursos | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---|---:|---:|---:|---:|---:|---|
| Controle uWebSockets aceito | `3 APIs 0.26 CPU / nginx 0.22 CPU` | 3.23ms | 0 | 0 | 0 | 5491.33 | referência da janela |
| epoll/UDS | `3 APIs 0.26 CPU / nginx 0.22 CPU` | 3.16ms | 0 | 0 | 0 | 5500.35 | repetir |
| epoll/UDS | `3 APIs 0.26 CPU / nginx 0.22 CPU` | 3.10ms | 0 | 0 | 0 | 5508.86 | melhor do screening |
| epoll/UDS | `2 APIs 0.40 CPU / nginx 0.20 CPU` | 3.15ms | 0 | 0 | 0 | 5501.08 | rejeitado |

Leitura: remover uWebSockets e escrever a resposta HTTP completa manualmente trouxe um ganho pequeno contra o controle ruim da própria janela (`3.23ms -> 3.10ms` no melhor caso), mas não superou a faixa histórica aceita da solução atual (`~2.98-3.05ms`) nem chegou perto do patamar dos líderes (`~1.25-1.50ms`). O resultado mostra que servidor próprio isolado não basta enquanto o parser/vetorização seguem via `simdjson + Payload + strings`; o ganho dos líderes vem do conjunto integrado servidor manual + parser byte-level + vetor `int16` direto + kernel de busca ajustado, não apenas da troca de framework.

Decisão: rejeitado e revertido integralmente. Nenhum arquivo de produção do experimento epoll foi mantido. Se essa linha for retomada, o próximo teste precisa ser estrutural de verdade: parser byte-level direto para `i16[14]` integrado ao servidor, ou adoção controlada de uma base C/io_uring já validada, porque um epoll C++ mantendo o hot path atual não entrega ganho sustentável.

### Experimento rejeitado: índice IVF com `K=256` inspirado no líder C

Hipótese: o líder parcial em C (`https://github.com/thiagorigonatti/rinha-2026`) usa `IVF_CLUSTERS=256` e `IVF_NPROBE=1`. Nossa submissão usa `2048` clusters. Menos clusters reduzem o custo de escolher centróides e avaliar bounding boxes, mas aumentam o tamanho médio de cada cluster; a hipótese era que a geometria `K=256` pudesse reduzir overhead total também no nosso kernel.

Comandos:

```text
./cpp/build/prepare-ivf-cpp /tmp/rinha-2026-official-data/references.json.gz /tmp/rinha-ivf-official-256.bin 256 65536 6
./cpp/build/benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-256.bin 3 0 1 1 1 0 5
./cpp/build/benchmark-ivf-cpp /tmp/rinha-2026-official-run/test-data.json /tmp/rinha-ivf-official-256.bin 3 0 1 1 0 0 5
```

Resultados offline:

| Índice/runtime | ns/query | FP | FN | parse_errors | failure_rate | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `K=256`, `nprobe=1`, bbox repair `0..5` | 192594 | 0 | 0 | 0 | 0% | correto, lento demais |
| `K=256`, `nprobe=1`, sem bbox repair | 68272 | 276 | 267 | 0 | 0.334566% | rejeitado por erro |

Leitura: no nosso layout/repair, `K=256` torna o repair exato caro demais porque cada cluster é muito grande. Desligar o repair reduz o custo bruto, mas introduz `1077` erros ponderados (`276 FP + 3*267 FN`), derrubando o score de detecção muito mais do que qualquer ganho plausível de p99 compensaria. O líder C consegue usar `K=256` porque o restante do stack dele é outro: parser/servidor/io_uring/kernel `int16` manual, não apenas a escolha de clusters.

Decisão: rejeitado sem k6. O índice de produção permanece em `2048` clusters.

### Experimento rejeitado: `cpuset` por container

Hipótese: as soluções Rust de topo usam `cpuset` para reduzir migração e ruído do scheduler, mantendo o limite total de CPU via `cpus`. A máquina local/Docker expõe `16` CPUs, então foi testado pinning isolado sem alterar recursos: `api1 -> CPU 0`, `api2 -> CPU 1`, `api3 -> CPU 2`, `nginx -> CPU 3`.

Validação de configuração:

```text
/perf-noon-tuning-api1-1 cpuset=0 nano=259999984 mem=115343360
/perf-noon-tuning-api2-1 cpuset=1 nano=259999984 mem=115343360
/perf-noon-tuning-api3-1 cpuset=2 nano=259999984 mem=115343360
/perf-noon-tuning-nginx-1 cpuset=3 nano=220000000 mem=20971520
```

Resultado k6:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Stack aceito + `cpuset` | 3.23ms | 0 | 0 | 0 | 5490.94 | rejeitado |

Leitura: pinning manual não reduziu jitter nesta máquina; pelo contrário, fixou cada processo em um core específico e piorou a cauda frente à faixa aceita sem pinning. O benefício visto nas soluções Rust não é portável para esta topologia C++/nginx atual.

Decisão: revertido. O `docker-compose.yml` permanece sem `cpuset`.

### Calibração externa: imagem pública do líder C no ambiente local

Objetivo: validar se o ambiente local reproduz o patamar do ranking parcial antes de perseguir cegamente knobs dos líderes. A imagem pública do líder C (`thiagorigonatti/rinha-2026:0.0.29`, repo `https://github.com/thiagorigonatti/rinha-2026`) foi executada sem alteração de código, com o `docker-compose.yml` do próprio repositório: 2 APIs em C/io_uring, HAProxy, UDS, `seccomp=unconfined`, `K=256`, `IVF_NPROBE=1`, `CANDIDATES=0`.

Observação operacional: rodar diretamente a partir de `/tmp` falhou porque o Docker Desktop não compartilhava o caminho do `haproxy.cfg`. A cópia de calibração foi feita em `~/Desktop/rinha-2026-topc-calibration`, apenas para permitir o bind mount.

Validações:

```text
GET /ready => 200
api1/api2 carregaram index IVF6: N=3000000 K=256 scale=10000.0
engine: IVF/kmeans + int16 + top5 seco + AVX2
```

Resultado k6 local:

| Stack | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `thiagorigonatti/rinha-2026:0.0.29` local | 5.61ms | 0 | 0 | 0 | 5251.14 |

Leitura: a implementação líder não reproduziu localmente o ranking parcial informado (`1.25ms`, `5901.92`) nesta máquina/benchmark, ficando inclusive abaixo da nossa stack C++ atual nas melhores rodadas locais. Isso não invalida a estratégia do líder no ambiente oficial, mas reduz o valor de copiar knobs isolados a partir do compose dele. A conclusão prática para nossa investigação é continuar exigindo validação local por hipótese; ranking externo serve como fonte de ideias, não como prova de ganho transferível.

Decisão: calibração encerrada, stack externa derrubada e nossa stack restaurada com `/ready` 204. Nenhuma mudança de produção.

### Experimento rejeitado: aceitar erro de detecção para reduzir p99 (`nprobe=8` sem reparo)

Hipótese: como o score de latência é logarítmico e o ranking parcial valoriza fortemente `p99` abaixo de `2ms`, poderia valer a pena desligar o reparo exato e aumentar o `nprobe` para reduzir custo de busca, aceitando uma quantidade muito pequena de erros. Este teste foi inspirado nas soluções Rust de topo, que usam busca aproximada com retry seletivo perto da fronteira, mas aqui foi isolado apenas o efeito de `nprobe` sem o reparo por bounding box.

Mudança temporária:

```text
IVF_FAST_NPROBE=8
IVF_FULL_NPROBE=8
IVF_BOUNDARY_FULL=false
IVF_BBOX_REPAIR=false
```

Resultado k6 oficial local:

| Variante | p99 | FP | FN | HTTP errors | failure_rate | p99_score | detection_score | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `nprobe=8`, sem bbox repair | 2.95ms | 6 | 3 | 0 | 0.02% | 2529.60 | 2638.76 | 5168.36 | rejeitado |

Leitura: a latência realmente melhorou em relação ao controle ruim da janela (`3.23ms -> 2.95ms`) e ficou no mesmo patamar das melhores execuções históricas da stack exata. Porém a perda de detecção derrubou o score para `5168.36`, muito abaixo do estado aceito com zero erro (`~5516-5526` local e `5548.91` na submissão anterior). A fórmula penaliza fortemente qualquer erro absoluto mesmo com taxa baixa; neste caso `6 FP + 3 FN` custaram aproximadamente `377` pontos líquidos frente a uma melhora pequena de p99.

Decisão: rejeitado e revertido. Para avançar nessa linha, não basta "aproximar mais"; é necessário um mecanismo de retry seletivo que preserve `0 FP / 0 FN` no dataset oficial local, ou uma queda de p99 muito maior que não apareceu aqui.

### Experimento aceito: poda parcial conservadora no scanner AVX2 do IVF exato

Hipótese: as implementações mais bem ranqueadas consultadas usam scanner vetorial com poda parcial antes de terminar todas as dimensões. Em especial, `joojf/rinha-2026` calcula parte das dimensões e só segue para o restante quando algum lane ainda pode bater o pior top-5; `jairoblatt/rinha-2026-rust` segue a mesma família de estratégia com IVF e retry seletivo. Nosso scanner AVX2 exato fazia sempre as 14 dimensões para todo bloco candidato durante o reparo por bounding box. A hipótese era que uma poda parcial conservadora reduziria CPU sem mudar métrica, índice, desempate ou acurácia.

Mudança implementada:

- Em `cpp/src/ivf.cpp`, `scan_blocks_avx2` agora acumula primeiro 8 dimensões em `uint64`.
- Se o top-5 já tem pior distância finita e todas as 8 lanes do bloco estão estritamente acima desse pior valor parcial, o bloco é descartado.
- Empates e casos iniciais sem top-5 finito continuam pelo caminho completo, preservando o desempate por `id`.
- A implementação evita o atalho `i32` visto em alguns líderes porque, com sentinela `-1` quantizada para `-10000`, a distância máxima teórica pode passar de `INT32_MAX`.

Validação offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Scanner AVX2 anterior | 84241.6 | 0 | 0 | 0 | referência |
| Scanner AVX2 com poda parcial `uint64` | 71099.1 | 0 | 0 | 0 | aceitar para k6 |

Validação k6 oficial local:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| k6 #1 | 3.12ms | 0 | 0 | 0 | 5506.17 |
| k6 #2 | 2.96ms | 0 | 0 | 0 | 5528.47 |

Leitura: o microbenchmark mostrou ganho material de aproximadamente `15.6%` no kernel exato (`84241.6 -> 71099.1 ns/query`) com acurácia perfeita. No k6, o ganho apareceu de forma menos estável porque o p99 também inclui parser, nginx, scheduling e ruído do Docker, mas a segunda rodada atingiu `5528.47`, ligeiramente acima da melhor faixa local anterior (`~5516-5526`) e mantendo `0%` de falhas. A primeira rodada (`5506.17`) ainda ficou dentro da variabilidade ruim da janela, então o ganho deve ser tratado como positivo, porém pequeno.

Decisão: aceito no branch experimental. Próximo passo recomendado: repetir em janela mais limpa antes de promover para `submission`, e investigar uma versão mais agressiva com ordem de dimensões por poder de poda ou parser direto para `i16[14]`.

### Experimento rejeitado: calibrar ponto da poda parcial AVX2

Hipótese: depois que a poda parcial conservadora funcionou com corte em 8 dimensões, o ponto do corte poderia ser ajustado para reduzir ainda mais CPU do scanner. Foram testados pontos de corte no microbenchmark offline mantendo a mesma lógica, índice, métrica e reparo exato.

Resultados offline:

| Corte após N dimensões | ns/query | FP | FN | parse_errors |
|---:|---:|---:|---:|---:|
| 4 | 74596.8 | 0 | 0 | 0 |
| 5 | 69425.6 | 0 | 0 | 0 |
| 6 | 70195.4 | 0 | 0 | 0 |
| 7 | 66337.7-67482.3 | 0 | 0 | 0 |
| 8 | 71099.1 | 0 | 0 | 0 |
| 9 | 67107.0 | 0 | 0 | 0 |

O melhor ponto offline foi `7`, então ele foi levado para k6.

Resultado k6 com corte em 7:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Poda parcial corte 7 | 3.29ms | 0 | 0 | 0 | 5482.29 |

Leitura: apesar do ganho claro no microbenchmark, o corte em 7 piorou a cauda end-to-end. A causa provável é interação de branch/cache/scheduling no container: reduzir alguns ciclos no scanner não garantiu melhor p99, e pode ter aumentado variabilidade na etapa crítica. Como o objetivo efetivo é score local, o resultado offline não é suficiente.

Decisão: rejeitado. O código voltou para o corte em 8 dimensões, que teve melhor evidência k6 (`3.12ms/5506.17` e `2.96ms/5528.47`) nesta janela.

### Experimento rejeitado: reordenar dimensões antes da poda parcial

Hipótese: mantendo a distância final idêntica, processar primeiro dimensões com maior separação aparente poderia aumentar a poda parcial. Foi testada uma ordem priorizando `minutes_since_last_tx`, `km_from_last_tx`, `unknown_merchant`, flags booleanas, `amount`, `amount_vs_avg` e `km_from_home` antes das demais dimensões.

Mudança temporária:

```text
ordem testada: [5, 6, 11, 9, 10, 0, 2, 7, 8, 12, 1, 3, 4, 13]
```

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Ordem natural com corte 8 | 71099.1 | 0 | 0 | 0 | referência |
| Ordem reordenada por sentinelas/flags | 76777.6 | 0 | 0 | 0 | rejeitado |

Leitura: a ordem reordenada preservou a acurácia, mas piorou o kernel. A explicação mais provável é que o layout SoA por dimensão e a distribuição real do dataset favorecem a ordem natural das dimensões iniciais; antecipar flags/sentinelas não compensou o custo de acesso e reduziu a eficiência da poda.

Decisão: rejeitado e revertido. A poda parcial permanece com dimensões `0..7` antes do check.

### Experimento rejeitado: reordenar dimensões no `bbox_lower_bound`

Hipótese: a reordenação de dimensões foi ruim no scanner AVX2, mas poderia ajudar no `bbox_lower_bound`, pois ali a função só precisa ultrapassar `worst` para abortar cedo. A soma final do lower bound permanece idêntica; apenas a ordem da soma foi alterada.

Mudança temporária:

```text
ordem testada no bbox: [5, 6, 11, 9, 10, 0, 2, 7, 8, 12, 1, 3, 4, 13]
```

Resultados:

| Etapa | p99/ns_query | FP | FN | HTTP/parse errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| Offline com bbox reordenado | 67548.5 ns/query | 0 | 0 | 0 | n/a | levar ao k6 |
| k6 com bbox reordenado | 3.45ms | 0 | 0 | 0 | 5461.94 | rejeitado |

Leitura: novamente houve ganho no microbenchmark, mas piora no p99 real. Nesta região do código, o k6 parece mais sensível a variabilidade/cache/branching do que ao `ns/query` médio medido isoladamente.

Decisão: rejeitado e revertido. A ordem natural do `bbox_lower_bound` foi restaurada.

### Experimento rejeitado: trocar nginx stream por HAProxy TCP/UDS

Hipótese: os líderes `thiagorigonatti/rinha-2026` e `jairoblatt/rinha-2026-rust` usam HAProxy com Unix sockets, enquanto nossa stack usa nginx `stream`. Foi testada a troca isolada do load balancer, mantendo as mesmas 3 APIs, os mesmos sockets, a mesma porta `9999`, o mesmo orçamento (`0.22 CPU / 20MB`) e nenhuma lógica de aplicação no LB.

Configuração temporária:

- `haproxy:3.0-alpine`.
- `mode tcp`, `balance roundrobin`, `nbthread 1`, `tune.bufsize 16384`.
- Upstreams `unix@/sockets/api1.sock`, `api2.sock`, `api3.sock`.

Resultados k6:

| LB | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| HAProxy run #1 | 3.04ms | 0 | 0 | 0 | 5517.59 | repetir |
| HAProxy run #2 | 3.21ms | 0 | 0 | 0 | 5494.13 | rejeitado |
| nginx stream aceito | 2.96-3.12ms | 0 | 0 | 0 | 5506.17-5528.47 | manter |

Leitura: HAProxy funcionou corretamente e ficou competitivo, mas não superou nginx no mesmo cenário. A segunda rodada mostrou cauda pior, e a troca adiciona uma mudança estrutural sem ganho sustentado.

Decisão: rejeitado e revertido. O stack volta para nginx `stream`.

### Experimento aceito: redistribuir CPU do classificador para o nginx

Hipótese: depois da poda parcial no scanner AVX2, o gargalo de cauda passou a depender mais do proxy/throttling do que do custo bruto de cada API. A stack aceita usava `3 APIs x 0.26 CPU` e `nginx 0.22 CPU`. Foi testada uma redistribuição mantendo exatamente `1.00 CPU` total e a mesma memória (`3 x 110MB + 20MB`), sem mudar topologia, número de APIs, socket Unix, imagem ou lógica de aplicação.

Validação de limites efetivos via Docker:

```text
/perf-noon-tuning-api1-1 nano=240000000 mem=115343360
/perf-noon-tuning-api2-1 nano=240000000 mem=115343360
/perf-noon-tuning-api3-1 nano=240000000 mem=115343360
/perf-noon-tuning-nginx-1 nano=280000000 mem=20971520
```

Resultados no benchmark oficial local atualizado (`/tmp/rinha-2026-official-run/test.js`, `54.100` entradas, alvo `900 RPS`):

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `api=0.25`, `nginx=0.25` | 2.99ms | 0 | 0 | 0 | 5523.66 | pior que o melhor aceito |
| `api=0.24`, `nginx=0.28` run #1 | 2.87ms | 0 | 0 | 0 | 5541.51 | repetir |
| `api=0.24`, `nginx=0.28` run #2 | 2.98ms | 0 | 0 | 0 | 5526.32 | aceitar |

Nota de método: uma execução acidental de `./run.sh` foi descartada para decisão porque ela usa a massa local menor de `14.500` entradas, não o benchmark oficial local atualizado usado nesta rodada. O resultado não é comparável com os números acima.

Leitura: o ganho é pequeno, mas a alteração é sustentável: preserva `0 FP`, `0 FN`, `0 HTTP`, mantém a soma exata de `1.00 CPU / 350MB`, e melhora o melhor resultado observado da stack atual (`5528.47 -> 5541.51`) sem introduzir complexidade. A segunda execução ficou praticamente empatada com o melhor aceito anterior, então a decisão é aceitar como ajuste de recurso de baixo risco, não como salto estrutural.

Decisão: aceito no branch experimental. O `docker-compose.yml` passa a usar `0.24 CPU` por API e `0.28 CPU` para o nginx.

### Experimento aceito: reduzir de 3 APIs para 2 APIs mais fortes

Hipótese: a exigência oficial é `>= 2` APIs, não exatamente 3. Com o classificador ficando menos pesado depois da poda parcial e com o nginx pedindo mais fatia de CPU, poderia ser melhor reduzir uma instância de API, diminuir a contenção no volume de sockets e dar mais CPU/memória a cada backend. Essa linha também conversa com os líderes C/Rust consultados, que tendem a usar poucas instâncias fortes em vez de pulverizar o orçamento.

Mudança temporária:

```text
api1/api2: 0.36 CPU, 165MB cada
nginx:     0.28 CPU, 20MB
total:     1.00 CPU, 350MB
upstream:  2 sockets Unix em round-robin
```

Validação de limites efetivos via Docker:

```text
/perf-noon-tuning-api1-1 nano=360000000 mem=173015040
/perf-noon-tuning-api2-1 nano=360000000 mem=173015040
/perf-noon-tuning-nginx-1 nano=280000000 mem=20971520
```

Resultados no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `2 APIs x 0.36` + `nginx 0.28` run #1 | 2.92ms | 0 | 0 | 0 | 5534.12 | repetir |
| `2 APIs x 0.36` + `nginx 0.28` run #2 | 2.92ms | 0 | 0 | 0 | 5534.57 | aceitar |

Leitura: a variante com 2 APIs não superou o melhor single-run de 3 APIs (`5541.51`), mas foi mais estável do que a repetição de 3 APIs (`5526.32`) e ficou acima do melhor aceito anterior à redistribuição (`5528.47`). Como a topologia continua 100% conforme o regulamento (`LB + 2 APIs`, sem lógica no LB, bridge, 1 CPU/350MB) e reduz a quantidade de processos disputando scheduler, ela é um candidato melhor para o estado experimental atual.

Decisão: aceito no branch experimental. O melhor single-run do dia permanece `5541.51` com 3 APIs, mas o estado atual passa a ser 2 APIs por estabilidade local.

### Experimento aceito: calibrar CPU entre 2 APIs e nginx

Hipótese: com apenas 2 APIs, o gargalo poderia pender para o nginx ou para as APIs. Foram testados dois deslocamentos simétricos mantendo a mesma topologia de 2 APIs, a mesma memória e o mesmo total exato de `1.00 CPU`.

Validações de limites efetivos:

```text
api=0.37 x2, nginx=0.26:
/perf-noon-tuning-api1-1 nano=370000000 mem=173015040
/perf-noon-tuning-api2-1 nano=370000000 mem=173015040
/perf-noon-tuning-nginx-1 nano=259999984 mem=20971520

api=0.35 x2, nginx=0.30:
/perf-noon-tuning-api1-1 nano=350000000 mem=173015040
/perf-noon-tuning-api2-1 nano=350000000 mem=173015040
/perf-noon-tuning-nginx-1 nano=300000000 mem=20971520
```

Resultados no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `api=0.37 x2`, `nginx=0.26` | 2.92ms | 0 | 0 | 0 | 5534.69 | rejeitado como marginal |
| `api=0.35 x2`, `nginx=0.30` run #1 | 2.85ms | 0 | 0 | 0 | 5545.37 | repetir |
| `api=0.35 x2`, `nginx=0.30` run #2 | 2.90ms | 0 | 0 | 0 | 5537.88 | aceitar |

Leitura: tirar CPU do nginx praticamente não mudou o resultado (`5534.57 -> 5534.69`) e ficou dentro de ruído. Dar mais CPU ao nginx, por outro lado, produziu o melhor single-run da rodada (`5545.37`) e uma confirmação ainda acima das variantes de 2 APIs anteriores. O sinal reforça que, no estado atual, a cauda é mais sensível a proxy/throttling/scheduling do que a uma pequena fatia extra de CPU no classificador.

Decisão: aceito no branch experimental. O `docker-compose.yml` passa a usar `api1/api2=0.35 CPU, 165MB` e `nginx=0.30 CPU, 20MB`.

### Experimento rejeitado: empurrar nginx para `0.32 CPU`

Hipótese: como `nginx=0.30` melhorou a cauda, talvez ainda houvesse ganho deslocando mais CPU do backend para o proxy. Foi testado `api=0.34 x2` e `nginx=0.32`, mantendo `1.00 CPU / 350MB`.

Validação de limites efetivos:

```text
/perf-noon-tuning-api1-1 nano=340000000 mem=173015040
/perf-noon-tuning-api2-1 nano=340000000 mem=173015040
/perf-noon-tuning-nginx-1 nano=320000000 mem=20971520
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `api=0.34 x2`, `nginx=0.32` | 2.96ms | 0 | 0 | 0 | 5528.70 | rejeitado |

Leitura: o ponto `0.32` passou do ótimo local. A cauda voltou para a faixa antiga e apareceu um pico transitório de VUs perto do fim da run, compatível com backend mais apertado. Isso indica que o nginx precisava de mais CPU que `0.28`, mas tirar mais do que `0.30` das APIs degrada o equilíbrio.

Decisão: rejeitado e revertido. O estado aceito volta para `api=0.35 x2` e `nginx=0.30`.

### Experimento rejeitado: acumulador AVX2 `i32` no scan IVF

Hipótese: os repositórios líderes consultados usam kernels AVX2 com distâncias acumuladas em registradores de 32 bits para reduzir custo por bloco. O nosso kernel usa acumuladores de 64 bits e faz prune parcial após as 8 primeiras dimensões. Foi testada uma versão temporária em `cpp/src/ivf.cpp` que substituía o par `lo/hi` de `u64` por um acumulador AVX2 `i32` e inseria as 8 lanes diretamente no top-5.

Fontes cruzadas:

```text
thiagorigonatti/rinha-2026: C + io_uring + HAProxy + IVF256 + int16 + AVX2
jairoblatt/rinha-2026-rust: Rust + monoio/io_uring + HAProxy + IVF + int16 + AVX2
joojf/rinha-2026: Rust + monoio/io_uring + nginx + UDS + int16 + AVX2
```

Resultado offline com o dataset oficial local atualizado (`54.100` payloads, índice `3.000.000` refs, `bbox_repair=true`, `0 FP/FN`):

| Variante | ns/query | FP | FN | checksum | Decisão |
|---|---:|---:|---:|---:|---|
| Kernel aceito `u64 + prune parcial` | 67016.3 | 0 | 0 | 92435214 | manter |
| Kernel temporário `i32 sem prune parcial` | 67838.5 | 0 | 0 | 92435214 | rejeitar |

Leitura: a hipótese é correta para alguns líderes, mas não para o nosso layout atual. A versão `i32` preservou a classificação, porém perdeu ~1,2% offline. O custo menor do acumulador não compensou a remoção do prune parcial após 8 dimensões, que evita calcular as 6 dimensões restantes em blocos sem chance de entrar no top-5.

Decisão: rejeitado e revertido. O kernel aceito continua sendo `u64 + prune parcial`.

### Experimento rejeitado: prefetch manual no scan de blocos IVF

Hipótese: o scanner do `joojf/rinha-2026` faz `_mm_prefetch` de blocos futuros antes de calcular a distância do bloco atual. Como nosso layout de bloco também tem `8 lanes x 14 dimensões`, foi testado o mesmo padrão com prefetch de `block + 8` em duas linhas do bloco (`base` e `base + 7*lanes`).

Resultado offline com o dataset oficial local atualizado (`54.100` payloads, índice `3.000.000` refs, `bbox_repair=true`, `0 FP/FN`):

| Variante | ns/query | FP | FN | checksum | Decisão |
|---|---:|---:|---:|---:|---|
| Kernel aceito sem prefetch manual | 67016.3 | 0 | 0 | 92435214 | manter |
| Kernel temporário com prefetch `block+8` | 67236.4 | 0 | 0 | 92435214 | rejeitar |

Leitura: o acesso do nosso scanner já está suficientemente sequencial para o prefetcher de hardware. O prefetch manual adicionou instruções e piorou levemente o microbenchmark (~0,3%). Como o k6 é mais ruidoso que esse delta, não há justificativa para levar a hipótese para benchmark completo.

Decisão: rejeitado e revertido. O scan permanece sem prefetch manual.

### Experimento rejeitado: remover `reuseport` do nginx

Hipótese: com `worker_processes 1`, o `reuseport` no `listen` poderia ser neutro ou até adicionar ruído desnecessário. Foi testado `listen 9999 backlog=4096;` mantendo todo o restante do estado aceito (`api=0.35 x2`, `nginx=0.30`, 2 APIs, UDS).

Validação operacional:

```text
GET /ready => 204
nginx recriado apenas com listen sem reuseport
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| nginx sem `reuseport` | 2.99ms | 0 | 0 | 0 | 5524.53 | rejeitar |

Leitura: mesmo com apenas um worker, `reuseport` não é prejudicial no nosso cenário. A remoção piorou a cauda em relação ao estado aceito (`p99 2.85-2.90ms`, `5537.88-5545.37`), sem qualquer ganho de detecção.

Decisão: rejeitado e revertido. O nginx voltou para `listen 9999 reuseport backlog=4096;`.

### Experimento rejeitado: `ulimits nofile` + `somaxconn`

Hipótese: os repositórios líderes usam `nofile=65535` e, em alguns casos, `net.core.somaxconn=4096` no LB. Como o teste oficial local usa carga incremental até `900 RPS`, a fila de accept/FD poderia reduzir cauda sob pico.

Configuração temporária:

```text
api1/api2:
  ulimits nofile soft/hard 65535

nginx:
  ulimits nofile soft/hard 65535
  sysctls net.core.somaxconn=4096
```

Validação operacional:

```text
GET /ready => 204
/perf-noon-tuning-api1-1 nano=350000000 mem=173015040 ulimits=[nofile 65535]
/perf-noon-tuning-api2-1 nano=350000000 mem=173015040 ulimits=[nofile 65535]
/perf-noon-tuning-nginx-1 nano=300000000 mem=20971520 ulimits=[nofile 65535] sysctls={"net.core.somaxconn":"4096"}
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `nofile=65535` + `somaxconn=4096` | 3.11ms | 0 | 0 | 0 | 5507.49 | rejeitar |

Leitura: o teste não está limitado por FD/backlog no nosso stack atual. A mudança piorou a cauda em vez de reduzir, provavelmente porque o gargalo real continua sendo custo de processamento/proxy sob limite de CPU e não fila de conexões.

Decisão: rejeitado e revertido. O compose voltou sem `ulimits` e sem `sysctls`.

### Experimento rejeitado: nginx HTTP upstream por request

Hipótese: o nosso nginx `stream` faz balanceamento L4 por conexão. Como o k6 usa keep-alive, parte da carga pode ficar concentrada em menos conexões e reduzir a igualdade entre APIs. Repositórios líderes usam balanceamento HTTP ou servidores que aceitam mais diretamente o custo de request-level routing. Foi testado nginx em modo `http` com upstream UDS e `keepalive 256`, sem inspecionar payload nem aplicar lógica de negócio.

Configuração temporária:

```nginx
http {
    upstream api {
        server unix:/sockets/api1.sock;
        server unix:/sockets/api2.sock;
        keepalive 256;
    }

    server {
        listen 9999 reuseport backlog=4096;
        keepalive_timeout 75s;
        keepalive_requests 100000;

        location / {
            proxy_pass http://api;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_buffering off;
        }
    }
}
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| nginx HTTP upstream por request | 3.09ms | 0 | 0 | 0 | 5510.41 | rejeitar |

Leitura: a hipótese é válida em tese, mas no nosso stack o overhead HTTP do proxy supera qualquer ganho de balanceamento por request. O `stream` L4 por UDS continua mais eficiente.

Decisão: rejeitado e revertido. O nginx voltou para `stream` com upstream UDS e `listen 9999 reuseport backlog=4096`.

### Experimento rejeitado: split fino `api=0.355`, `nginx=0.29`

Hipótese: depois do melhor estado aceito em `api=0.35 x2 / nginx=0.30`, havia uma dúvida se o ponto ótimo estaria ligeiramente deslocado para mais CPU nas APIs e menos no nginx. Foi testado o split `0.355 + 0.355 + 0.29 = 1.00 CPU`.

Validação de limites efetivos:

```text
/perf-noon-tuning-api1-1 nano=355000000 mem=173015040
/perf-noon-tuning-api2-1 nano=355000000 mem=173015040
/perf-noon-tuning-nginx-1 nano=290000000 mem=20971520
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `api=0.355 x2`, `nginx=0.29` | 3.05ms | 0 | 0 | 0 | 5516.40 | rejeitar |

Leitura: reduzir o nginx de `0.30` para `0.29` piorou a cauda mesmo devolvendo CPU às APIs. O limite aceito de `0.30` para nginx parece ser o menor patamar sustentável no stack atual.

Decisão: rejeitado e revertido. O estado aceito permanece `api=0.35 x2`, `nginx=0.30`.

### Validação de estado aceito após a rodada de reversões

Depois dos experimentos temporários de nginx/compose, o stack foi restaurado para o estado aceito:

```text
api1/api2: 0.35 CPU, 165MB
nginx:     0.30 CPU, 20MB
nginx:     stream + UDS + reuseport
GET /ready => 204
```

Resultado de validação no benchmark oficial local atualizado:

| Estado | p99 | FP | FN | HTTP errors | final_score | Leitura |
|---|---:|---:|---:|---:|---:|---|
| aceito restaurado após sequência de testes | 3.02ms | 0 | 0 | 0 | 5520.34 | drift de ambiente |

Leitura: como não havia diff pendente de código/compose e a detecção permaneceu perfeita, esse número não invalida o estado aceito anterior (`2.85-2.90ms`, `5537.88-5545.37`). A rodada foi feita após vários k6 consecutivos e recriações de containers; portanto a interpretação correta é ruído/deriva térmica ou de scheduler da máquina local, não regressão funcional.

### Experimento rejeitado: `q_grid` quantizado para seleção de centróides

Hipótese: o C líder quantiza a query e usa `q_grid = q_i16 / scale` também para selecionar os centróides IVF. O nosso código selecionava centróides com o `QueryVector` float original e só usava `i16` no scan. Alinhar a seleção de centróides com o espaço quantizado poderia reduzir pequena divergência entre fase de probe e fase de scan.

Resultado offline com o dataset oficial local atualizado:

| Variante | ns/query | FP | FN | checksum | Leitura |
|---|---:|---:|---:|---:|---|
| seleção com `QueryVector` float original | 67016.3 | 0 | 0 | 92435214 | baseline |
| seleção com `q_grid` quantizado | 66587.7 | 0 | 0 | 92435214 | ganho offline |

Resultado no benchmark oficial local atualizado após rebuild da imagem:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `q_grid` quantizado | 3.20ms | 0 | 0 | 0 | 5494.45 | rejeitar |

Leitura: a hipótese é tecnicamente interessante e melhorou o microbenchmark em ~0,6%, mas não reproduziu no benchmark completo. Como o objetivo desta fase é melhoria sustentável e inquestionável, o resultado offline isolado não é suficiente para aceitar a mudança.

Decisão: rejeitado e revertido. O código voltou a selecionar centróides com o `QueryVector` float original. A imagem local foi rebuildada e o stack respondeu `GET /ready => 204` no estado restaurado.

### Achado de pesquisa: micro-otimizações sustentáveis estão praticamente esgotadas no stack atual

Fontes consultadas nesta rodada:

```text
https://github.com/thiagorigonatti/rinha-2026
https://github.com/jairoblatt/rinha-2026-rust
https://github.com/joojf/rinha-2026
https://github.com/zanfranceschi/rinha-de-backend-2026/tree/main/docs/br
https://rinhadebackend.com.br/
```

Síntese técnica dos líderes com pontuação acima da nossa:

| Referência | Stack | Elementos decisivos observados |
|---|---|---|
| `thiagorigonatti/rinha-2026` | C + `io_uring` + HAProxy HTTP + UDS | servidor HTTP manual, respostas HTTP pré-montadas, parser mínimo, IVF int16, AVX2, bbox repair |
| `jairoblatt/rinha-2026-rust` | Rust + `monoio`/io_uring + HAProxy TCP + UDS | runtime io_uring, parser manual, índice embutido/binário, AVX2, 2 APIs |
| `joojf/rinha-2026` | Rust + `monoio`/io_uring + nginx HTTP + UDS | parser manual direto para `i16`, respostas pré-montadas, nginx com keepalive upstream, AVX2 |

Leitura depois dos experimentos negativos de hoje:

```text
Não reproduziram ganho:
- HAProxy TCP isolado
- nginx HTTP upstream isolado
- remover reuseport
- ulimits/somaxconn
- worker_processes=2
- cpuset
- splits finos de CPU fora de api=0.35/nginx=0.30
- prefetch manual no IVF
- acumulador AVX2 i32 sem prune parcial
- q_grid quantizado apesar de ganho offline
```

Conclusão: o stack C++/uWebSockets já está no limite do que parece extraível por ajuste local. A diferença para o topo (`~1.25-1.50ms`) não deve vir de mais um knob de nginx ou de uma microtroca no kernel, mas de reduzir o overhead estrutural do caminho HTTP: servidor manual/io_uring ou runtime `monoio`, parser manual que vetoriza direto, e respostas HTTP completas pré-montadas.

Próximos experimentos assertivos para nova rodada:

1. Implementar um servidor HTTP manual mínimo em C/C++ usando UDS + `io_uring`, reaproveitando o IVF atual e medindo apenas troca de servidor.
2. Se o item 1 for caro demais, portar primeiro o handler para um parser manual que gere `QueryVector`/`i16` sem `simdjson::dom` e sem `std::string` no hot path, mas só aceitar se o k6 reproduzir.
3. Rodar nova bateria somente após cooldown da máquina, porque a validação final do estado aceito caiu de `2.85-2.90ms` para `3.02ms` sem diff, sinal forte de drift local após cargas consecutivas.

### Escopo técnico do próximo experimento estrutural

A leitura do servidor C líder mostrou que o próximo experimento não deve ser "trocar uWebSockets por qualquer coisa", e sim substituir o caminho HTTP inteiro por um servidor mínimo mensurável:

```text
socket UDS não bloqueante
io_uring para ACCEPT/READ/WRITE
pool fixa de conexões
buffer fixo por conexão
parser HTTP mínimo:
  - localizar \r\n\r\n
  - reconhecer GET /ready
  - reconhecer POST /fraud-score
  - ler Content-Length
  - passar body ao vetorizador
respostas HTTP completas pré-montadas para score 0..5
reuso de conexão keep-alive
```

Partes que podem ser reaproveitadas do código atual:

```text
cpp/src/ivf.cpp
cpp/include/rinha/ivf.hpp
cpp/src/vectorize.cpp inicialmente, até substituir por parser manual
Dockerfile / prepare-ivf-cpp / index.bin
docker-compose.yml com 2 APIs + nginx stream + UDS
```

Critério de aceite para essa próxima etapa:

```text
1. core_tests ou teste equivalente de contrato HTTP passando
2. benchmark offline sem FP/FN
3. k6 oficial local atualizado com 0% falhas
4. duas runs consecutivas acima do melhor estado aceito atual, não só um outlier
```

Leitura: sem esse critério, há risco alto de trocar framework, aumentar complexidade e ganhar apenas ruído. O objetivo da próxima rodada deve ser provar que o overhead restante é do servidor HTTP, não do IVF.

### Experimento rejeitado: `worker_processes 2` no nginx

Hipótese: com `nginx=0.30 CPU`, dois workers poderiam distribuir melhor aceitações/conexões e reduzir cauda do proxy, principalmente com `listen ... reuseport`. A mudança foi isolada em `nginx.conf`, mantendo 2 APIs, sockets Unix e o mesmo orçamento de recursos.

Validação operacional:

```text
GET /ready => 204
processos nginx no container => 3 (master + 2 workers)
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `worker_processes 2` | 2.87ms | 0 | 0 | 0 | 5542.10 | rejeitado como não material |

Leitura: o resultado é bom, mas não supera claramente o estado aceito com 1 worker (`5545.37` no melhor, `5537.88` na confirmação). Como dois workers adicionam mais disputa de scheduler dentro de apenas `0.30 CPU` de nginx e não trouxeram ganho inquestionável, a mudança não merece entrar no estado atual.

Decisão: rejeitado e revertido. O nginx volta para `worker_processes 1`.

### Experimento rejeitado: HAProxy no novo ponto `2 APIs + LB 0.30`

Hipótese: HAProxy havia sido rejeitado em `3 APIs + LB 0.22`, mas os líderes C/Rust consultados usam HAProxy ou runtimes L4 enxutos, e a rodada de CPU mostrou que o LB precisava de mais orçamento. Por isso o teste foi repetido em condição mais justa: 2 APIs, LB com `0.30 CPU`, sockets Unix e `1.00 CPU / 350MB`.

Configuração temporária:

```text
haproxy:3.0-alpine
mode tcp
balance roundrobin
nbthread 1
tune.bufsize 16384
upstreams: unix@/sockets/api1.sock, unix@/sockets/api2.sock
```

Validação operacional:

```text
GET /ready => 204
nginx service image => haproxy:3.0-alpine
api1/api2 => 0.35 CPU, 165MB
LB => 0.30 CPU, 20MB
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| HAProxy `2 APIs + 0.30 CPU` | 2.97ms | 0 | 0 | 0 | 5527.24 | rejeitado |

Leitura: mesmo com mais CPU para o LB, HAProxy ficou abaixo do nginx no estado atual (`nginx 0.35/0.30`: `5545.37` melhor run, `5537.88` confirmação). O ganho dos líderes que usam HAProxy não vem do HAProxy isolado; ele depende do restante do stack (`io_uring`/parser manual/index/kernel).

Decisão: rejeitado e revertido. O stack volta para `nginx:1.27-alpine` com `worker_processes 1`.

### Experimento rejeitado: `cpuset` no arranjo de 2 APIs

Hipótese: `cpuset` havia sido ruim no arranjo anterior de 3 APIs, mas poderia funcionar melhor com apenas 2 APIs e um nginx mais forte, reduzindo migração de processos e ruído de scheduler. Foi testado pinning simples e isolado:

```text
api1  -> CPU 0
api2  -> CPU 1
nginx -> CPU 2
```

Validação de limites efetivos:

```text
/perf-noon-tuning-api1-1 cpuset=0 nano=350000000 mem=173015040
/perf-noon-tuning-api2-1 cpuset=1 nano=350000000 mem=173015040
/perf-noon-tuning-nginx-1 cpuset=2 nano=300000000 mem=20971520
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `cpuset` em 2 APIs + nginx | 2.96ms | 0 | 0 | 0 | 5528.23 | rejeitado |

Leitura: o pinning manual voltou a piorar a cauda. A limitação por `NanoCpus` parece interagir melhor com o scheduler do Docker quando os processos podem migrar, em vez de ficarem fixos em três CPUs específicas.

Decisão: rejeitado e revertido. O `docker-compose.yml` permanece sem `cpuset`.

### Experimento rejeitado: ponto intermediário `api=0.345`, `nginx=0.31`

Hipótese: como `api=0.35/nginx=0.30` foi bom e `api=0.34/nginx=0.32` piorou, o ponto intermediário poderia ser o melhor equilíbrio fino entre backend e proxy.

Validação de limites efetivos:

```text
/perf-noon-tuning-api1-1 nano=345000000 mem=173015040
/perf-noon-tuning-api2-1 nano=345000000 mem=173015040
/perf-noon-tuning-nginx-1 nano=310000000 mem=20971520
```

Resultados no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `api=0.345 x2`, `nginx=0.31` run #1 | 2.85ms | 0 | 0 | 0 | 5545.86 | repetir |
| `api=0.345 x2`, `nginx=0.31` run #2 | 3.15ms | 0 | 0 | 0 | 5502.10 | rejeitar |

Leitura: o primeiro resultado foi o melhor single-run do dia por margem mínima, mas não reproduziu. A queda para `5502.10` na repetição torna o ponto instável demais para aceitar. Como a diferença positiva era menor que 1 ponto e a regressão foi grande, o resultado deve ser tratado como outlier.

Decisão: rejeitado e revertido. O estado aceito volta para `api=0.35 x2` e `nginx=0.30`.

### Rodada 18h-20h: investigação estrutural e parser seletivo

Contexto: após a rodada anterior, o estado aceito permanecia melhor como `2 APIs + nginx stream + UDS`, com `api=0.35 CPU` por instância e `nginx=0.30 CPU`. A pontuação oficial já registrada no ranking para `viniciusdsandrade (andrade-cpp-ivf)` segue sendo o melhor referencial externo desta branch:

```text
p99=2.83ms
failure_rate=0%
final_score=5548.91
ranking parcial em 2026-05-01 10:51:16
```

Investigação inicial:

```text
branch=perf/noon-tuning
estado inicial=limpo
liburing local via pkg-config=indisponível
histórico reaproveitável de servidor epoll/io_uring em cpp/src=não encontrado na branch atual
```

Leitura: `io_uring` continua sendo a hipótese estrutural de maior teto, mas não é uma troca barata neste ponto. Exige introduzir dependência de build/runtime, servidor HTTP próprio, buffers fixos, respostas HTTP completas e validação cuidadosa de keep-alive. Por isso, antes de abrir esse bloco grande, foram testadas duas hipóteses menores e reversíveis no hot path atual.

### Experimento rejeitado: parser seletivo manual em `request.cpp`

Hipótese: substituir `simdjson::dom` por um parser seletivo do payload oficial poderia reduzir heap/cópias no hot path. A versão testada removia o vetor de merchants, extraía apenas os campos usados na vetorização e mantinha o mesmo contrato de `Payload`.

Validação:

```text
cmake --build cpp/build --target test benchmark-request-cpp -j8
ctest via target test => 100% passed
```

Resultados offline:

| Variante | parse_payload | parse_vectorize | parse_classify | Decisão |
|---|---:|---:|---:|---|
| parser manual inicial | 1797.31 ns/query | 2172.46 ns/query | 31944.5 ns/query | corrigir alocação de chave |
| parser manual sem string temporária de chave | 1611.51 ns/query | 1668.13 ns/query | 30506.8 ns/query | rejeitar |
| baseline histórico `simdjson::dom` | ~629.99 ns/query | ~696.48 ns/query | ~28859.4 ns/query | manter |

Leitura: o parser manual ingênuo perdeu para `simdjson::dom`. A troca só faz sentido se for um parser direto para `QueryVector`/representação quantizada, sem preencher `Payload`, sem `std::from_chars` genérico por campo e sem varreduras repetidas por chave. Como esta versão aumentou o custo offline antes mesmo do k6, ela foi rejeitada sem rodar benchmark HTTP.

Decisão: rejeitado e revertido. `cpp/src/request.cpp` voltou ao parser `simdjson::dom`.

### Experimento rejeitado: remover `Content-Type` e `cork` na resposta

Hipótese: o k6 faz `JSON.parse(res.body)` e não depende de `Content-Type`; portanto remover `writeHeader("Content-Type", "application/json")` e `res->cork(...)` poderia reduzir trabalho por resposta no caminho HTTP.

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp test -j8
ctest via target test => 100% passed
docker compose build api1
docker compose up -d --force-recreate
GET /ready => 204
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| sem `Content-Type`/`cork` | 3.24ms | 0 | 0 | 0 | 5488.85 | rejeitado |

Leitura: o `cork`/header não é o gargalo e provavelmente ajuda a agrupar header/body no uWebSockets. A alteração reduziu trabalho aparente no código, mas piorou a cauda de forma clara.

Decisão: rejeitado e revertido. O stack voltou a responder com `Content-Type: application/json` dentro de `res->cork(...)`.

Estado após a rodada:

```text
source diff funcional=nenhum
imagem Docker reconstruída no estado aceito
docker compose up -d --force-recreate
GET /ready => 204
```

Próxima hipótese com maior chance real: implementar um servidor manual separado e opcional, começando por um alvo isolado (`rinha-backend-2026-cpp-manual`) em vez de alterar o binário aceito. O critério mínimo para continuar esse caminho é compilar localmente, passar contrato HTTP, manter 0 FP/FN e mostrar k6 acima do estado aceito em duas runs consecutivas. Sem isso, a troca de servidor vira complexidade sem ganho sustentável.

### Experimento rejeitado: reduzir `LIBUS_RECV_BUFFER_LENGTH`

Hipótese: o `uSockets` vendorizado define `LIBUS_RECV_BUFFER_LENGTH=524288` por padrão. Como os payloads reais do teste ficam em torno de `435 bytes` em média e `472 bytes` no máximo observado pelo benchmark offline, um buffer global de 512KB poderia gerar pressão desnecessária de cache/memória. Foi testado `LIBUS_RECV_BUFFER_LENGTH=16384` no target `usockets`.

Validação:

```text
cmake --build cpp/build --target test rinha-backend-2026-cpp -j8
ctest via target test => 100% passed
docker compose build api1
docker compose up -d --force-recreate
GET /ready => 204
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `LIBUS_RECV_BUFFER_LENGTH=16384` | 3.37ms | 0 | 0 | 0 | 5472.28 | rejeitado |

Leitura: reduzir o buffer piorou a cauda. O default de 512KB não parece ser gargalo mensurável no nosso caminho, ou o `uSockets` se beneficia do buffer maior no fluxo interno de leitura. Esta classe de tuning interno do framework é menos promissora que trocar o servidor inteiro por um caminho manual.

Decisão: rejeitado e revertido. `cpp/CMakeLists.txt` voltou a definir apenas `LIBUS_NO_SSL` para `usockets`; imagem Docker reconstruída no estado aceito e `/ready` validado em `204`.

### Experimento rejeitado: `seccomp=unconfined`

Hipótese: o stack C líder usa `security_opt: seccomp=unconfined`, principalmente para viabilizar `io_uring`. Mesmo sem ativar `io_uring`, remover o filtro seccomp poderia reduzir overhead de syscalls no caminho Docker/nginx/API. A mudança foi testada nos dois serviços de API e no nginx, sem alterar CPU, memória, rede ou binário.

Validação:

```text
docker compose config => security_opt aplicado em api1, api2 e nginx
docker compose up -d --force-recreate
GET /ready => 204
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `seccomp=unconfined` | 3.05ms | 0 | 0 | 0 | 5516.04 | rejeitado |

Leitura: a mudança não superou o estado aceito e adiciona uma opção de segurança que só se justifica quando necessária para `io_uring`. Como o ganho não apareceu no stack epoll/uWebSockets, ela não deve entrar na submissão.

Decisão: rejeitado e revertido. O compose voltou sem `security_opt`; `docker compose config` confirmou a remoção e `/ready` voltou `204` após recriação.

### Experimento rejeitado: `proxy_buffer_size 1k` no nginx stream

Hipótese: a documentação oficial do nginx stream proxy indica `proxy_buffer_size` com default de `16k`; como o payload e a resposta desta API são pequenos, reduzir para `1k` poderia diminuir pressão de memória/cache no load balancer sem alterar semântica de proxy TCP.

Fonte consultada: https://nginx.org/en/docs/stream/ngx_stream_proxy_module.html

Validação:

```text
docker compose up -d --force-recreate nginx
docker compose exec -T nginx nginx -t => ok
GET /ready => 204
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `proxy_buffer_size 1k` | 2.97ms | 0 | 0 | 0 | 5526.58 | rejeitado |

Leitura: o resultado ficou bom dentro do drift local recente, mas não superou a submissão oficial parcial (`2.83ms`, `5548.91`) nem o melhor estado aceito local. A alteração também adiciona mais um knob de infraestrutura sem evidência clara de benefício.

Observação operacional: após editar `nginx.conf` via patch, o container do nginx chegou a validar um arquivo truncado por causa do bind mount sobre inode antigo. A validação correta exigiu recriar o serviço (`docker compose up -d --force-recreate nginx`) antes de rodar `nginx -t`.

Decisão: rejeitado e revertido. O nginx voltou sem `proxy_buffer_size` explícito.

### Experimento rejeitado: `known_merchants` inline com `string_view`

Hipótese: manter `simdjson`, mas remover alocações evitáveis na comparação de `customer.known_merchants` contra `merchant.id`. O `test/test-data.json` local tem no máximo 5 merchants conhecidos por payload; por isso foi testado armazenamento inline com `std::array<std::string_view, 8>` e fallback para overflow, preservando a API e o restante da vetorização.

Investigação prévia:

```text
jq '[.entries[].request.customer.known_merchants | length] | max' test/test-data.json => 5
cmake --build cpp/build --target test benchmark-request-cpp -j8 => tests 100% passed
```

Microbenchmark offline:

| Variante | parse_payload | parse_vectorize | parse_classify |
|---|---:|---:|---:|
| baseline histórico simdjson | ~630ns | ~696ns | ~28.9us |
| `string_view` inline run 1 | 575.56ns | 605.84ns | 29.17us |
| `string_view` inline run 2 | 572.05ns | 633.31ns | 30.25us |

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `known_merchants` inline com `string_view` | 4.71ms | 123 | 117 | 0 | 3003.62 | rejeitado |

Leitura: o microbenchmark indicou ganho de parser, mas o k6 revelou alteração semântica severa. A causa mais provável é que as `std::string_view` obtidas do `simdjson::dom::element::get(std::string_view&)` não são uma substituição segura para as strings materializadas neste fluxo, porque a área interna usada para strings pode ser reusada/invalidadada conforme outros campos são acessados. Isso afeta diretamente `known_merchant`, altera a dimensão 11 e gera FP/FN.

Decisão: rejeitado e revertido. `cpp/src/request.cpp` voltou a materializar `known_merchants` e `merchant_id` como `std::string`; `cmake --build cpp/build --target test -j8` voltou a passar 100%.

### Experimento rejeitado: parser `simdjson::ondemand`

Hipótese: manter `simdjson`, mas trocar apenas a API DOM por On Demand para evitar materialização da árvore intermediária. A implementação experimental preservou as cópias para `std::string` nos campos usados após o parse e usou `find_field` na ordem natural do payload oficial.

Fonte local consultada: `cpp/third_party/simdjson/README.md` e comentários de API em `cpp/third_party/simdjson/singleheader/simdjson.h`, especialmente as notas sobre `ondemand::parser::iterate`, lifetime do buffer e consumo de campos em ordem.

Validação pré-k6:

```text
cmake --build cpp/build --target test benchmark-request-cpp -j8 => tests 100% passed
GET /ready => 204
```

Microbenchmark offline:

| Variante | parse_payload | parse_vectorize | parse_classify |
|---|---:|---:|---:|
| baseline histórico DOM | ~630ns | ~696ns | ~28.9us |
| On Demand run 1 | 469.77ns | 540.17ns | 30.46us |
| On Demand run 2 | 1100.68ns | 623.31ns | 29.09us |
| On Demand run 3 (`repeat=20`) | 479.12ns | 536.46ns | 28.89us |

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `simdjson::ondemand` | 4.47ms | 123 | 117 | 0 | 3027.12 | rejeitado |

Leitura: o On Demand parecia promissor no microbenchmark, mas falhou no critério que importa: preservação semântica no stack completo. A repetição do mesmo padrão de `123 FP / 117 FN` visto no experimento anterior indica que a troca de API do parser alterou alguma dimensão sensível, provavelmente por consumo/ordem/lifetime em objetos aninhados ou por diferença na extração de strings. O teste unitário atual e o checksum agregado do microbenchmark não são suficientes para capturar essa classe de divergência.

Decisão: rejeitado e revertido. O parser DOM atual permanece, porque é mais robusto semanticamente no dataset oficial local. Antes de qualquer nova tentativa nessa linha, o próximo pré-requisito é criar um comparador payload-a-payload de vetor DOM vs parser candidato, falhando na primeira dimensão divergente.

### Experimento rejeitado sem k6: `Payload` com buffer textual fixo

Hipótese: `transaction_requested_at` e `last_transaction_timestamp` têm 20 bytes e podem exceder o SSO comum de `std::string`; trocar os campos textuais persistidos do `Payload` por buffers fixos pequenos (`FixedString`) poderia reduzir alocações por request mantendo o parser DOM atual.

Alteração testada:

```text
Payload.transaction_requested_at: std::string -> FixedString<32>
Payload.last_transaction_timestamp: std::string -> FixedString<32>
Payload.merchant_mcc: std::string -> FixedString<8>
parse_timestamp: const std::string& -> std::string_view
mcc_risk: const std::string& -> std::string_view
```

Validação:

```text
cmake --build cpp/build --target test benchmark-request-cpp -j8 => tests 100% passed
```

Microbenchmark offline:

| Variante | parse_payload | parse_vectorize | parse_classify | Decisão |
|---|---:|---:|---:|---|
| baseline histórico DOM | ~630ns | ~696ns | ~28.9us | referência |
| `Payload` com buffer fixo (`repeat=20`) | 709.29ns | 670.02ns | 29.93us | rejeitado |

Leitura: a hipótese reduziu uma possível fonte de heap allocation, mas aumentou custo do parser puro e piorou o caminho agregado com classificação. O ganho em `parse_vectorize` não compensa a piora de `parse_payload` e `parse_classify`; levar isso para k6 seria ruído caro.

Decisão: rejeitado e revertido sem k6. `Payload` voltou a usar `std::string`; `cmake --build cpp/build --target test -j8` voltou a passar 100%.

### Experimento rejeitado: `known_merchants` inline mantendo `std::string`

Hipótese: remover apenas a alocação dinâmica do `std::vector` de `known_merchants`, mantendo ownership dos valores com `std::string`. Diferente da tentativa com `std::string_view`, essa variante não dependeria do lifetime interno do `simdjson` e deveria preservar semântica.

Alteração testada:

```text
std::vector<std::string> known_merchants
  -> KnownMerchantStrings { std::array<std::string, 8> inline_values; std::vector<std::string> overflow; }
```

Validação pré-k6:

```text
cmake --build cpp/build --target test benchmark-request-cpp -j8 => tests 100% passed
```

Microbenchmark offline:

| Variante | parse_payload | parse_vectorize | parse_classify |
|---|---:|---:|---:|
| baseline histórico DOM | ~630ns | ~696ns | ~28.9us |
| inline strings run 1 (`repeat=20`) | 624.93ns | 866.43ns | 28.94us |
| inline strings run 2 (`repeat=20`) | 606.77ns | 662.28ns | 29.88us |

Resultado no benchmark oficial local:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| `known_merchants` inline com `std::string` | 4.76ms | 123 | 117 | 0 | 2998.49 | rejeitado |

Leitura: apesar de parecer semanticamente segura e de mostrar algum ganho no parser puro, a alteração repetiu o mesmo padrão de divergência de classificação das tentativas anteriores na região de `known_merchant`. Isso reforça que mudanças no parser/vetorização não devem mais avançar direto para k6 apenas com checksums agregados; é necessário primeiro um comparador por payload e por dimensão do vetor.

Decisão: rejeitado e revertido. `cpp/src/request.cpp` voltou ao `std::vector<std::string>` original para `known_merchants`; `cmake --build cpp/build --target test -j8` voltou a passar 100%.

### Correção crítica: benchmark local estava desalinhado com o upstream atual

Investigação: após três variantes do parser produzirem o mesmo padrão de `123 FP / 117 FN`, foi rodada uma baseline de controle no estado restaurado. A baseline restaurada também produziu `123 FP / 117 FN`, portanto a causa não era o parser. A divergência estava nos artefatos locais de benchmark.

Estado antigo desta branch:

```text
test/test-data.json: 14500 entradas, sha256=c635c408e0b541b14cf15451e4a00152c439b841b725a8d0dd57d03b871eed49
resources/references.json.gz: 1.6MB, sha256=bf07d83bacae10b784933d5dc998a3355f1da4f2f1016dfe9dbac2bb6ac8b9b7
test/test.js: formato antigo, lendo entry.info.expected_response
Dockerfile: já baixava references.json.gz do commit d501ddc...
```

Estado oficial/upstream atual usado para alinhar a branch:

```text
upstream/main: e701e7c
test/test-data.json: 54100 entradas, sha256=d0f76589e36549f4c9268642787a79be455e85665a7ec080506627012485bb37
resources/references.json.gz: 48MB, sha256=43d10de80609e77ce25740f375607afce7561ec44da50c27c142493db8fcab67
test/test.js: formato atual, lendo entry.expected_approved
cenário k6: 120s com alvo 900 rps
```

Ação aplicada:

```text
test/test-data.json atualizado a partir de upstream/main
resources/references.json.gz atualizado a partir de upstream/main
test/test.js atualizado a partir de upstream/main
```

Resultado de controle no benchmark oficial local atualizado:

| Estado | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| estado restaurado + benchmark upstream atual | 3.12ms | 0 | 0 | 0 | 5505.57 |

Breakdown:

```text
TP=24037
TN=30021
FP=0
FN=0
HTTP=0
weighted_errors_E=0
detection_score=3000
p99_score=2505.57
```

Leitura: as runs anteriores com `123 FP / 117 FN` foram contaminadas por dataset/script local desatualizados e não devem ser usadas como prova semântica contra os parsers testados. As alterações de parser continuam revertidas, mas a rejeição delas agora deve ser lida como "não aceitas nesta rodada"; qualquer retomada precisa usar o benchmark alinhado e, antes do k6, um comparador vetor-a-vetor por payload.

Comparação com ranking/submissão: o controle local atualizado (`3.12ms`, `5505.57`) fica abaixo da submissão parcial informada pelo ranking (`2.83ms`, `5548.91`). A diferença é compatível com ruído de máquina/local e com o cenário oficial atualizado mais pesado; a métrica relevante preservada é `0 FP`, `0 FN`, `0 HTTP`.

### Reteste válido rejeitado: `known_merchants` inline após alinhamento do benchmark

Contexto: a hipótese `known_merchants` inline havia sido rejeitada em uma rodada contaminada por assets locais antigos. Depois da correção de `test/test-data.json`, `resources/references.json.gz` e `test/test.js`, ela foi reexecutada contra o benchmark oficial local atualizado para separar efeito real de ruído de dataset.

Alteração reavaliada:

```text
std::vector<std::string> known_merchants
  -> KnownMerchantStrings { std::array<std::string, 8> inline_values; std::vector<std::string> overflow; }
```

Validação pré-k6:

```text
cmake --build cpp/build --target test benchmark-request-cpp -j8 => tests 100% passed
GET /ready => 204
```

Microbenchmark offline já com dataset atualizado:

| Métrica | Resultado |
|---|---:|
| `parse_payload` | 620.55ns |
| `parse_vectorize` | 678.62ns |
| `parse_classify` | 399.15us |

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| baseline alinhado restaurado | 3.12ms | 0 | 0 | 0 | 5505.57 | referência |
| `known_merchants` inline com `std::string` | 3.15ms | 0 | 0 | 0 | 5501.25 | rejeitado |

Breakdown da variante:

```text
TP=24037
TN=30021
FP=0
FN=0
HTTP=0
weighted_errors_E=0
detection_score=3000
p99_score=2501.25
```

Leitura: com os assets corretos, a hipótese não altera semântica nem gera divergência de classificação. Porém o ganho esperado no parser não se converteu em melhora de p99; houve regressão pequena de `0.03ms` no p99 e perda de `4.32` pontos. Como a competição está sensível a poucos centésimos de milissegundo, a mudança não é sustentável.

Decisão: rejeitado e revertido. O código voltou ao `std::vector<std::string>` original em `cpp/src/request.cpp`. O aprendizado útil é que essa família de micro-otimização de `known_merchants` não deve ser retomada sem evidência micro maior que o ruído do k6.

### Experimento rejeitado: pré-alocação fixa do body HTTP

Hipótese: o microbenchmark alinhado indicou que montar o corpo da request com `std::string::reserve(768)` era mais barato do que deixar o crescimento padrão (`27.28ns` vs. `34.74ns` por query no teste isolado de append). A expectativa era reduzir pequenas alocações ou realocações no hot path de `POST /fraud-score`.

Alteração testada:

```text
body = std::string{}
  -> body inicializado com reserve(768) no lambda de onData
```

Validação:

```text
cmake --build cpp/build --target test -j8 => tests 100% passed
GET /ready => 204
```

Resultado no benchmark oficial local atualizado:

| Variante | p99 | FP | FN | HTTP errors | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---|
| baseline alinhado restaurado | 3.12ms | 0 | 0 | 0 | 5505.57 | referência |
| `body.reserve(768)` | 3.33ms | 0 | 0 | 0 | 5476.94 | rejeitado |

Breakdown da variante:

```text
TP=24037
TN=30021
FP=0
FN=0
HTTP=0
weighted_errors_E=0
detection_score=3000
p99_score=2476.94
```

Leitura: o ganho isolado de append não apareceu no endpoint real. O custo extra de reservar 768 bytes por request e a pressão adicional de heap/cache pioraram o p99 em `0.21ms`, uma perda grande demais para considerar ruído aceitável.

Decisão: rejeitado e revertido. O hot path voltou a usar `body = std::string{}` sem reserva explícita.

### Experimento aceito: redistribuição de CPU para APIs

Hipótese: com nginx em modo `stream` L4 usando UDS, o load balancer provavelmente estava superdimensionado em `0.30 CPU`, enquanto a classificação IVF nas APIs continuava sendo o caminho dominante. Transferir CPU do nginx para as duas APIs poderia reduzir throttling dos classificadores sem aumentar erros.

Alteração testada:

```text
api1/api2: 0.35 CPU -> 0.40 CPU cada
nginx:     0.30 CPU -> 0.20 CPU
total:     1.00 CPU mantido
memória:   inalterada em 165MB + 165MB + 20MB = 350MB
```

Validação:

```text
docker inspect:
  api1 NanoCpus=400000000 Memory=173015040
  api2 NanoCpus=400000000 Memory=173015040
  nginx NanoCpus=200000000 Memory=20971520
GET /ready => 204
```

Resultado no benchmark oficial local atualizado:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| baseline alinhado restaurado (`0.35/0.35/0.30`) | referência | 3.12ms | 0 | 0 | 0 | 5505.57 |
| `0.40/0.40/0.20` | 1 | 3.03ms | 0 | 0 | 0 | 5518.50 |
| `0.40/0.40/0.20` | 2 | 2.96ms | 0 | 0 | 0 | 5528.15 |

Breakdown da melhor run:

```text
TP=24037
TN=30022
FP=0
FN=0
HTTP=0
weighted_errors_E=0
detection_score=3000
p99_score=2528.15
```

Leitura: o ganho reproduziu em duas runs consecutivas e manteve acurácia perfeita. A melhor run local reduziu p99 em `0.16ms` contra o baseline alinhado e elevou o score em `22.58` pontos. Isso confirma que, no cenário atual, o nginx precisa de menos CPU que as APIs e que o gargalo marginal está mais próximo do classificador/worker C++.

Decisão: aceito. `docker-compose.yml` fica com `api1/api2=0.40 CPU` e `nginx=0.20 CPU`.
