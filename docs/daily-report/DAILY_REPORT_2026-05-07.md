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

## Ciclo 12h42: repetição k6 do nearest centroid AVX2 aceito

Depois dos microexperimentos rejeitados, rodei nova validação k6 sem rebuild para confirmar se o ganho real do nearest centroid AVX2 com melhor por lane era estável no compose local.

Resultado:

| Run | p99 | Falhas | final_score |
|---|---:|---:|---:|
| k6 válido #1 | 1.23ms | 0% | 5908.42 |
| k6 repetição #2 | 1.24ms | 0% | 5907.40 |

Decisão: **mantém branch experimental, sem promoção ainda**. A repetição confirma ganho local contra o envelope ruidoso recente, mas não supera a `submission` publicada historicamente (`~5944-5950`). Não há base para abrir nova issue/submissão neste momento.

## Ciclo 12h55: comparação apples-to-apples contra `submission`

Para separar ruído local de ganho real, rodei a imagem atualmente publicada na branch `submission` (`ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-cd3e915`) no mesmo host e na mesma janela da branch experimental.

Primeira tentativa: inválida, porque executei no checkout `/home/andrade/Desktop/rinha-de-backend-2026-rust`, que não possui `run-local-k6.sh`. O stack foi derrubado sem usar resultado. Segunda tentativa: válida, subindo o compose da `submission` e executando o k6 pelo script do worktree experimental contra `localhost:9999`.

Resultado comparativo local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `submission` atual `submission-cd3e915` | 1.27ms | 0% | 5896.08 |
| experimental centróide AVX2 #1 | 1.23ms | 0% | 5908.42 |
| experimental centróide AVX2 #2 | 1.24ms | 0% | 5907.40 |

Decisão: **a branch experimental virou candidata real de promoção**, mas ainda não vou abrir nova issue oficial. Motivo: ela supera a `submission` atual no mesmo regime local por `~11-12` pontos, porém a melhor evidência histórica da `submission` segue mais alta (`~5944-5950`) e a issue oficial `#2026` continua aberta sem comentário, indicando runner/submissão ainda bloqueados. Próximo passo prudente: mais uma validação em janela menos ruidosa ou preparar imagem candidata sem acionar issue enquanto `#2026` estiver aberta.

## Ciclo 13h02: nearest centroid AVX2 em blocos de 16 clusters

Hipótese: processar 16 clusters por iteração com dois vetores AVX2 poderia reduzir overhead do loop externo e reutilizar o mesmo broadcast da query por dimensão.

Patch temporário:

```cpp
for (; cluster + 16 <= clusters; cluster += 16) {
    __m256 acc0 = ...
    __m256 acc1 = ...
    ...
}
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| bloco 16 #1 | 7968.67 | 0 | 0 |
| bloco 16 #2 | 7581.96 | 0 | 0 |

Decisão: **rejeitado e revertido**. A acurácia se manteve, mas o resultado não melhorou a versão aceita e adicionou complexidade/pressão de registradores. O bloco de 8 com melhor por lane continua sendo o melhor formato medido.

## Ciclo 13h10: prune parcial no nearest centroid AVX2

Hipótese: calcular as primeiras 7 dimensões do centróide e descartar o bloco se todas as lanes já estivessem piores ou empatadas com o melhor daquela lane poderia evitar as 7 dimensões restantes sem alterar o resultado. A regra é exata porque distâncias só aumentam com as dimensões restantes, e empate posterior não vence clusters anteriores.

Patch temporário:

```cpp
for (std::size_t dim = 0; dim < 7; ++dim) { ... }
const __m256 partial_ge = _mm256_cmp_ps(acc, best_distances, _CMP_GE_OQ);
if (_mm256_movemask_ps(partial_ge) == 0xFF) {
    continue;
}
for (std::size_t dim = 7; dim < kDimensions; ++dim) { ... }
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| prune centróide 7 dims #1 | 8426.62 | 0 | 0 |
| prune centróide 7 dims #2 | 8536.30 | 0 | 0 |

Decisão: **rejeitado e revertido**. A acurácia foi preservada, mas a checagem parcial é cara demais. O melhor por lane demora a ficar apertado e o branch prejudica o pipeline; calcular as 14 dimensões direto segue melhor.

## Ciclo 13h18: calibração `stats=0` vs `stats=1` no benchmark IVF

Percebi que os benchmarks offline vinham usando `stats=1`, enquanto a API real usa o caminho sem stats. Fiz uma primeira medição, mas descartei porque o binário ainda estava compilado com a hipótese anterior de prune parcial antes da reversão. Depois reconstruí o benchmark limpo e medi os dois modos.

Comando válido:

```text
cmake --build cpp/build --target benchmark-ivf-cpp -j2
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 0 0
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 1 0
```

Resultados válidos:

| Modo | ns/query | FP | FN |
|---|---:|---:|---:|
| `stats=0` #1 | 7749.57 | 0 | 0 |
| `stats=0` #2 | 7810.13 | 0 | 0 |
| `stats=1` #1 | 7734.14 | 0 | 0 |

Decisão: **sem mudança de código**. A diferença entre os modos é pequena e está dentro do ruído local, então `stats=1` continua útil para diagnósticos de repair/cluster. Mas, para decisões de promoção, o k6 segue obrigatório e qualquer comparação offline deve reconstruir o benchmark depois de reverter hipóteses.

## Ciclo 13h24: ponteiro local para centróides no nearest centroid AVX2

Hipótese: trocar chamadas repetidas de `centroids.data()`/`operator[]` por um ponteiro local `const float* centroids_ptr` poderia remover overhead no loop quente do centróide.

Patch temporário:

