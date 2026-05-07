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

## Ciclo 10h25: correção de conformidade MIT na branch padrão

Hipótese/risco: o upstream atualizado passou a exigir que repositórios de participantes estejam sob licença MIT. O branch experimental já tinha `LICENSE`, mas a branch padrão pública do fork (`main`) ainda não.

Verificação:

```text
gh repo view viniciusdsandrade/rinha-de-backend-2026 --json defaultBranchRef,isPrivate,url
defaultBranch=main
isPrivate=false

origin/main: sem LICENSE antes da correção
origin/submission: sem LICENSE
HEAD experimental: LICENSE
```

Ação executada:

- Criado worktree temporário a partir de `origin/main`.
- Adicionado `LICENSE` MIT com copyright `2026 Vinicius Andrade`.
- Primeiro push foi rejeitado por fast-forward porque `origin/main` havia avançado com merge do upstream.
- Feito `git fetch origin main`, rebase do commit de licença sobre `origin/main` atualizado e push sem force.

Resultado:

```text
origin/main: c422143 add mit license
origin/main contém LICENSE
origin/submission permanece mínima, sem LICENSE
```

Decisão: **aceito como correção de conformidade**, não como experimento de performance. A branch `submission` permanece enxuta com `docker-compose.yml`, `info.json` e `nginx.conf`; a exigência de licença fica coberta pela branch padrão pública `main`.

## Ciclo 10h12: índice intermediário `1280/s98304/i6`

Hipótese: o índice atual `1280/s65536/i6` é o melhor ponto conhecido, e o índice `1280/s262144/i10` já havia sido rejeitado por perder acurácia. Faltava medir um ponto intermediário de amostra de treino (`98304`) que talvez preservasse acurácia e melhorasse centróides sem aumentar demais o custo.

Comandos:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280-s98304-i6.bin 1280 98304 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280-s98304-i6.bin 3 0 1 1 1 1 4 1 0
```

Resultado offline:

| Índice | ns/query | FP | FN | parse errors | repaired_pct |
|---|---:|---:|---:|---:|---:|
| `1280/s98304/i6` | 14741.6 | 3 | 6 | 0 | 4.45287 |

Decisão: **rejeitado sem k6**. O índice intermediário perdeu acurácia mesmo com `bbox_repair=true` e `repair=1..4`, além de ficar mais lento que o índice atual em microbenchmark. A escolha de centróides é sensível; mais amostra de treino não é monotonicamente melhor para o nosso repair/top-5. Manter `1280/s65536/i6`.

## Ciclo 10h18: índice menor `1280/s49152/i6`

Hipótese: se aumentar a amostra de treino piorou, talvez uma amostra menor que `65536` pudesse gerar centróides mais favoráveis para o conjunto de teste, ou pelo menos reduzir blocos escaneados sem perder acurácia.

Comandos:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280-s49152-i6.bin 1280 49152 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280-s49152-i6.bin 3 0 1 1 1 1 4 1 0
```

Resultado offline:

| Índice | ns/query | FP | FN | parse errors | repaired_pct |
|---|---:|---:|---:|---:|---:|
| `1280/s49152/i6` | 13515.6 | 0 | 9 | 0 | 4.44547 |

Decisão: **rejeitado sem k6**. A amostra menor preservou FP, mas introduziu `9 FN` no repeat 3 e aumentou blocos primários/bbox. Isso encerra a varredura leve de `train_sample` por hoje: `49152` e `98304` são piores que `65536`, e o `262144/i10` já havia sido rejeitado anteriormente.

## Ciclo 10h20: iterações do índice `1280/s65536`

Hipótese: mantendo o `train_sample=65536`, talvez `i5` ou `i7` fossem melhores que `i6`. Menos iteração poderia preservar clusters mais próximos da amostra inicial; mais iteração poderia reduzir blocos/bbox. O teste foi offline, pois qualquer FP/FN já rejeita a hipótese.

Comandos:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280-s65536-i5.bin 1280 65536 5
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280-s65536-i5.bin 3 0 1 1 1 1 4 1 0

nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280-s65536-i7.bin 1280 65536 7
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280-s65536-i7.bin 3 0 1 1 1 1 4 1 0
```

Resultados offline:

| Índice | ns/query | FP | FN | parse errors | repaired_pct |
|---|---:|---:|---:|---:|---:|
| `1280/s65536/i5` | 12663.7 | 3 | 3 | 0 | 4.43438 |
| `1280/s65536/i7` | 12469.1 | 0 | 6 | 0 | 4.43068 |

Decisão: **rejeitado sem k6**. `i7` reduziu um pouco o custo offline, mas introduziu `FN`, que é exatamente o erro mais caro depois de HTTP error. `i5` também perde acurácia. O ponto `1280/s65536/i6` continua sendo o único desta família com `0 FP/FN` no benchmark local.

## Ciclo 10h15: repair `1..3` mantendo `extreme_repair`

Hipótese: a janela atual repara `fraud_count` inicial `1..4`, além dos extremos selecionados por regra. Se os casos `f4` fossem majoritariamente seguros, pular esse repair reduziria consultas reparadas sem alterar `FN`.

Comando:

```text
cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 3 1 0
```

Resultado:

| Config | ns/query | FP | FN | parse errors | repaired_pct |
|---|---:|---:|---:|---:|---:|
| `repair=1..3` + `extreme_repair` | 13157.6 | 176 | 0 | 0 | 3.72828 |

Decisão: **rejeitado sem k6**. O repair de `f4` é necessário: removê-lo transforma transações legítimas em negativas fraudulentas (`FP`) e derruba o `detection_score`. A janela `1..4` continua justificada.

## Ciclo 10h22: MIT também na branch `submission`

Risco: embora a branch padrão `main` já tenha sido corrigida com `LICENSE`, o runner/organização pode inspecionar diretamente a branch `submission`. Como a regra de submissão exige que `submission` contenha `docker-compose.yml` e `info.json`, mas não proíbe arquivos extras, adicionar `LICENSE` na raiz reduz ambiguidade sem mudar runtime.

Ação:

- Worktree de submissão: `/home/andrade/Desktop/rinha-de-backend-2026-rust`.
- Branch: `submission`.
- Commit: `135c78c` (`add mit license to submission`).
- Push: `origin/submission`.
- Arquivos na raiz após a mudança: `LICENSE`, `docker-compose.yml`, `info.json`, `nginx.conf`.

Validação:

```text
DOCKER_HOST=unix:///run/docker.sock docker compose config --quiet
Resultado: OK
```

Decisão: **aceito como correção de conformidade**, sem alteração de performance. A imagem da submissão continua `submission-cd3e915`; somente a licença foi adicionada ao branch.

## Ciclo 10h19: `AppState` IVF direto sem `std::variant`

Hipótese: como a submissão real usa sempre `IVF_INDEX_PATH`, remover o fallback exato do binário principal e trocar o `std::variant<Classifier, IvfIndex>` por um `IvfIndex` direto poderia eliminar um desvio no hot path e reduzir tamanho/complexidade do binário.

A alteração temporária removeu `refs.cpp`/`classifier.cpp` do target principal e deixou `AppState` com `rinha::IvfIndex index` direto. Os testes legados foram mantidos inalterados.

Validação de build/teste:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed
```

Primeira tentativa k6 foi inválida:

| Condição | p99 | HTTP errors | final_score | Observação |
|---|---:|---:|---:|---|
| k6 sem stack ativo | 0.00ms | 54059 | -3000.00 | `run-local-k6.sh` não sobe compose; não é dado de performance |

Ao subir o compose com a imagem local já disponível (`docker compose up -d --no-build`) e confirmar `/ready`, a medição válida ficou:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `AppState` IVF direto | 1.30ms | 0% | 5886.90 |

Decisão: **rejeitado e revertido**. A hipótese é tecnicamente plausível, mas não gerou ganho mensurável; pelo contrário, caiu abaixo do envelope da submissão atual (`~5944-5950`). O custo do `std::get_if`/`variant` não aparece como gargalo real diante do restante do stack e do ruído do k6 local.

## Ciclo 10h27: `uWS::Loop::setSilent(true)`

Hipótese: uWebSockets escreve por padrão o header `uWebSockets: 20` em todas as respostas. Como o contrato oficial só exige JSON válido/HTTP 200 para `/fraud-score` e 2xx para `/ready`, remover esse marcador poderia reduzir bytes e chamadas de escrita no caminho HTTP sem alterar semântica.

Ação temporária:

```cpp
uWS::Loop::get()->setSilent(true);
```

Validação rápida:

```text
GET /ready -> HTTP/1.1 204 No Content
Headers observados: Date apenas; header uWebSockets removido
```

