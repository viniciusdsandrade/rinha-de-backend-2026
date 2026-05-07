# Daily Report - 2026-05-07

## Contexto operacional

Janela de trabalho atualizada pelo usuário: manter o mesmo ciclo de investigação, experimento, report, commit/push e eventual submissão se houver ganho, agora até **03h30 BRT da madrugada seguinte**.

Estado de partida conferido às `2026-05-07 09:36:54 -03`:

- Branch experimental: `perf/noon-tuning`, limpa e alinhada com `origin/perf/noon-tuning`.
- Branch de submissão: `submission`, limpa e alinhada com `origin/submission`.
- Melhor submissão preparada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-cd3e915`.
- Evidência da melhor submissão: melhor validação pública/local `p99=1.12ms`, `final_score=5950.45`, `0%` falhas; validações finais da branch `submission` em `5938.77`, `5944.96`, `5944.68`.
- Issue oficial de teste: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2009`, ainda `OPEN` e sem comentários na checagem inicial.

Atualização operacional às `09:46`:

- A issue `#2009` passou a responder `HTTP 410` via REST: **foi deletada**.
- A busca por issues abertas/fechadas não encontrou resultado processado para `submission-cd3e915`.
- Existe uma issue aberta da organização, `#2026` (`Runner broken: stale submission directory since #2019`), relatando que o runner oficial está falhando desde `#2019` com `git clone failed: Destination path "submission" already exists and is not an empty directory`.
- Decisão: **não abrir nova issue oficial enquanto o runner estiver reportado como quebrado**, para evitar submeter uma versão boa em uma janela que tende a gerar falha inválida. A branch `submission` permanece preparada com `submission-cd3e915`.

## Ciclo 09h38: especialização do caminho IVF `nprobe=1`

Hipótese: a configuração aceita usa `IVF_FAST_NPROBE=1` e `IVF_FULL_NPROBE=1`. O template `fraud_count_once_fixed<1>` ainda usava a rotina genérica de seleção dos melhores centróides (`insert_probe`, arrays de `best_distances` e laço genérico para ignorar cluster já escaneado). Especializar o caminho `MaxNprobe == 1` poderia reduzir custo fixo por consulta sem alterar métrica, índice, reparo, resposta ou acurácia.

Escopo testado:

- Alterar somente `cpp/src/ivf.cpp`.
- Para `MaxNprobe == 1`, selecionar `best_cluster` por comparação direta em vez de `insert_probe`.
- Para `MaxNprobe == 1`, substituir o laço de `already_scanned` por comparação direta com `best_clusters[0]`.
- Manter o restante do IVF e do compose inalterado.

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-ivf-cpp -j2
ctest --test-dir cpp/build --output-on-failure

100% tests passed, 0 tests failed out of 1
```

Microbenchmark IVF:

```text
cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 1 0

samples=54100 repeat=8 refs=3000000 clusters=1280
ns_per_query=12171.6
fp=0 fn=0 parse_errors=0 failure_rate_pct=0
repaired_pct=4.43808
```

Resultado k6 local com compose reconstruído:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `nprobe=1` especializado #1 | 1.29ms | 0 | 0 | 0 | 5890.47 |

Decisão: **rejeitado e revertido**. A mudança preservou correção, mas piorou a cauda real de forma clara contra a submissão atual (`submission-cd3e915`) e contra as melhores runs públicas/locais recentes. A provável explicação é que a rotina genérica já estava bem otimizada pelo compilador no caso `nprobe=1`, enquanto a especialização mudou layout/inlining/branching de forma desfavorável para o binário final. O código foi restaurado ao estado anterior e os containers do experimento foram derrubados.

Aprendizado: nesta região, não basta reduzir aparência de trabalho no source. O critério continua sendo k6 completo; micro-otimizações no seletor de centróide precisam demonstrar redução real de p99 antes de virar candidato público.

## Ciclo 09h44: `-fno-exceptions` com parser de env sem exceções

Hipótese: a rodada anterior com `-fno-exceptions` falhou porque o binário principal ainda usava `std::stoi`/`std::stoul` com `catch (...)` no parser de variáveis de ambiente e bind address. Reescrever esses trechos com `std::from_chars` permitiria compilar o binário principal sem suporte a exceções, reduzindo código de EH e possivelmente melhorando layout/inlining do hot path.

Escopo testado:

- `cpp/src/main.cpp`: trocar `std::stoi`/`std::stoul` por `std::from_chars` em `parse_bind_addr` e `uint_env_or_default`.
- `cpp/CMakeLists.txt`: adicionar `-fno-exceptions` apenas ao target `rinha-backend-2026-cpp`.
- Sem alteração de algoritmo, parser de payload, IVF, compose, resposta, recursos ou dados.

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure

100% tests passed, 0 tests failed out of 1
```

Resultado k6 local com compose reconstruído:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `-fno-exceptions` + env sem exceções #1 | 1.25ms | 0 | 0 | 0 | 5902.70 |

Decisão: **rejeitado e revertido**. A mudança é semanticamente limpa e preserva correção, mas a p99 real ficou abaixo da submissão preparada (`submission-cd3e915`) e não justifica nova imagem. O ganho teórico de remover exceções do binário não apareceu no score final; provavelmente o caminho dominante continua sendo proxy/scheduling + parse/IVF, e não metadados de EH no binário.

