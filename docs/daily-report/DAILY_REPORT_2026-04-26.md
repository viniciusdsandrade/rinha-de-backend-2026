# Daily Report 2026-04-26

Branch ativa: `submission-2`.

Objetivo da rodada: continuar a investigação de performance da submissão C++20/uWebSockets/nginx com carga leve sobre a máquina, priorizando hipóteses com ganho técnico sustentável e validação antes de alterar o caminho oficial.

Janela operacional combinada: trabalhar somente até `23:00` em `America/Sao_Paulo`. A rodada registrada abaixo ocorreu a partir de ~`21:26`, com execução deliberadamente leve para não pressionar a máquina.

## Baseline de Referência

Estado aceito antes da rodada:

| Configuração | p99 | final_score | FP | FN | HTTP errors | Observação |
|---|---:|---:|---:|---:|---:|---|
| C++20/uWebSockets + AVX2/FMA + nginx stream UDS, `api=0.44` cada e `nginx=0.12` | 3.92ms | 5407.21 | 0 | 0 | 0 | Melhor run aceito da validação de encerramento em 2026-04-25 |

Média da validação final 5x do baseline aceito em 2026-04-25:

| p99 médio | final_score médio | FP/FN/HTTP |
|---:|---:|---|
| 4.04ms | 5393.78 | 0/0/0 em todos os runs |

## Experimentos

| Horário | Hipótese | Mudança | Evidência | Decisão | Aprendizado |
|---|---|---|---|---|---|
| 21:26 | O gargalo real é o KNN exato varrendo 100k referências por request; agrupar referências por features discretas e usar lower bound pode reduzir candidatos sem perder exatidão | Criado benchmark offline `benchmark-classifier-cpp` para comparar scan completo contra grupos por `last_transaction` sentinel, booleanos `is_online/card_present/unknown_merchant` e bucket de `mcc_risk` | Amostra 1.000 queries: `mismatches=0`, scan completo `880889 ns/query`, agrupado `121880 ns/query`, linhas médias `100000 -> 13469.8` | Prosseguir para dataset completo | A hipótese tem ganho material fora do ruído e mantém decisão idêntica na amostra inicial. |
| 21:27 | O agrupamento precisa preservar o resultado no dataset local completo antes de entrar no servidor | Benchmark offline em 14.500 queries de `test/test-data.json` | `mismatches=0`, `groups=117`, scan completo `867726 ns/query`, agrupado `122422 ns/query`, linhas médias `100000 -> 13836.9`, `groups_per_query=2.8069` | Prosseguir para implementação no classificador | O lower bound por grupo preservou `fraud_count` do top-5 para todo o dataset local e reduziu ~86% da varredura média no benchmark escalar. |
| 21:28 | Portar o grouping para o `ReferenceSet` e para o caminho AVX2 pode reduzir p99 real no compose oficial local | `ReferenceSet` passou a construir `ReferenceGroup` no carregamento; `top5_avx2` passou a ordenar grupos por lower bound e visitar somente grupos ainda capazes de bater o top-5 atual; fallback para scan original se grupos estiverem vazios ou acima do limite de segurança | `ctest --test-dir cpp/build --output-on-failure`: 100% passou. Benchmark offline pós-port: `mismatches=0`, scan completo `956782 ns/query`, agrupado `129234 ns/query`, linhas médias `100000 -> 13836.9` | Validar no compose | O ganho offline permaneceu após integrar a estrutura real de produção. O custo de manter cópia agrupada do dataset é baixo frente ao limite de memória observado. |
| 21:29 | O ganho offline deve aparecer no stack oficial local mesmo com nginx/UDS/quotas | `docker compose up -d --build --force-recreate` com prioridade baixa via `nice -n 10 ionice -c3`; smoke `GET /ready` | `/ready` retornou 204. Limites efetivos: APIs `440M` NanoCPUs e `165MB`; nginx `120M` NanoCPUs e `20MB`. Memória idle: API1 `17.21MiB`, API2 `16.03MiB`, nginx `10.02MiB` | Rodar k6 controlado | A cópia agrupada não pressionou memória; o processo continuou muito abaixo dos limites. |
| 21:30 | O classificador agrupado deve melhorar p99 sem perder precisão | k6 local oficial, run 1 | `p99=3.79ms`, `final_score=5421.46`, `FP=0`, `FN=0`, `HTTP=0` | Reamostrar por risco de outlier | Primeiro run superou o melhor run aceito anterior, mas precisava confirmação porque já houve outliers locais antes. |
| 21:31 | Confirmar se o ganho é reproduzível em amostra curta sem sobrecarregar a máquina | k6 local oficial, run 2 | `p99=3.27ms`, `final_score=5485.37`, `FP=0`, `FN=0`, `HTTP=0` | Reamostrar uma última vez | O ganho ficou material e não apenas marginal. |
| 21:32 | Terceira amostra leve para decidir se mantém ou reverte | k6 local oficial, run 3 | `p99=3.25ms`, `final_score=5488.65`, `FP=0`, `FN=0`, `HTTP=0` | Manter hipótese como candidata forte | A terceira amostra confirmou ganho grande com detecção perfeita. |
| 21:35 | Remover warnings locais de AVX sem alterar comportamento | `std::array<__m256>` foi trocado por array C local em `top5_avx2`; benchmark ficou fora do alvo `all` via `EXCLUDE_FROM_ALL` | Build local `-j2`, `ctest`, benchmark 1.000 queries com `mismatches=0`, `git diff --check` sem problemas | Manter | A limpeza reduz ruído de build e evita que o benchmark offline aumente o custo do build Docker padrão. |
| 21:38 | Validar o binário final após limpeza de warnings | Rebuild incremental do compose com `nice/ionice`, smoke `/ready`, k6 local oficial | `/ready=204`; k6: `p99=3.52ms`, `final_score=5454.07`, `FP=0`, `FN=0`, `HTTP=0` | Aceito provisoriamente | O binário final pós-limpeza manteve ganho material sobre o baseline, embora o melhor run continue sendo o run 3 anterior. |
| 21:40 | Procurar chave de agrupamento melhor sem mexer no caminho oficial | Criado modo `sweep` no benchmark offline para comparar `base`, `no_risk`, `amount4`, `amount8`, `hour`, `amount4_hour` e `amount4_hour_day` | Amostra 3.000 queries: `base=248068 ns/query`, `amount4=227987 ns/query`, `amount8=241616 ns/query`, `hour=318865 ns/query`, `amount4_hour=389363 ns/query`, `amount4_hour_day=1179520 ns/query`; todos com `mismatches=0` | Testar somente `amount4` no compose | `amount4` foi a única variação com sinal offline material e custo de grupos ainda aceitável. As chaves por hora/dia reduziram linhas, mas aumentaram overhead de ordenação/grupos e pioraram tempo. |
| 21:45 | Verificar se o ganho offline de `amount4` se traduz em performance real | `group_key` de produção foi temporariamente estendida com bucket de `amount` em 4 faixas; build/teste local passou; compose foi reconstruído com prioridade baixa e k6 executado uma vez | k6: `p99=3.78ms`, `final_score=5422.11`, `FP=0`, `FN=0`, `HTTP=0` | Reverter `amount4` e manter agrupamento base | O ganho offline foi marginal e não apareceu no stack real. A variação ficou pior que o agrupamento base pós-limpeza (`3.52ms`) e pior que os melhores runs da rodada (`3.27ms`/`3.25ms`). |
| 21:47 | Deixar o ambiente local coerente com o código final revertido | Após reverter `amount4`, rebuild local `-j2`, `ctest`, `git diff --check`, `docker compose config -q` e rebuild do compose em baixa prioridade | `cmake --build`: sem trabalho pendente; `ctest`: 1/1 passou; `git diff --check`: sem problemas; `docker compose config -q`: sem erro; `/ready=204` | Encerrar a rodada técnica e preparar commit escopado | O estado final publicado deve ser o agrupamento base, não o screening `amount4`. O compose local foi reconstruído para evitar confusão em testes posteriores. |