Resultado k6 válido:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `setSilent(true)` | 1.27ms | 0% | 5897.37 |

Decisão: **rejeitado e revertido**. A remoção do header é funcionalmente segura, mas o ganho esperado é pequeno demais e a amostra ficou bem abaixo do envelope da submissão atual. O custo relevante não está nos bytes desse header.

## Ciclo 10h32: variação de número de clusters IVF

Hipótese: o índice atual usa `1280` clusters. Aumentar clusters poderia reduzir blocos primários por consulta; reduzir clusters poderia melhorar estabilidade/recall da região primária. Testei os dois lados com `train_sample=65536`, `iterations=6`, `nprobe=1`, repair `1..4` e `extreme_repair` ativo.

Comandos:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1536-s65536-i6.bin 1536 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1536-s65536-i6.bin 3 0 1 1 1 1 4 1 0

nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1024-s65536-i6.bin 1024 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1024-s65536-i6.bin 3 0 1 1 1 1 4 1 0
```

Resultados offline:

| Índice | ns/query | FP | FN | repaired_pct | avg_primary_blocks | avg_bbox_scanned_blocks |
|---|---:|---:|---:|---:|---:|---:|
| `1536/s65536/i6` | 12834.6 | 6 | 0 | 4.44917 | 271.393 | 39.893 |
| `1024/s65536/i6` | 13528.3 | 3 | 6 | 4.42884 | 408.823 | 50.9443 |

Decisão: **rejeitado sem k6**. `1536` reduz blocos primários, mas introduz falso positivo; `1024` piora custo e também introduz falso negativo. O ponto `1280/s65536/i6` segue sendo o único da família testada com 0 FP/FN e custo competitivo.

## Ciclo 10h35: refinamento intermediário de clusters IVF

Hipótese: como `1536` foi mais rápido mas errou, e `1024` errou e ficou mais lento, valores intermediários ao redor de `1280` talvez preservassem acurácia com custo menor.

Comandos:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1408-s65536-i6.bin 1408 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1408-s65536-i6.bin 3 0 1 1 1 1 4 1 0

nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1216-s65536-i6.bin 1216 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1216-s65536-i6.bin 3 0 1 1 1 1 4 1 0
```

Resultados offline:

| Índice | ns/query | FP | FN | repaired_pct | avg_primary_blocks | avg_bbox_scanned_blocks |
|---|---:|---:|---:|---:|---:|---:|
| `1408/s65536/i6` | 12582.5 | 9 | 3 | 4.44917 | 304.26 | 45.4231 |
| `1216/s65536/i6` | 13458.3 | 12 | 9 | 4.42884 | 339.522 | 47.1551 |

Decisão: **rejeitado sem k6**. O `1408` até reduz tempo offline, mas já nasce com erro de detecção; `1216` perde nos dois eixos. Com `sample=65536`, `iterations=6`, `nprobe=1` e repair `1..4`, o `1280` continua sendo o ponto dominante.

## Ciclo 10h40: amostragem K-means por ponto médio

Hipótese: o treino K-means do IVF escolhe linhas uniformes começando no início de cada intervalo (`index * rows / sample`). Trocar para o ponto médio de cada intervalo poderia produzir centróides mais representativos sem custo no runtime, pois só muda o pré-processamento do índice.

Patch temporário:

```cpp
sample_rows[index] = (((index * 2 + 1) * rows) / (sample * 2));
```

Comando:

```text
cmake --build cpp/build --target prepare-ivf-cpp benchmark-ivf-cpp -j2
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280-midpoint-s65536-i6.bin 1280 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280-midpoint-s65536-i6.bin 3 0 1 1 1 1 4 1 0
```

Resultado offline:

| Índice | ns/query | FP | FN | repaired_pct | avg_primary_blocks | avg_bbox_scanned_blocks |
|---|---:|---:|---:|---:|---:|---:|
| `1280/midpoint-s65536/i6` | 13241.5 | 9 | 6 | 4.44362 | 328.591 | 52.6158 |

Decisão: **rejeitado e revertido**. A ideia era sustentável porque só afetaria build/preprocessamento, mas a amostragem por ponto médio degradou os centróides para este dataset e introduziu erros de detecção. A amostragem original continua superior.

## Ciclo 10h44: `train_sample=32768`

Hipótese: já havíamos rejeitado `49152`, `98304` e `262144`. Um `train_sample` ainda menor (`32768`) poderia acelerar/regularizar o K-means por amostrar menos linhas, caso a amostra atual estivesse superajustando centróides.