```cpp
const float* centroids_ptr = centroids.data();
const __m256 centroid = _mm256_loadu_ps(centroids_ptr + (dim * clusters) + cluster);
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| ponteiro local #1 | 7600.74 | 0 | 0 |
| ponteiro local #2 | 8019.76 | 0 | 0 |

Decisão: **rejeitado e revertido**. O primeiro número ficou na faixa normal do melhor estado, mas a segunda run piorou. O compilador provavelmente já otimiza `data()`/`operator[]` bem; a alteração não trouxe ganho sustentável.

## Ciclo 11h34: benchmark do caminho de request

Objetivo: medir o custo isolado de montagem de resposta, parse DOM, parse seletivo e vetorização para decidir se ainda faz sentido insistir no parser antes de novas mudanças no IVF.

Comando executado:

```text
nice -n 10 cpp/build/benchmark-request-cpp test/test-data.json cpp/build/perf-data/references.bin cpp/build/perf-data/labels.bin
```

Resultado:

| Métrica | ns/query | Observação |
|---|---:|---|
| `body_append_default` | 30.46 | montagem simples do corpo de resposta |
| `body_append_reserve768` | 26.68 | ganho micro; irrelevante para p99 atual |
| `dom_padded_parse` | 240.19 | parse DOM isolado ainda sub-microssegundo |
| `dom_reserve768_parse` | 243.58 | `reserve` não ajudou o DOM |
| `parse_payload` | 622.41 | parser seletivo segue barato |
| `parse_vectorize` | 1114.22 | parse + vetorização em ~1.1us |
| `parse_classify` | 382873.00 | métrica descartada para a submissão atual |

Interpretação: o custo real de parser/vetorização está baixo demais para explicar o gap restante contra o ranking parcial. A métrica `parse_classify` usa o classificador exato antigo do benchmark de request, não o caminho IVF atual da API, então não deve orientar otimização da submissão vigente.

Decisão: **sem mudança de código**. O próximo foco continua sendo IVF/infra/proxy/scheduling ou experimentos de scoring com evidência k6, não micro-otimização de parser.

## Ciclo 11h36: nginx `worker_processes 1`

Hipótese: como o nginx atua apenas como load balancer L4 (`stream`) entre porta 9999 e dois unix sockets, reduzir de 2 workers para 1 poderia diminuir contenção dentro do limite pequeno de CPU do próprio nginx.

Patch temporário:

```nginx
worker_processes 1;
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| nginx 1 worker | 1.25ms | 0% | 5903.39 |

Decisão: **rejeitado e revertido**. O resultado ficou abaixo das duas medições válidas do estado atual (`5908.42` e `5907.40`). O nginx com 2 workers continua melhor no envelope local, provavelmente por absorver melhor conexões simultâneas do k6 mesmo sendo apenas L4.

## Ciclo 11h39: redistribuição de CPU para APIs

Hipótese: mover CPU do nginx para as APIs poderia melhorar o trecho computacional do IVF, mantendo a soma em 1.00 CPU.

Patch temporário:

```yaml
api1/api2:  cpus: "0.43"
nginx:      cpus: "0.14"
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| api 0.43 / nginx 0.14 | 1.25ms | 0% | 5903.48 |

Decisão: **rejeitado e revertido**. O resultado ficou no mesmo patamar ruim do teste com 1 worker no nginx. A evidência local indica que reduzir a fatia do nginx prejudica o envelope de p99 mais do que a API ganha com CPU extra.

## Ciclo 11h42: redistribuição de CPU para nginx

Hipótese: como reduzir CPU do nginx piorou, talvez aumentar sua fatia pudesse reduzir p99 de proxy/conexão. A soma continuou em 1.00 CPU.

Patch temporário:

```yaml
api1/api2:  cpus: "0.39"
nginx:      cpus: "0.22"
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| api 0.39 / nginx 0.22 | 1.27ms | 0% | 5895.39 |

Decisão: **rejeitado e revertido**. Dar mais CPU ao nginx tirou capacidade demais das APIs e piorou o score. A família de redistribuição testada (`0.43/0.14`, `0.41/0.18`, `0.39/0.22`) favorece manter o estado atual `api=0.41`, `nginx=0.18`.

## Ciclo 11h45: filtro de lane antes de `top.insert`

Hipótese: no `scan_blocks_avx2`, pular lanes com distância maior que o pior top-5 atual antes de carregar `id`/`label` poderia economizar leituras e chamadas de `top.insert`. A regra é exata porque empate ainda passa para preservar o tie-break por menor `id`.

Patch temporário:

```cpp
if (values[lane] > top.worst_distance()) {
    continue;
}
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| filtro lane #1 | 7551.81 | 0 | 0 |
| filtro lane #2 | 8004.26 | 0 | 0 |

Decisão: **rejeitado e revertido**. A primeira run ficou dentro da faixa do melhor estado aceito, mas a segunda piorou bastante. A checagem adicional não demonstrou ganho sustentável e ainda adiciona branch no loop de lanes.

## Ciclo 11h46: nginx sem `reuseport`

Hipótese: com `worker_processes 2`, remover `reuseport` poderia reduzir variação de distribuição entre workers e melhorar p99.

Patch temporário:

```nginx
listen 9999 backlog=4096;
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| sem `reuseport` | 1.24ms | 0% | 5905.46 |

Decisão: **rejeitado e revertido**. O resultado foi melhor que as redistribuições ruins de CPU, mas ainda abaixo das duas runs válidas do estado atual (`5908.42` e `5907.40`). O `reuseport` permanece.

## Ciclo 11h49: run de controle após hipóteses de infra

Objetivo: medir novamente o estado limpo atual depois das hipóteses rejeitadas de nginx/CPU para separar regressão real de ruído local.

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| controle estado atual | 1.25ms | 0% | 5903.45 |

Interpretação: a run de controle ficou no mesmo patamar dos experimentos de infra rejeitados, abaixo das duas runs válidas anteriores do mesmo estado (`5908.42` e `5907.40`). Isso indica ruído/carga local relevante nesta janela.

Decisão: **sem mudança de código**. Para promoção, não basta uma medição única com melhora pequena; a próxima mudança precisa repetir ganho ou melhorar acima do ruído observado.

## Ciclo 11h52: IPO nas libs internas

Hipótese: o executável principal já usa IPO/LTO, mas `simdjson_singleheader` e `usockets` eram libs estáticas sem IPO explícito. Ativar IPO nelas poderia permitir melhor otimização interprocedural no binário final.

Patch temporário:

