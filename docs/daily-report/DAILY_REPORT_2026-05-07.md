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

## Ciclo 10h07: reaproveitar `std::vector` de `known_merchants`

Hipótese: uma alternativa menos agressiva que o parser `known_merchants` sem cópia seria apenas reaproveitar o `std::vector<std::string>` via `thread_local`, reduzindo alocações sem mudar `Payload`, sem usar `string_view` e sem alterar a lógica de comparação.

Baseline microbench limitado (`100` amostras, repeat `20`) antes do patch:

| Métrica | ns/query |
|---|---:|
| `parse_payload` | 548.7 |
| `parse_vectorize` | 601.117 |
| `parse_classify` | 387570 |

Resultado com `thread_local std::vector<std::string>`:

| Métrica | ns/query |
|---|---:|
| `parse_payload` | 563.302 |
| `parse_vectorize` | 600.157 |
| `parse_classify` | 433564 |

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-request-cpp -j2
ctest --test-dir cpp/build --output-on-failure

100% tests passed, 0 tests failed out of 1
```

Decisão: **rejeitado e revertido sem k6**. A mudança não melhora o parser e piora o microbench de classificação limitado. Provável causa: `thread_local` adiciona custo/indireção e o vetor local pequeno já é barato o suficiente frente ao parse DOM/padding. A tentativa anterior com `string_view`/array continua sendo a única variação de `known_merchants` que gerou sinal local, mas ela já foi rejeitada em validação pública.

## Ciclo 10h20: screening tardio de `mimalloc`

Hipótese: como a API ainda faz pequenas alocações por request (`simdjson::padded_string`, `std::string` temporárias e estruturas DOM), trocar o allocator glibc por `mimalloc` poderia suavizar cauda. A ideia foi inspirada pela stack Rust líder, mas tratada como screening tardio, porque allocator costuma ter efeito pequeno e dependente do workload.

Escopo testado:

- `Dockerfile` runtime instalando `libmimalloc2.0`.
- `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libmimalloc.so.2`.
- Sem mudança de código, índice, compose, API ou algoritmo.

Observação operacional:

- A primeira execução via `run-local-k6.sh` retornou `54059` erros HTTP, mas a checagem manual mostrou que a imagem subia corretamente com `mimalloc` e `/ready` respondia `204`.
- Repeti o teste com a pilha já de pé para separar falha de startup de efeito de performance.

Resultado k6 local com `mimalloc`:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `mimalloc` #1 | 1.27ms | 0 | 0 | 0 | 5894.89 |

Decisão: **rejeitado e revertido**. A correção de detecção permaneceu perfeita, mas a p99 ficou muito abaixo da submissão preparada (`submission-cd3e915`, validações finais `5944-5950`). O custo/risco de adicionar biblioteca runtime e `LD_PRELOAD` não se justifica; glibc malloc segue melhor para este hot path.

## Ciclo 10h10: `-fno-stack-protector`

Hipótese: o binário exportava referência a `__stack_chk_fail`, então poderia haver stack protector/hardening residual no caminho de funções com buffers locais. Remover stack protector do binário principal poderia reduzir prólogo/epílogo de funções quentes.

Verificação inicial:

```text
readelf -s cpp/build/rinha-backend-2026-cpp | rg '__stack_chk|chk_fail'
__stack_chk_fail presente
```

Escopo testado:

- Adicionar `-fno-stack-protector` somente ao target `rinha-backend-2026-cpp`.
- Sem alteração de código, compose, índice, parser ou algoritmo.

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-ivf-cpp -j2
ctest --test-dir cpp/build --output-on-failure

100% tests passed, 0 tests failed out of 1
```

Resultado:

```text
readelf após patch: __stack_chk_fail ainda presente
benchmark-ivf-cpp repeat=8:
ns_per_query=12818.1
fp=0 fn=0 parse_errors=0 failure_rate_pct=0
```

Decisão: **rejeitado e revertido sem k6**. A flag no target principal não removeu a dependência final de `__stack_chk_fail`, provavelmente por objetos de bibliotecas linkadas, e o microbenchmark ficou pior que o controle recente GCC (`12629.4 ns/query`). Não há evidência para ampliar a flag para dependências ou publicar imagem.

## Ciclo 10h15: rechecagem de runner, líderes e hipóteses de baixo sinal

Objetivo: evitar repetir trabalho já rejeitado e validar se havia alguma janela segura para submissão ou algum repositório líder novo publicamente indexado.

Verificações:

```text
gh issue view 2026 -R zanfranceschi/rinha-de-backend-2026
state=OPEN
comments=[]
title=Runner broken: stale submission directory since #2019

gh search repos 'rinha 2026 thiagorigonatti'
[]

gh search repos 'rinha 2026 jairoblatt'
[]

gh search repos 'rinha backend 2026 fraud detection'
[]
```

Decisões:

- Não abrir nova issue oficial enquanto `#2026` continuar aberta e sem comentário, porque a falha do runner pode produzir resultado inválido.
- Não repetir transplantes de knobs dos líderes sem URL/código novo. As famílias `HAProxy`, `nginx http`, `nginx 1.29`, `seccomp`, `ulimits`, `cpuset`, `worker_priority`, `backlog`, `reuseport`, `worker_processes`, `MALLOC_ARENA_MAX`, `mimalloc`, parser com `string_view`, parser mutable/padding, `mcc` switch/precompute e flags pequenas já têm evidência negativa ou insuficiente neste stack.
- Não testar prune parcial com comparação `>=` no kernel IVF: apesar de parecer mais agressivo, ele não é semanticamente seguro por causa do desempate por `id` quando a distância empata com o pior top-5.
- Não testar `access_log off`/`error_log off` isolado no `nginx stream`: o modo atual não tem access log de stream configurado, e os runs bons não indicam emissão de erros. A versão com `http { access_log off; error_log /dev/null; }` já foi rejeitada em 2026-05-02.

Leitura: o espaço de micro-otimizações seguras está quase saturado. A melhor submissão preparada continua `submission-cd3e915`; as próximas tentativas com chance real precisam ser estruturais, mas devem ser pequenas o suficiente para não arriscar a máquina: protótipo de servidor HTTP manual/epoll em branch separada, ou nova geração de índice/layout AoSoA16 com benchmark offline antes de qualquer k6.