Comando:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1280-s32768-i6.bin 1280 32768 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1280-s32768-i6.bin 3 0 1 1 1 1 4 1 0
```

Resultado offline:

| Índice | ns/query | FP | FN | repaired_pct | avg_primary_blocks | avg_bbox_scanned_blocks |
|---|---:|---:|---:|---:|---:|---:|
| `1280/s32768/i6` | 13629.9 | 6 | 3 | 4.44732 | 334.411 | 53.0566 |

Decisão: **rejeitado sem k6**. A amostra menor piorou custo e acurácia. Com os pontos `32768`, `49152`, `65536`, `98304` e `262144` já avaliados, `65536` continua sendo o único ponto limpo e competitivo desta dimensão.

## Ciclo 10h36: `-ffast-math` no C++/IVF

Hipótese: como os vetores são finitos e normalizados, `-ffast-math` poderia acelerar os cálculos de distância no IVF e no pré-processamento sem alterar a ordenação prática dos vizinhos. O teste foi feito nos targets `rinha-backend-2026-cpp`, `prepare-ivf-cpp` e `benchmark-ivf-cpp`.

Validações:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp prepare-ivf-cpp benchmark-ivf-cpp -j2
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed
```

Resultado offline:

| Variante | ns/query | FP | FN | repaired_pct | avg_primary_blocks | avg_bbox_scanned_blocks |
|---|---:|---:|---:|---:|---:|---:|
| `-ffast-math` | 12037.3 | 0 | 0 | 4.43808 | 322.897 | 45.5807 |

Resultado k6 válido:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `-ffast-math` | 1.27ms | 0% | 5895.59 |

Decisão: **rejeitado e revertido**. O benchmark offline melhorou e preservou acurácia, mas o stack oficial local não confirmou ganho de ponta a ponta. Como a pontuação ficou abaixo do envelope estável da submissão atual (`~5944-5950`), não há ganho sustentável suficiente para promover a flag.

## Ciclo 10h45: recalibração de baseline no mesmo regime de host

Hipótese: a run k6 baixa de `-ffast-math` poderia ter sido ruído/estado da máquina, não regressão da flag. Para comparar de forma justa, reconstruí a imagem sem alterações de código e rodei uma baseline imediatamente depois.

Comandos:

```text
DOCKER_HOST=unix:///run/docker.sock docker compose -p perf-noon-tuning build --pull=false api1 api2
DOCKER_HOST=unix:///run/docker.sock docker compose -p perf-noon-tuning up -d --no-build
DOCKER_HOST=unix:///run/docker.sock nice -n 10 ./run-local-k6.sh
```

Resultado:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| baseline reconstruída, sem alterações | 1.28ms | 0% | 5892.01 |
| `-ffast-math` anterior | 1.27ms | 0% | 5895.59 |

Decisão: **calibração registrada; sem promoção**. O resultado mostra que a rodada estava em um regime de host pior que o envelope histórico, então `-ffast-math` não deve ser lido como regressão. Ao mesmo tempo, também não provou ganho de score de ponta a ponta; por isso segue rejeitado para submissão até existir validação reproduzível acima da imagem atual.

## Ciclo 10h44: contexto de ruído local

Após a recalibração baixa, verifiquei o estado da máquina para evitar tomar decisão por ruído:

```text
docker ps: nenhum container ativo
uptime/load average: 1.04, 1.18, 1.09
nproc: 16
free -m: 7623 MB total, 5149 MB available, swap 2099/4095 MB em uso
processos mais ativos: Docker Desktop/QEMU e shell de benchmark
```

Leitura: não há container concorrente óbvio, mas o ambiente Docker Desktop está com VM/QEMU ativa e swap já usada. Isso explica por que duas runs consecutivas (`baseline` e `-ffast-math`) ficaram no patamar `~5892-5896`, abaixo do melhor envelope histórico. Para as próximas microdecisões, vou privilegiar rejeição por acurácia offline e só usar k6 quando o sinal for forte o suficiente.

## Ciclo 10h47: prune AVX2 após 6 dimensões

Hipótese: o kernel AVX2 atualmente calcula as 8 primeiras dimensões antes de verificar se todas as lanes já passaram da pior distância do top-5. Antecipar esse prune para 6 dimensões poderia descartar blocos ruins com menos operações, mantendo resultado exato porque a distância só cresce com as dimensões restantes.

