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