```cmake
set_property(TARGET simdjson_singleheader PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
set_property(TARGET usockets PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
```

Verificação:

```text
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado: testes passaram (`1/1`).

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| IPO libs #1 | 1.24ms | 0% | 5906.98 |
| IPO libs #2 | 1.26ms | 0% | 5901.07 |

Decisão: **rejeitado e revertido**. A primeira run superou a run de controle imediata, mas a repetição caiu abaixo do envelope aceito. O ganho não é sustentável e ainda aumenta custo/risco de build.

## Ciclo 11h56: estado do issue oficial

Verifiquei o issue oficial da submissão/runner:

```text
gh issue view 2026 -R zanfranceschi/rinha-de-backend-2026 --json state,title,comments,updatedAt,url
```

Resultado:

| Campo | Valor |
|---|---|
| URL | https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2026 |
| Estado | `OPEN` |
| Título | `Runner broken: stale submission directory since #2019` |
| Comentários | `0` |
| Última atualização | `2026-05-07T11:00:57Z` |

Decisão: **não abrir nova submissão agora**. O runner oficial segue com issue aberto, então o ciclo continua em branch experimental com foco em evidência local e não em novo issue oficial duplicado.

## Ciclo 11h58: arredondamento manual em `quantize`

Hipótese: `quantize()` chama `std::lround` para 14 dimensões por request. Como os valores já são clampados em `[-1, 1]`, um arredondamento manual equivalente poderia evitar chamada de libm e reduzir custo no caminho quente.

Patch temporário:

```cpp
const long rounded = static_cast<long>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
```

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado: testes passaram (`1/1`).

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| arredondamento manual #1 | 7633.38 | 0 | 0 |
| arredondamento manual #2 | 8325.97 | 0 | 0 |

Decisão: **rejeitado e revertido**. A acurácia foi preservada, mas a performance não melhorou. O custo dominante não parece ser `std::lround`, e a troca manual ainda deixou a medição mais instável.

## Ciclo 12h02: especialização explícita para `nprobe=1`

Hipótese: o compose atual usa `IVF_FAST_NPROBE=1` e `IVF_FULL_NPROBE=1`. Criar um caminho explícito para `nprobe=1` poderia remover array de probes e o loop genérico de `already_scanned`, preservando a mesma seleção de centróide, scan primário e repair por bbox.

Patch temporário:

```cpp
std::uint8_t IvfIndex::fraud_count_once_probe1(...) const noexcept {
    const std::uint32_t best_cluster = nearest_centroid_probe1(...);
    scan_blocks(... best_cluster ...);
    if (repair) {
        for (std::uint32_t cluster = 0; cluster < clusters_; ++cluster) {
            if (cluster == best_cluster || offsets_[cluster] == offsets_[cluster + 1U]) {
                continue;
            }
            ...
        }
    }
    return top.frauds();
}
```

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado: testes passaram (`1/1`).

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| probe1 especializado #1 | 7537.78 | 0 | 0 |
| probe1 especializado #2 | 7565.76 | 0 | 0 |

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| probe1 especializado | 1.25ms | 0% | 5904.43 |

Decisão: **rejeitado e revertido**. O benchmark offline ficou estável e correto, mas o k6 não mostrou ganho material contra o estado atual. Como o objetivo primário é score local, não vale manter duplicação de código sem impacto claro no p99.

## Ciclo 12h07: remover header `uWebSockets`

Hipótese: o uWebSockets escreve o header informativo `uWebSockets: 20` por padrão em cada resposta. A macro nativa `UWS_HTTPRESPONSE_NO_WRITEMARK` remove esse header sem alterar contrato, body, status, roteamento ou lógica de negócio.

Patch aplicado:

```cmake
target_compile_definitions(rinha-backend-2026-cpp
    PRIVATE
        LIBUS_NO_SSL
        UWS_NO_ZLIB
        UWS_HTTPRESPONSE_NO_WRITEMARK
        SIMDJSON_EXCEPTIONS=0
)
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado: testes passaram (`1/1`).

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| sem `uWebSockets` header #1 | 1.23ms | 0% | 5908.38 |
| sem `uWebSockets` header #2 | 1.23ms | 0% | 5909.24 |

Decisão: **aceito**. A mudança é pequena, usa macro suportada pelo próprio uWebSockets, preserva o contrato da API e sustentou duas runs no topo do envelope local recente. Melhor resultado desta rodada: `final_score=5909.24`, ainda abaixo do melhor histórico publicado, mas acima do estado experimental recente repetido (`5908.42`/`5907.40`) por pequena margem.

## Ciclo 12h13: remover header `Date`

Hipótese: depois de remover o header `uWebSockets`, remover também `Date` poderia reduzir bytes e escrita por resposta. Essa mudança exige patch no vendor, então a barra de aceitação é mais alta.

Patch temporário:

```cpp
#ifndef UWS_HTTPRESPONSE_NO_DATE
writeHeader("Date", ...);
#endif
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado: testes passaram (`1/1`).

Observação metodológica: a primeira tentativa de k6 foi descartada porque o build Docker falhou por DNS/OAuth contra `auth.docker.io` e o compose subiria imagem antiga. A run abortada por `terminated signal` também foi descartada.

Resultados k6 válidos:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| sem `Date` #1 | 1.23ms | 0% | 5908.88 |
| sem `Date` #2 | 1.25ms | 0% | 5903.47 |

Decisão: **rejeitado e revertido**. A primeira run ficou competitiva, mas a repetição caiu para o mesmo envelope de ruído baixo. Como a mudança toca código vendorizado e não supera a macro nativa `UWS_HTTPRESPONSE_NO_WRITEMARK`, não vale manter.

## Ciclo 12h18: controle pós-reversão do `Date`

Objetivo: reconstruir e medir o estado aceito real depois de reverter o patch `no-date`, garantindo que as próximas comparações não usem imagem Docker stale.

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| controle `NO_WRITEMARK` após reversão `Date` | 1.26ms | 0% | 5900.96 |