## Resultado Comparativo

Comparação contra o melhor run aceito anterior:

| Métrica | Antes | Melhor run da rodada | Diferença |
|---|---:|---:|---:|
| p99 | 3.92ms | 3.25ms | -0.67ms, ~17.1% melhor |
| final_score | 5407.21 | 5488.65 | +81.44 pontos |
| FP/FN/HTTP | 0/0/0 | 0/0/0 | Sem regressão |

Comparação por média:

| Métrica | Baseline aceito 5x | Rodada agrupada 4x | Diferença |
|---|---:|---:|---:|
| p99 médio | 4.04ms | 3.46ms | -0.58ms, ~14.4% melhor |
| final_score médio | 5393.78 | 5462.39 | +68.61 pontos |
| FP/FN/HTTP | 0/0/0 | 0/0/0 | Sem regressão |

## Estado Atual da Hipótese

O índice exato por grupos com lower bound é a primeira melhoria técnica material desde o ajuste de stack C++:

- Mantém kNN exato no sentido de não descartar grupo cujo lower bound ainda possa superar o top-5 corrente.
- Preservou o `fraud_count` do top-5 em 14.500 queries locais no benchmark offline.
- Reduziu a varredura média offline de `100000` para `13836.9` linhas por query.
- Melhorou p99 no compose local em 4 runs da rodada: `3.79ms`, `3.27ms`, `3.25ms`, `3.52ms`.
- Não introduziu FP, FN nem HTTP errors nos quatro k6.
- A variação `amount4` foi rejeitada: apesar de ser a melhor no benchmark offline, entregou `p99=3.78ms` no k6 e foi revertida para preservar a configuração base mais estável.

## Validações Executadas

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-tests rinha-backend-2026-cpp -j2
ctest --test-dir cpp/build --output-on-failure
cpp/build/benchmark-classifier-cpp resources/references.json.gz test/test-data.json 1 0
cpp/build/benchmark-classifier-cpp resources/references.json.gz test/test-data.json 1 3000 sweep
docker compose up -d --build --force-recreate
curl -sS -o /dev/null -w '%{http_code}\n' http://localhost:9999/ready
./run.sh  # 5 execuções controladas via nice/ionice, incluindo 1 screening rejeitado de amount4
docker compose config -q
git diff --check
```

Resultados:

- `ctest`: 1/1 teste passou.
- Benchmark offline completo: `mismatches=0`.
- `/ready`: HTTP 204.
- `docker compose config -q`: sem erro.
- `git diff --check`: sem problemas.
- k6: 5 execuções com `0 FP`, `0 FN`, `0 HTTP errors`; 4 no agrupamento base aceito provisoriamente e 1 no `amount4` rejeitado.

## Próximos Passos

1. Reamostrar o agrupamento base em 5x quando a máquina estiver mais livre, separando média estável de melhor run.
2. Não promover novas chaves de grupo apenas por benchmark offline; qualquer variação precisa superar o agrupamento base no k6, com `0 FP`, `0 FN` e `0 HTTP errors`.
3. Próximas variações aceitáveis devem reduzir overhead por query, não apenas linhas varridas: evitar grupos demais, ordenação cara e bounding boxes pouco seletivas.
4. Não voltar para micro-otimizações de parser/headers/allocator antes de esgotar a linha de índice exato por grupos.