## Ciclo 09h50: revisão do upstream atualizado

Hipótese: antes de continuar perseguindo microganhos, era necessário validar se o diretório oficial mudou requisitos de submissão, benchmark ou pontuação desde a última leitura.

Verificação:

```text
git fetch upstream main
git diff --stat HEAD..upstream/main -- docs/br docs/en test/test.js config.json run.sh
```

Achados relevantes:

- `docs/br/AVALIACAO.md` e `docs/en/EVALUATION.md` foram atualizados para a fórmula logarítmica atual: `p99_score + detection_score`, teto de `6000`, piso de `-6000`, p99 saturando em `1ms`, corte de p99 em `2000ms` e corte de detecção acima de `15%` de falhas.
- `docs/br/SUBMISSAO.md` e `docs/en/SUBMISSION.md` agora distinguem explicitamente **testes de prévia** e **teste final**. A issue `rinha/test [id]` dispara prévia; o teste final rodará uma única vez ao fim da Rinha e pode usar script diferente/mais pesado.
- Nova regra explícita: todos os repositórios precisam estar sob licença MIT.
- Nova regra explícita: não é permitido usar os payloads do teste como lookup.
- `config.json` adicionou `post_test_script`, reduziu health-check retries de `30` para `20` e polling de issues de `120000ms` para `30000ms`.
- `run.sh` oficial ficou mais simples (`K6_NO_USAGE_REPORT=true`, executa k6 silencioso e imprime `test/results.json` com `jq`); não muda contrato da API nem topologia.

Impacto na nossa submissão:

- A implementação atual **não usa lookup dos payloads**; usa índice IVF pré-processado de `references.json.gz` e classificação por vetor.
- A branch `submission` está estruturalmente correta e enxuta: contém apenas `docker-compose.yml`, `info.json` e `nginx.conf`.
- Risco corrigido no branch experimental: o fork não possuía arquivo `LICENSE`/`COPYING` detectável no checkout local. Foi adicionado `LICENSE` MIT no branch de código para alinhar com a nova regra. A branch `submission` não foi alterada, para permanecer mínima com apenas os artefatos de execução.
- Como o teste final pode ser mais pesado, a estratégia de manter `0 FP/FN/HTTP` continua correta. A otimização abaixo de `1ms` só ajuda até saturar o `p99_score`; o ganho restante real é aproximar p99 de `1ms` sem sacrificar detecção.

## Ciclo 09h52: toolchain Clang 18 vs GCC

Hipótese: trocar o compilador poderia melhorar inlining, layout e vetorização do binário C++ sem alterar algoritmo. Como o hot path já está muito apertado, o teste foi feito primeiro em microbenchmark host para evitar rebuild de imagem caro sem sinal prévio.

Achado de compilação:

- Clang rejeitou um shadowing aceito pelo GCC em `cpp/src/main.cpp`: o lambda capturava `body` para acumular chunks e depois declarava outro `body` para o JSON de resposta.
- Ajuste mínimo aplicado: renomear a variável de resposta para `response_body`.
- Esse ajuste é semântico no-op, preserva GCC e deixa a base compilável em Clang para experimentos futuros.

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure

100% tests passed, 0 tests failed out of 1

cmake --build cpp/build-clang --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-ivf-cpp -j2
ctest --test-dir cpp/build-clang --output-on-failure

100% tests passed, 0 tests failed out of 1
```

Microbenchmark comparativo no mesmo índice/dataset:

| Toolchain | ns/query | FP | FN | parse errors | failure_rate |
|---|---:|---:|---:|---:|---:|
| GCC atual | 12629.4 | 0 | 0 | 0 | 0% |
| Clang 18 | 13600.6 | 0 | 0 | 0 | 0% |

Decisão: **Clang rejeitado** para a próxima imagem. O binário Clang ficou cerca de `7.7%` pior no microbenchmark offline, então não há base para pagar o custo de adaptar Dockerfile e rodar k6 completo. O pequeno rename de compatibilidade fica no branch experimental como higiene de portabilidade, mas não altera a submissão atual.

## Ciclo 10h00: desligar `extreme_repair`

Hipótese: o reparo extremo roda em poucos casos (`384` consultas reparadas no repeat 8) e talvez pudesse ser desligado para reduzir custo de cauda sem impacto material de acurácia.

Experimento sem alteração de código, usando o flag de benchmark para desabilitar `extreme_repair`:

```text
cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 1 1

samples=54100 repeat=8 refs=3000000 clusters=1280
ns_per_query=12283.2
fp=8 fn=16 parse_errors=0 failure_rate_pct=0.00554529
repaired_pct=4.34935
extreme_repair_queries=0
```

Decisão: **rejeitado sem k6**. O tempo offline ficou dentro do ruído positivo da rodada, mas a mudança introduziu erros reais de detecção. No dataset base isso equivale a aproximadamente `1 FP / 2 FN` por passada, o suficiente para reduzir drasticamente o `detection_score` frente à submissão atual com `0%` falhas. Nesta faixa de ranking, acurácia perfeita vale mais que remover algumas centenas de repairs.