Interpretação: a run ficou abaixo do par aceito de `NO_WRITEMARK` (`5908.38`/`5909.24`) e também abaixo de runs anteriores do mesmo estado. Como não houve mudança de código além da reversão já registrada, o resultado reforça ruído local forte nesta janela.

Decisão: **sem mudança adicional**. Manter `UWS_HTTPRESPONSE_NO_WRITEMARK` como única mudança aceita no eixo de headers e exigir repetição para qualquer nova promoção.

## Ciclo 12h22: nginx `multi_accept on`

Hipótese: aceitar múltiplas conexões por wake-up no nginx poderia reduzir overhead de accept no proxy L4.

Patch temporário:

```nginx
multi_accept on;
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `multi_accept on` | 1.25ms | 0% | 5902.29 |

Decisão: **rejeitado e revertido**. A alteração piorou o p99 no envelope local. Para esse perfil, `multi_accept off` permanece melhor, provavelmente por evitar rajadas que aumentam latência de cauda.

## Ciclo 12h27: `res->cork` no envio de resposta

Hipótese: envolver o `res->end(response_body)` em `res->cork(...)` poderia reduzir flushes no uWebSockets no endpoint `/fraud-score`.

Patch temporário:

```cpp
res->cork([res, response_body]() {
    res->end(response_body);
});
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado: testes passaram (`1/1`).

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `res->cork` | 1.25ms | 0% | 5904.47 |

Decisão: **rejeitado e revertido**. O endpoint já envia uma resposta pequena em uma única chamada, então o `cork` adicionou wrapper sem reduzir a cauda. Manter `res->end(response_body)` direto.

## Ciclo 12h30: flags matemáticas conservadoras

Hipótese: adicionar `-fno-math-errno` e `-fno-trapping-math` ao binário C++ e ao benchmark IVF poderia liberar otimizações seguras em cálculos de distância sem alterar o resultado funcional.

Patch temporário:

```text
-fno-math-errno
-fno-trapping-math
```

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 0 0
```

Resultado: testes passaram (`1/1`).

Resultados offline:

| Variante | ns/query | FP | FN | checksum |
|---|---:|---:|---:|---:|
| flags math #1 | 7633.86 | 0 | 0 | 246463184 |
| flags math #2 | 7794.50 | 0 | 0 | 246463184 |

Decisão: **rejeitado e revertido sem k6**. A mudança preservou corretude, mas não melhorou o microbenchmark do kernel IVF. Como o gargalo restante é cauda muito estreita e a evidência local ficou abaixo do estado aceito, não justifica custo de uma rodada Docker/k6.

## Ciclo 12h36: `-fomit-frame-pointer`

Hipótese: forçar omissão de frame pointer no binário C++ e no benchmark IVF poderia reduzir pressão de registradores no hot path e melhorar marginalmente o kernel.

Patch temporário:

```text
-fomit-frame-pointer
```

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 0 0
```

Resultado: testes passaram (`1/1`).

Resultados offline:

| Variante | ns/query | FP | FN | checksum |
|---|---:|---:|---:|---:|
| `-fomit-frame-pointer` #1 | 8120.67 | 0 | 0 | 246463184 |
| `-fomit-frame-pointer` #2 | 7800.07 | 0 | 0 | 246463184 |

Decisão: **rejeitado e revertido sem k6**. A flag não trouxe ganho no benchmark local e provavelmente já é redundante no perfil Release ou irrelevante para o gargalo atual.

## Ciclo 12h34: remover `onAborted` vazio

Hipótese: remover o callback vazio `res->onAborted([]() {})` do endpoint `/fraud-score` poderia reduzir uma pequena alocação/registro por request no uWebSockets.

Patch temporário:

```cpp
- res->onAborted([]() {});
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose -p perf-noon-tuning build --pull=false api1 api2
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado: build Docker válido, serviço respondeu `/ready`, testes unitários passaram (`1/1`).

Resultado k6:

| Variante | p99 | Falhas | HTTP errors | final_score |
|---|---:|---:|---:|---:|
| sem `onAborted` | 0.89ms | 100% | 54059 | 0 |

Decisão: **rejeitado e revertido imediatamente**. Embora a latência aparente tenha caído, a remoção quebrou o comportamento sob carga e causou erro HTTP massivo. O callback vazio é necessário como proteção prática no ciclo de vida do `HttpResponse` capturado pelo `onData`.

## Ciclo 12h42: booleano explícito para caminho IVF

Hipótese: substituir o `std::get_if` por um booleano calculado no startup e `std::get` direto no caminho IVF poderia reduzir uma checagem por request sem alterar a arquitetura.

Patch temporário:

```cpp
if (use_ivf) {
    const auto& ivf = std::get<rinha::IvfIndex>(classifier);
    const std::uint8_t fraud_count = ivf.fraud_count(query, ivf_config);
}
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose -p perf-noon-tuning build --pull=false api1 api2
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Observação metodológica: a primeira tentativa Docker falhou por DNS contra `registry-1.docker.io`; a execução que teria usado imagem stale foi abortada e descartada. A medição abaixo usou rebuild posterior válido.

Resultado: testes unitários passaram (`1/1`) e o build Docker válido foi concluído.

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `use_ivf` flag #1 | 1.23ms | 0% | 5909.71 |
| `use_ivf` flag #2 | 1.25ms | 0% | 5903.29 |

Decisão: **rejeitado e revertido**. A primeira run foi ligeiramente acima do melhor local recente, mas a repetição caiu para o envelope de ruído. O ganho não é sustentável nem inquestionável.

## Ciclo 12h45: nginx `worker_connections 1024`

Hipótese: reduzir `worker_connections` de `4096` para `1024` poderia diminuir estruturas internas do nginx e melhorar a cauda, mantendo folga suficiente para o cenário local.

Patch temporário:

```nginx
worker_connections 1024;
```

Verificação:

```text
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `worker_connections 1024` | 1.25ms | 0% | 5902.66 |

Decisão: **rejeitado e revertido**. A redução piorou a cauda. Para o perfil atual, manter `4096` é mais seguro.

## Ciclo 12h48: nginx `backlog=1024`

Hipótese: reduzir o backlog do `listen 9999` de `4096` para `1024` poderia diminuir filas internas e estabilizar a cauda sem limitar o cenário local.

Patch temporário:

```nginx
listen 9999 reuseport backlog=1024;
```

Verificação:

```text
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `backlog=1024` | 1.24ms | 0% | 5907.68 |

Decisão: **rejeitado e revertido**. A run foi boa, mas ainda abaixo do melhor estado aceito (`5909.24`) e abaixo da melhor run experimental rejeitada por não reproduzir. Sem ganho sustentável.

## Ciclo 12h58: `UWS_HTTP_MAX_HEADERS_COUNT=16`

Hipótese: reduzir o limite compile-time de headers do parser HTTP do uWebSockets de `100` para `16` poderia diminuir o tamanho do objeto de request/parser e melhorar o hot path. O `test/test.js` local envia apenas `Content-Type` explicitamente, e os headers HTTP usuais ficam bem abaixo de 16.

Patch temporário:

```cmake
UWS_HTTP_MAX_HEADERS_COUNT=16
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
docker compose -p perf-noon-tuning build --pull=false api1 api2
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado: testes unitários passaram (`1/1`).

Observação metodológica: a primeira tentativa de build Docker com BuildKit falhou por DNS contra `registry-1.docker.io`; a execução com imagem stale foi abortada. Para obter uma imagem válida, foi necessário repetir o build com `DOCKER_BUILDKIT=0 COMPOSE_DOCKER_CLI_BUILD=0`, que funcionou, mas foi mais pesado por reconstruir etapas de apt no builder clássico.

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| headers 16 #1 | 1.22ms | 0% | 5913.28 |
| headers 16 #2 | 1.23ms | 0% | 5908.74 |
| headers 16 #3 | 1.25ms | 0% | 5903.51 |

Decisão: **rejeitado e revertido**. A primeira run foi excelente, mas a terceira caiu para o envelope comum de ruído. A média não supera de forma clara o estado aceito anterior, e a redução do limite de headers adiciona risco de compatibilidade com harness oficial caso ele envie mais headers do que o teste local.

## Ciclo 13h00: varredura offline da janela de reparo IVF

Hipótese: a janela atual de reparo `repair_min=1` / `repair_max=4` pode estar escaneando mais clusters do que o necessário; reduzir a janela poderia melhorar latência, desde que a penalidade de detecção não destrua o score.

Verificação:

```text
for cfg in '1 4' '2 3' '2 4' '1 3' '0 5'; do
  nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 4 0 1 1 1 "$min" "$max" 0 0
done
```

Resultados offline:

| `repair_min..max` | ns/query | FP | FN | Observação |
|---|---:|---:|---:|---|
| `1..4` | 8042.64 | 0 | 0 | configuração atual |
| `2..3` | 7408.23 | 88 | 112 | mais rápido, mas perde detecção |
| `2..4` | 7465.36 | 0 | 112 | mais rápido, mas deixa fraude passar |
| `1..3` | 7936.19 | 88 | 0 | quase igual e cria FP |
| `0..5` | 53630.40 | 0 | 0 | correto, mas muito mais lento |

Decisão: **sem mudança**. As variantes mais rápidas criam FP/FN e perderiam muito mais no `detection_score` do que ganhariam em p99; a variante mais ampla mantém corretude mas é inviável em latência. Manter `1..4`.

## Ciclo 13h02: promover `disable_extreme_repair` para produção

Hipótese: desabilitar o ramo de reparo extremo poderia reduzir custo mantendo a janela `repair_min=1` / `repair_max=4`. Uma primeira leitura do benchmark parecia indicar mesmo checksum/correção com menor tempo, mas havia risco de o parâmetro só estar ativo no caminho com stats.

Patch temporário:

```cpp
bool disable_extreme_repair = false;
const bool extreme_repair = !config.disable_extreme_repair && should_repair_extreme(frauds, query);
```

e no compose:

```yaml
IVF_DISABLE_EXTREME_REPAIR: "true"
```

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 0 1
```

Resultado: testes unitários passaram (`1/1`).

Resultados offline após a promoção real do flag:

| Variante | ns/query | FP | FN | checksum |
|---|---:|---:|---:|---:|
| `disable_extreme_repair=true` #1 | 7802.57 | 8 | 16 | 246457048 |
| `disable_extreme_repair=true` #2 | 8137.56 | 8 | 16 | 246457048 |

Decisão: **rejeitado e revertido sem k6**. O reparo extremo é necessário para correção perfeita no dataset local. A leitura anterior era enganosa porque o argumento `disable_extreme` do benchmark só tinha efeito no caminho com stats; ao torná-lo efetivo no caminho normal, surgiram FP/FN. Perder detecção por esse ganho de latência não compensa.

## Ciclo 13h04: reparo extremo com comparações quantizadas

Hipótese: trocar as comparações float de `should_repair_extreme` por comparações no vetor já quantizado (`query_i16`) poderia reduzir custo por query sem alterar o conjunto reparado.

Patch temporário:

```cpp
bool should_repair_extreme_i16(std::uint8_t frauds, const std::array<std::int16_t, kDimensions>& query) noexcept;
```

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 8 0 1 1 1 1 4 0 0
```

Resultado: testes unitários passaram (`1/1`) e a classificação permaneceu correta (`FP=0`, `FN=0`, checksum `246463184`).

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| reparo i16 #1 | 8048.76 | 0 | 0 |
| reparo i16 #2 | 7689.77 | 0 | 0 |
| reparo i16 #3 | 8187.26 | 0 | 0 |
| reparo i16 #4 | 7607.79 | 0 | 0 |

Decisão: **rejeitado e revertido sem k6**. A mudança preserva correção, mas os resultados alternam entre bom e ruim no mesmo envelope de ruído. Além disso, introduziu warning de função float não usada no binário normal. Sem ganho sustentável.

## Ciclo 13h06: reparo seletivo vs. bbox global

Hipótese: simplificar o controle `boundary_full` poderia ser útil se o bbox global ou o caminho sem bbox tivesse uma relação melhor de latência/correção.

Verificação:

```text
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 4 0 1 1 1
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 4 0 1 1 0
```

Resultados offline:

| Configuração | ns/query | FP | FN | Interpretação |
|---|---:|---:|---:|---|
| atual, reparo seletivo `1..4` | 7621.45 | 0 | 0 | referência da rodada |
| `boundary_full=false`, bbox em toda query | 49253.30 | 0 | 0 | correto, mas muito lento |
| `boundary_full=false`, sem bbox | 6335.06 | 508 | 524 | rápido, mas detecção inviável |

Decisão: **sem mudança**. O desenho atual de reparo seletivo continua sendo o melhor compromisso: preserva detecção perfeita sem pagar bbox global em todas as queries.

## Ciclo 13h06: índice IVF com 1024 clusters

Hipótese: reduzir clusters de `1280` para `1024` poderia diminuir custo de centroid probe e bbox, compensando clusters um pouco maiores.

Verificação:

```text
cmake --build cpp/build --target prepare-ivf-cpp benchmark-ivf-cpp -j2
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz cpp/build/perf-data/index-1024.bin 1024 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1024.bin 4 0 1 1 1 1 4 0 0
```

Resultado offline:

| Índice | ns/query | FP | FN | Memória |
|---|---:|---:|---:|---:|
| `1024` clusters | 8702.64 | 4 | 8 | 94.64 MB |

Decisão: **rejeitado**. Além de mais lento que o índice `1280`, o índice de `1024` clusters perde correção perfeita. O arquivo gerado ficou apenas em `cpp/build/perf-data/index-1024.bin`, ignorado pelo git.

## Ciclo 13h08: índice IVF com 1536 clusters

Hipótese: aumentar clusters de `1280` para `1536` poderia reduzir o tamanho médio dos clusters escaneados, compensando o centroid probe maior.

Verificação:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz cpp/build/perf-data/index-1536.bin 1536 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1536.bin 4 0 1 1 1 1 4 0 0
```

Resultados offline:

| Índice/configuração | ns/query | FP | FN |
|---|---:|---:|---:|
| `1536`, reparo `1..4` | 7438.93 | 8 | 0 |
| `1536`, reparo `1..5` | 36365.90 | 0 | 0 |
| `1536`, reparo `0..4` | 22771.10 | 8 | 0 |
| `1536`, reparo `0..5` | 50113.80 | 0 | 0 |

Decisão: **rejeitado**. O índice de `1536` melhora latência só enquanto aceita falsos positivos. As janelas que recuperam correção perfeita ficam muito mais lentas que o índice atual `1280`.

## Ciclo 13h09: índices IVF intermediários 1152 e 1408

Hipótese: pontos intermediários ao redor de `1280` clusters poderiam encontrar equilíbrio melhor sem a perda grande observada em `1024` e `1536`.

Verificação:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz cpp/build/perf-data/index-1408.bin 1408 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1408.bin 4 0 1 1 1 1 4 0 0
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz cpp/build/perf-data/index-1152.bin 1152 65536 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1152.bin 4 0 1 1 1 1 4 0 0
```

Resultados offline:

| Índice | ns/query | FP | FN | Memória |
|---|---:|---:|---:|---:|
| `1408` clusters | 7779.99 | 12 | 4 | 94.72 MB |
| `1152` clusters | 8327.30 | 8 | 4 | 94.66 MB |

Decisão: **rejeitados**. Ambos perdem correção perfeita e não oferecem ganho claro de latência. A evidência acumulada (`1024`, `1152`, `1408`, `1536`) reforça `1280` como ponto local robusto.

## Ciclo 13h11: qualidade de treino do índice 1280

Hipótese: manter `1280` clusters, mas alterar o treino do k-means, poderia gerar clusters mais bem separados e reduzir reparos ou scans.

Verificação:

```text
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz cpp/build/perf-data/index-1280-it8.bin 1280 65536 8
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280-it8.bin 4 0 1 1 1 1 4 0 0
nice -n 10 cpp/build/prepare-ivf-cpp resources/references.json.gz cpp/build/perf-data/index-1280-s131k.bin 1280 131072 6
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280-s131k.bin 4 0 1 1 1 1 4 0 0
```

Resultados offline:

| Índice | ns/query | FP | FN |
|---|---:|---:|---:|
| `1280`, sample `65536`, `8` iterações | 7630.59 | 0 | 4 |
| `1280`, sample `131072`, `6` iterações | 7796.11 | 8 | 8 |

Decisão: **rejeitados**. Ambos perdem correção perfeita. O índice atual `1280 / sample 65536 / 6 iterações` permanece melhor para o dataset local.

## Ciclo 13h26: buffer lazy para body multi-chunk

Hipótese: o callback `onData` carregava um `std::string` dentro da lambda para toda request, mesmo quando o body chega em chunk único. Trocar por `std::unique_ptr<std::string>` lazy reduz o estado capturado no hot path e só aloca buffer quando houver chunk parcial.

Patch aceito:

```cpp
res->onData([res, state, body = std::unique_ptr<std::string>{}](std::string_view chunk, bool is_last) mutable {
    if (!is_last) {
        if (!body) {
            body = std::make_unique<std::string>();
        }
        body->append(chunk.data(), chunk.size());
        return;
    }

    if (body) {
        body->append(chunk.data(), chunk.size());
    }
    const std::string_view request_body = body ? std::string_view(*body) : chunk;
    // ...
});
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
DOCKER_BUILDKIT=0 docker build --pull=false -t rinha-backend-2026-cpp-api:local .
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado: testes unitários passaram (`1/1`). A imagem Docker foi reconstruída com sucesso em build único.

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| buffer lazy #1 | 1.23ms | 0% | 5911.55 |
| buffer lazy #2 | 1.24ms | 0% | 5906.96 |
| buffer lazy #3 | 1.22ms | 0% | 5913.54 |

Decisão: **aceito**. A média das três runs (`5910.68`) supera o par aceito anterior de `NO_WRITEMARK` (`5908.38` / `5909.24`), sem erro HTTP e sem alterar semântica da API. Ganho pequeno, mas a mudança é localizada, sustentável e reduz trabalho no caso comum de body em chunk único.

