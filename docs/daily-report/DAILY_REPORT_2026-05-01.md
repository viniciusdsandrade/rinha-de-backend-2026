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