Patch temporário:

```cpp
for (std::size_t dim = 0; dim < 6; ++dim) { ... prune ... }
for (std::size_t dim = 6; dim < kDimensions; ++dim) { ... }
```

Comando:

```text
cmake --build cpp/build --target benchmark-ivf-cpp -j2
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 1 0
```

Resultado offline:

| Variante | ns/query | FP | FN | repaired_pct |
|---|---:|---:|---:|---:|
| prune após 6 dims | 13209.2 | 0 | 0 | 4.43808 |

Decisão: **rejeitado e revertido**. A mudança é exata, mas piora o custo offline. O prune após 6 dimensões provavelmente roda a checagem cedo demais, antes de acumular distância suficiente para descartar muitos blocos; o ponto atual em 8 dimensões permanece melhor.

## Ciclo 10h50: prune AVX2 tardio após 14 dimensões

Hipótese: depois de o prune em 6 dimensões piorar, testei pontos mais tardios. A ideia era verificar se a checagem parcial após 8 dimensões estava custando mais do que economizava, por disparar antes de acumular distância suficiente para descartar muitos blocos.

Resultados offline intermediários:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| prune após 10 dims | 12465.0 | 0 | 0 |
| prune após 12 dims | 12443.0 | 0 | 0 |
| prune após 14 dims, mantendo skip de bloco | 12097.3 | 0 | 0 |
| sem skip de bloco antes do insert | 12724.2 | 0 | 0 |

Patch mantido na branch experimental:

```cpp
for (std::size_t dim = 0; dim < kDimensions; ++dim) {
    acc = acc_dim_i32(acc, q[dim], blocks_ptr + block_base + (dim * kBlockLanes));
}
// mantém skip se todas as lanes são piores que o worst atual
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-ivf-cpp -j2
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed
benchmark final limpo: 12270.4 ns/query, 0 FP, 0 FN
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| prune após 14 dims | 1.27ms | 0% | 5897.34 |
| baseline recalibrada no mesmo regime | 1.28ms | 0% | 5892.01 |

Decisão: **aceito apenas na branch experimental; não promovido para `submission` ainda**. A mudança é exata, preserva acurácia e reduz custo offline. No k6 ruidoso atual há pequeno ganho contra a baseline recalibrada, mas ainda não supera a submissão estável (`~5944-5950`). Próximo passo: combinar/validar em janela de host menos ruidosa antes de publicar imagem oficial.

## Ciclo 10h55: `-funroll-loops` no binário da API e benchmark IVF

Hipótese: com o kernel AVX2 agora fazendo o prune só depois das 14 dimensões, deixar o compilador desenrolar loops poderia reduzir branches/overhead no hot path do IVF sem tocar no algoritmo nem na acurácia.

Patch temporário:

```cmake
target_compile_options(rinha-backend-2026-cpp PRIVATE ... -funroll-loops)
target_compile_options(benchmark-ivf-cpp PRIVATE ... -funroll-loops)
```

Validação offline:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp benchmark-ivf-cpp -j2
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 1 0
```

Resultado:

| Variante | ns/query | FP | FN | repaired_pct |
|---|---:|---:|---:|---:|
| prune 14 dims, sem `-funroll-loops` | 12270.4 | 0 | 0 | 4.43808 |
| prune 14 dims, com `-funroll-loops` | 12788.7 | 0 | 0 | 4.43808 |

Decisão: **rejeitado e revertido**. A flag preserva acurácia, mas piora o custo offline em aproximadamente `+4.2%`. Interpretação: o hot path já está suficientemente explícito/vetorizado, e o desenrolamento automático provavelmente aumentou pressão de código/cache sem reduzir trabalho real. Não vale k6 nem promoção.

## Ciclo 11h02: filtro por lane antes de `Top5::insert`

Hipótese: após calcular as 14 dimensões no bloco AVX2, muitas lanes já ficam acima do pior top-5 atual. Checar `values[lane] <= top.worst_distance()` antes de chamar `Top5::insert` poderia evitar chamadas inúteis e `refresh_worst()` desnecessário, mantendo o desempate correto porque a igualdade ainda chamaria `insert`.

Patch temporário:

```cpp
if (id != invalid && values[lane] <= top.worst_distance()) {
    top.insert(values[lane], labels_ptr[label_base + lane], id);
}
```

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests benchmark-ivf-cpp -j2
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed
```

Resultados offline:

| Run | ns/query | FP | FN |
|---|---:|---:|---:|
| filtro por lane #1 | 12040.1 | 0 | 0 |
| filtro por lane #2 | 13149.7 | 0 | 0 |
| filtro por lane #3 | 12864.8 | 0 | 0 |

Decisão: **rejeitado e revertido**. A primeira run parecia promissora, mas não reproduziu. A hipótese adiciona um branch por lane e chama `worst_distance()` com frequência; quando o branch predictor não ajuda, o custo supera a economia de chamadas a `insert`. Como a melhoria não é estável nem inquestionável, não vale k6 nem promoção.

## Ciclo 11h10: `always_inline` em `acc_dim_i32`

Hipótese: forçar inline no helper AVX2 usado em cada dimensão do bloco poderia eliminar qualquer chamada residual no loop quente e estabilizar melhor o código gerado.

Patch temporário:

```cpp
inline __attribute__((always_inline)) __m256i acc_dim_i32(...)
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| `always_inline` #1 | 11963.2 | 0 | 0 |
| `always_inline` #2 | 12429.1 | 0 | 0 |
| `always_inline` #3 | 12439.7 | 0 | 0 |
| baseline revertido no mesmo momento | 12317.6 | 0 | 0 |

Decisão: **rejeitado e revertido**. O melhor ponto isolado foi bom, mas a sequência não mostrou ganho sustentável; a média do patch ficou praticamente empatada com o baseline imediato e abaixo do critério de melhoria inquestionável. Não vale k6 nem promoção.

## Ciclo 11h18: ordem manual no `bbox_lower_bound`

Hipótese: no repair IVF, o `bbox_lower_bound` testa em média `~56.8` clusters por query. Reordenar as dimensões para começar pelas mais discriminativas/sentinel (`minutes_since_last_tx`, `km_from_last_tx`, `km_from_home`, `amount_vs_avg`, flags) poderia antecipar o `sum > stop_after` e reduzir CPU sem alterar o conjunto escaneado.

Patch temporário:

```cpp
constexpr std::array<std::size_t, kDimensions> kBboxDimensionOrder{
    5, 6, 7, 2, 0, 8, 11, 12, 3, 4, 9, 10, 1, 13
};
for (const std::size_t dim : kBboxDimensionOrder) { ... }
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| ordem manual #1 | 12670.4 | 0 | 0 |
| ordem manual #2 | 12564.2 | 0 | 0 |

Decisão: **rejeitado e revertido**. A acurácia foi preservada, mas a ordem natural continua melhor no benchmark. A provável explicação é que o custo indireto/menos linear da ordem manual supera qualquer early-abort adicional, e as bboxes por cluster já são largas em muitas dimensões.

## Ciclo 11h25: unroll manual do `bbox_lower_bound`

Hipótese: como o `bbox_lower_bound` tem early return, o compilador poderia não desenrolar o loop natural de 14 dimensões. Escrever as 14 dimensões explicitamente em macro local poderia reduzir overhead de loop no repair sem mudar a ordem nem a acurácia.

Patch temporário:

```cpp
RINHA_BBOX_DIM(0);
RINHA_BBOX_DIM(1);
...
RINHA_BBOX_DIM(13);
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| unroll manual #1 | 12512.8 | 0 | 0 |
| unroll manual #2 | 12575.7 | 0 | 0 |

Decisão: **rejeitado e revertido**. A macro preserva resultado, mas piora o custo. O loop natural segue melhor; provável combinação de código menor, melhor cache de instruções e otimização suficiente do compilador.

## Ciclo 11h38: alinhar `benchmark-ivf-cpp` com IPO/LTO da API

Hipótese: a API `rinha-backend-2026-cpp` já compila com `INTERPROCEDURAL_OPTIMIZATION`, mas o alvo `benchmark-ivf-cpp` não. Isso faz o filtro offline medir um binário ligeiramente diferente do hot path real da API. A mudança não otimiza a submissão diretamente; ela melhora a fidelidade do instrumento usado para aceitar/rejeitar próximos experimentos.

Patch mantido:

```cmake
if(RINHA_IPO_SUPPORTED)
    set_property(TARGET benchmark-ivf-cpp PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
```

Resultados offline após reconstruir o benchmark:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| benchmark IVF com IPO #1 | 12348.7 | 0 | 0 |
| benchmark IVF com IPO #2 | 12016.9 | 0 | 0 |