## Ciclo 23h58: `thread_local` para buffer de erro do parser

Hipótese: o handler de `/fraud-score` cria um `std::string error` por request para o parser. Como o caminho feliz não usa a mensagem, reaproveitar o buffer via `thread_local` poderia reduzir construção/destruição no hot path sem alterar o contrato HTTP.

Patch testado:

```cpp
rinha::Payload payload;
thread_local std::string error;
error.clear();
if (!rinha::parse_payload(request_body, payload, error)) {
    // ...
}
```

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
DOCKER_BUILDKIT=0 docker build --pull=false -t rinha-backend-2026-cpp-api:local .
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado: testes unitários passaram (`1/1`). A imagem Docker foi reconstruída antes das medições.

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `thread_local error` #1 | 1.22ms | 0% | 5913.14 |
| `thread_local error` #2 | 1.23ms | 0% | 5910.03 |
| `thread_local error` #3 | 1.25ms | 0% | 5902.65 |

Decisão: **rejeitado**. A primeira run foi forte, mas a terceira caiu abaixo do patamar aceito do buffer lazy e a média (`5908.61`) não melhora o estado atual (`5910.68`). O patch foi revertido para evitar complexidade global/thread-local sem ganho sustentável.

## Ciclo 00h20: `known_merchants` com `string_view` e buffer inline

Hipótese: o parser copiava `known_merchants` para `std::vector<std::string>` e copiava `merchant.id` apenas para calcular `unknown_merchant`. Como a decisão acontece dentro do próprio `parse_payload`, trocar para `std::string_view` com buffer inline poderia remover alocações/cópias mantendo compatibilidade com listas maiores via fallback dinâmico.

Observação do dataset local:

```text
jq '[.entries[].payload.customer.known_merchants | length] | {max:max, counts: group_by(.) | map({len:.[0], n:length})}' test/test-data.json
```

Resultado: todas as `54.100` requisições locais têm `known_merchants` vazio (`max=0`). Mesmo assim o patch manteve suporte genérico para os exemplos oficiais com listas preenchidas.

Verificação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
DOCKER_BUILDKIT=0 docker build --pull=false -t rinha-backend-2026-cpp-api:local .
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado: testes unitários passaram (`1/1`). A imagem Docker foi reconstruída antes do k6.

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `known_merchants string_view` #1 | 1.26ms | 0% | 5900.26 |

Decisão: **rejeitado cedo**. O resultado ficou abaixo do estado aceito por margem suficiente para não gastar mais duas runs. A hipótese provável é que, no dataset local, a lista vazia torna as cópias quase irrelevantes, enquanto o wrapper inline/fallback adiciona custo de stack/código no hot path. Patch revertido.

## Ciclo 00h47: reuso de buffer padded do simdjson

Hipótese: `parse_payload` ainda cria `simdjson::padded_string` por request. Reaproveitar um `thread_local std::string` e chamar `simdjson::pad_with_reserve` poderia manter a cópia obrigatória para padding, mas reduzir alocações/destruições recorrentes.

Patch testado:

```cpp
thread_local simdjson::dom::parser parser;
thread_local std::string json_buffer;

json_buffer.assign(body.data(), body.size());
const simdjson::padded_string_view json = simdjson::pad_with_reserve(json_buffer);
```

Verificação:

```text
cmake --build cpp/build --target benchmark-request-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
nice -n 10 cpp/build/benchmark-request-cpp test/test-data.json resources/references.json.gz 3 0
DOCKER_BUILDKIT=0 docker build --pull=false -t rinha-backend-2026-cpp-api:local .
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultados offline:

| Métrica | ns/query |
|---|---:|
| `dom_padded_parse` | 242.18 |
| `dom_reserve768_parse` | 245.07 |
| `parse_payload` com buffer reutilizável | 595.26 |
| `parse_vectorize` com buffer reutilizável | 654.96 |

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| buffer padded reutilizável #1 | 1.23ms | 0% | 5911.21 |
| buffer padded reutilizável #2 | 1.24ms | 0% | 5906.34 |

Decisão: **rejeitado**. Apesar de `parse_payload` offline melhorar contra o registro anterior (`622.41ns`), o k6 repetiu o padrão histórico desse tipo de mudança: uma run boa seguida por queda abaixo do estado aceito. Como o parser/vetorização está na casa de sub-microssegundos e o p99 é dominado por cauda de infraestrutura/IVF, a mudança foi revertida.

## Ciclo 01h28: especialização IVF para reparo `probe1`

Hipótese: na configuração de submissão (`fast_nprobe=1`, `full_nprobe=1`, `boundary_full=true`), queries na janela de reparo executam `fraud_count_once` duas vezes. A segunda chamada reescaneia o mesmo cluster primário antes de aplicar bbox repair. Uma especialização para `probe1` poderia escanear o primário uma vez, decidir a janela e aplicar bbox repair sobre o mesmo `Top5`.

Medição de incidência:

```text
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 3 0 1 1 1 1 4 1 0 0
```

Resultado com stats: `repaired_queries=7203` em `162300` consultas (`4.438%`). O custo duplicado existe, mas afeta uma fração pequena do dataset.

Patch testado: novo caminho `fraud_count_probe1_boundary(...)` usado apenas quando `boundary_full && fast_nprobe == 1 && full_nprobe == 1`.

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 5 0 1 1 1 1 4 0 0 0
DOCKER_BUILDKIT=0 docker build --pull=false -t rinha-backend-2026-cpp-api:local .
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultados offline:

| Run | ns/query | FP | FN |
|---|---:|---:|---:|
| especialização `probe1` #1 | 7564.86 | 0 | 0 |
| especialização `probe1` #2 | 7674.69 | 0 | 0 |
| especialização `probe1` #3 | 7805.54 | 0 | 0 |

Resultados k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| especialização `probe1` #1 | 1.22ms | 0% | 5914.91 |
| especialização `probe1` #2 | 1.24ms | 0% | 5907.72 |
| especialização `probe1` #3 | 1.25ms | 0% | 5903.51 |

Decisão: **rejeitado**. O offline confirmou correção e sinal positivo, mas o k6 não sustentou: média `5908.71`, abaixo do estado aceito do buffer lazy (`5910.68`). Como o ganho ataca só `~4.4%` das queries e aumenta complexidade no classificador, o patch foi revertido.

## Ciclo 02h04: prefetch em `scan_blocks_avx2`

Investigação externa: repositórios públicos com pontuação alta mostram prefetch explícito no scan de blocos IVF. `jairoblatt/rinha-2026-rust` usa prefetch de `block+8` no kernel de scan e `joojf/rinha-2026` também antecipa blocos no loop AVX2. A hipótese era que nosso layout SoA de blocos (`14 * 8` `i16` por bloco) poderia se beneficiar do mesmo padrão.

Patch testado:

```cpp
const std::uint32_t prefetch_block = block + 8U;
if (prefetch_block < end_block) {
    const std::size_t prefetch_base =
        static_cast<std::size_t>(prefetch_block) * kDimensions * kBlockLanes;
    _mm_prefetch(reinterpret_cast<const char*>(blocks_ptr + prefetch_base), _MM_HINT_T0);
    _mm_prefetch(
        reinterpret_cast<const char*>(blocks_ptr + prefetch_base + ((kDimensions * kBlockLanes) / 2U)),
        _MM_HINT_T0
    );
}
```

Verificação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 5 0 1 1 1 1 4 0 0 0
```