Decisão: **mantido como ajuste de metodologia na branch experimental**. Não é uma promoção de performance para `submission`, mas reduz um desvio entre benchmark e API, já que a API final já usa IPO. As próximas decisões offline devem considerar que a faixa ainda tem ruído, mas agora mede um binário mais parecido com o real.

## Ciclo 11h55: nearest centroid AVX2 para `nprobe=1`

Hipótese: o custo restante do IVF não estava apenas no scan dos blocos, mas também na seleção do cluster primário. Com `1280` clusters e `14` dimensões, o caminho escalar fazia `1280 x 14` cargas/contas por query. Como os centróides já estão em layout transposto (`centroids_[dim * clusters + cluster]`), dá para calcular 8 clusters por vez com AVX2, mantendo a mesma ordem de soma por lane e desempate por menor índice.

Patch mantido na branch experimental:

```cpp
__attribute__((target("avx2")))
std::uint32_t nearest_centroid_avx2(...) {
    for (; cluster + 8 <= clusters; cluster += 8) {
        __m256 acc = _mm256_setzero_ps();
        for (std::size_t dim = 0; dim < kDimensions; ++dim) {
            const __m256 centroid = _mm256_loadu_ps(centroids.data() + (dim * clusters) + cluster);
            const __m256 q = _mm256_set1_ps(query[dim]);
            const __m256 delta = _mm256_sub_ps(q, centroid);
            acc = _mm256_add_ps(acc, _mm256_mul_ps(delta, delta));
        }
        ...
    }
}
```

Validação offline:

```text
cmake --build cpp/build --target benchmark-ivf-cpp -j2
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 1 0
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| centróide escalar, benchmark com IPO #1 | 12348.7 | 0 | 0 |
| centróide escalar, benchmark com IPO #2 | 12016.9 | 0 | 0 |
| centróide AVX2 #1 | 8110.35 | 0 | 0 |
| centróide AVX2 #2 | 8201.03 | 0 | 0 |
| centróide AVX2 #3 | 8329.17 | 0 | 0 |

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed
```

Validação k6 local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| centróide AVX2 | 1.24ms | 0% | 5906.98 |

Decisão: **aceito na branch experimental; ainda não promovido para `submission`**. Esta é a primeira melhoria forte e sustentável da rodada: o offline caiu de `~12.0-12.3 us/query` para `~8.1-8.3 us/query` sem FP/FN. O k6 também melhorou contra o envelope ruidoso recente (`~5892-5897`), mas ainda ficou abaixo do melhor histórico da submissão publicada (`~5944-5950`). Próximo passo: validar em outra janela/rodada k6 e, se estabilizar acima da `submission`, publicar nova imagem e abrir nova issue de submissão apenas se a issue oficial atual não estiver bloqueando.

## Ciclo 12h08: reduzir comparação escalar no nearest centroid AVX2

Hipótese: a primeira versão AVX2 do nearest centroid ainda fazia 8 comparações escalares por bloco de centróides. Mantendo o melhor de cada lane em registradores (`best_distances` + `best_clusters`) e reduzindo apenas no final, o caminho evita `1280` comparações escalares por query, preservando desempate por menor cluster na redução final.

Patch mantido:

```cpp
__m256 best_distances = _mm256_set1_ps(inf);
__m256i best_clusters = _mm256_setzero_si256();
...
const __m256 mask = _mm256_cmp_ps(acc, best_distances, _CMP_LT_OQ);
best_distances = _mm256_blendv_ps(best_distances, acc, mask);
best_clusters = _mm256_blendv_epi8(best_clusters, candidate_clusters, _mm256_castps_si256(mask));
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| centróide AVX2 com comparação escalar #1 | 8110.35 | 0 | 0 |
| centróide AVX2 com comparação escalar #2 | 8201.03 | 0 | 0 |
| centróide AVX2 com comparação escalar #3 | 8329.17 | 0 | 0 |
| centróide AVX2 com melhor por lane #1 | 7695.19 | 0 | 0 |
| centróide AVX2 com melhor por lane #2 | 7522.45 | 0 | 0 |
| centróide AVX2 com melhor por lane #3 | 7585.52 | 0 | 0 |

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed
```