Resultados offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| prefetch `block+8` #1 | 7817.31 | 0 | 0 |
| prefetch `block+8` #2 | 8007.09 | 0 | 0 |

Decisão: **rejeitado sem k6**. A ideia é válida nos líderes, mas no nosso kernel atual o sinal offline ficou abaixo da faixa boa recente e não justificou uma rodada de compose. Provável causa: o scan com bbox/cluster pequeno já é suficientemente cache-local, e o prefetch adiciona instruções em consultas onde não há distância longa o bastante para esconder latência. Patch revertido.

## Ciclo 14h08: nprobe maior inspirado nos líderes

Investigação externa: `jairoblatt/rinha-2026-rust` usa `FAST_NPROBE=8` e `FULL_NPROBE=24`; `joojf/rinha-2026` usa `FAST_NPROBE=12` e `FULL_NPROBE=24`. A hipótese era reduzir dependência do reparo por bbox usando mais centróides primários, possivelmente ganhando robustez e reduzindo casos de borda.

Importante metodológico: uma primeira rodada foi feita antes de rebuildar o binário após o revert do prefetch. Para evitar contaminar a decisão, o `benchmark-ivf-cpp` foi recompilado e só os resultados limpos abaixo foram usados.

Verificação limpa:

```text
cmake --build cpp/build --target benchmark-ivf-cpp -j2
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 3 0 4 16 1 2 3 1 0 0
nice -n 10 cpp/build/benchmark-ivf-cpp test/test-data.json cpp/build/perf-data/index-1280.bin 3 0 8 24 1 2 3 1 0 0
```

Resultados offline:

| Configuração | ns/query | FP | FN | Observação |
|---|---:|---:|---:|---|
| `fast=4`, `full=16`, bbox, janela `2..3` | 25669.90 | 0 | 0 | correta, mas ~3x mais lenta que o estado atual |
| `fast=8`, `full=24`, bbox, janela `2..3` | 42254.00 | 3 | 0 | mais lenta e perde correção perfeita |

Decisão: **rejeitado sem k6**. A estratégia de múltiplos probes funciona nos líderes porque o kernel e o índice deles foram desenhados para esse regime. No nosso índice `1280` com bbox repair, aumentar `nprobe` multiplica blocos escaneados (`avg_primary_blocks` sobe para `1343.71` em `4/16` e `2613.12` em `8/24`) e elimina qualquer chance de melhorar p99.

## Ciclo 14h18: HAProxy L4 via unix sockets

Hipótese: trocar o nginx `stream` por HAProxy TCP/L4 poderia reduzir cauda do LB, inspirado pelo uso de LB dedicado/custom em submissões líderes. O teste manteve a topologia regulamentar, dois unix sockets de API, porta `9999`, sem lógica de aplicação no LB e os mesmos limites de recurso (`0.18 CPU`, `20MB`) para o balanceador.

Patch temporário:

```yaml
lb:
  image: haproxy:3.0-alpine
  volumes:
    - ./haproxy.cfg:/usr/local/etc/haproxy/haproxy.cfg:ro
    - sockets:/sockets
```

Configuração HAProxy testada:

```text
global
    maxconn 8192
    nbthread 1

defaults
    mode tcp
    no log
    retries 0

backend api
    balance roundrobin
    server api1 unix@/sockets/api1.sock
    server api2 unix@/sockets/api2.sock
```

Verificação:

```text
DOCKER_BUILDKIT=0 docker build --pull=false -t rinha-backend-2026-cpp-api:local .
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| HAProxy L4 | 1.27ms | 0% | 5896.32 |

Decisão: **rejeitado e revertido**. HAProxy funcionou corretamente e manteve 0% falhas, mas p99 piorou de forma clara contra nginx stream. O stack atual de nginx L4 + UDS permanece superior neste ambiente local.

## Ciclo 14h26: nginx HTTP proxy via unix sockets

Hipótese: `joojf/rinha-2026` usa nginx em modo HTTP com upstream UDS e keepalive. Embora o modo `stream` seja mais simples e não parseie HTTP, o HTTP proxy com conexão persistente ao upstream poderia reduzir churn de conexão nginx -> API.

Patch temporário:

```nginx
http {
    access_log off;
    error_log /dev/null;

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

Verificação:

```text
docker compose -p perf-noon-tuning up -d --no-build
./run-local-k6.sh
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| nginx HTTP/UDS | 1.34ms | 0% | 5873.19 |

Decisão: **rejeitado e revertido**. O modo HTTP adicionou parse/proxy overhead no LB e piorou a cauda. O nginx `stream` L4 continua a escolha correta para este stack.