Observação de infra: a primeira tentativa de k6 desta variante foi descartada porque o build Docker falhou por DNS/OAuth em `auth.docker.io`, e `up --no-build` teria usado imagem local antiga. O stack foi derrubado e o build foi reexecutado até `BUILD_OK` antes do k6 válido.

Validação k6 válida:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| centróide AVX2 com comparação escalar | 1.24ms | 0% | 5906.98 |
| centróide AVX2 com melhor por lane | 1.23ms | 0% | 5908.42 |

Decisão: **aceito na branch experimental; ainda não promovido para `submission`**. O refinamento é sustentável no offline e gera pequeno avanço no k6 local válido, mas a pontuação segue abaixo da submissão histórica publicada. Próximo passo: buscar combinação adicional ou validar em janela menos ruidosa antes de qualquer nova imagem oficial.

## Ciclo 12h16: FMA explícito no nearest centroid AVX2

Hipótese: trocar `mul + add` por `_mm256_fmadd_ps` no cálculo de distância dos centróides poderia reduzir uma instrução por dimensão/lane no caminho AVX2. Como FMA pode alterar arredondamento, a regra era aceitar apenas com `0 FP / 0 FN`.

Patch temporário:

```cpp
acc = _mm256_fmadd_ps(delta, delta, acc);
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| FMA explícito #1 | 7705.01 | 0 | 0 |
| FMA explícito #2 | 7900.34 | 0 | 0 |

Decisão: **rejeitado e revertido**. A acurácia se manteve, mas o custo ficou pior/empatado contra a versão `mul + add` (`~7522-7695 ns/query`). Interpretação: o compilador/CPU já lida bem com as duas instruções, e o FMA não trouxe redução prática no loop.

## Ciclo 12h23: comparação direta de cluster já escaneado no repair

Hipótese: como o perfil atual usa `nprobe=1`, o repair não precisaria percorrer genericamente `best_clusters[0..nprobe)` para saber se um cluster já foi escaneado; uma comparação direta com `best_clusters[0]` deveria reduzir um pequeno custo em todas as queries reparadas.

Patch temporário:

```cpp
bool already_scanned = cluster == best_clusters[0];
for (std::uint32_t index = 1; index < nprobe; ++index) { ... }
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| comparação direta #1 | 7537.98 | 0 | 0 |
| comparação direta #2 | 8337.15 | 0 | 0 |
| comparação direta #3 | 7531.10 | 0 | 0 |

Decisão: **rejeitado e revertido**. Apesar de duas runs próximas do melhor estado, o resultado não superou claramente a versão aceita do nearest centroid (`7522.45`, `7585.52`, `7695.19`) e teve um outlier ruim. Como a regra da rodada é performance sustentável e inquestionável, não vale manter.

## Ciclo 12h29: pré-carregar query lanes no nearest centroid AVX2

Hipótese: `_mm256_set1_ps(query[dim])` estava dentro do loop de blocos de centróides. Pré-carregar as 14 lanes da query uma vez fora do loop poderia reduzir trabalho repetido.

Patch temporário:

```cpp
__m256 q[kDimensions];
for (std::size_t dim = 0; dim < kDimensions; ++dim) {
    q[dim] = _mm256_set1_ps(query[dim]);
}
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| query lanes pré-carregadas #1 | 8314.78 | 0 | 0 |
| query lanes pré-carregadas #2 | 8006.27 | 0 | 0 |

Decisão: **rejeitado e revertido**. A acurácia se manteve, mas o custo piorou. A hipótese provavelmente aumentou pressão de registradores ou spills; manter o `set1` local dentro do loop deixa o compilador gerar código melhor.

## Ciclo 12h35: dois acumuladores no nearest centroid AVX2

Hipótese: o loop de 14 dimensões do centróide tinha uma dependência serial em `acc`. Dividir em `acc0` para dimensões `0..6` e `acc1` para `7..13`, somando ao final, poderia reduzir latência do pipeline.

Patch temporário:

```cpp
__m256 acc0 = _mm256_setzero_ps();
__m256 acc1 = _mm256_setzero_ps();
...
const __m256 acc = _mm256_add_ps(acc0, acc1);
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| dois acumuladores #1 | 7953.40 | 0 | 0 |
| dois acumuladores #2 | 7706.46 | 0 | 0 |

Decisão: **rejeitado e revertido**. A mudança manteve acurácia, mas piorou o custo. Provável causa: pressão adicional de registradores e código maior superam qualquer benefício de quebrar dependência.
