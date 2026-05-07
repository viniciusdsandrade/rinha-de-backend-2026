# Daily Report 2026-05-06

## Contexto

Continuação do ciclo `perf/noon-tuning` após a submissão oficial `#1714`.

Baseline efetivo da rodada:

| Referência | p99 | Falhas | Score | Observação |
|---|---:|---:|---:|---|
| Issue oficial `#1714` | 1.29ms | 0% | 5888.51 | Melhor submissão efetiva atual |
| Melhor validação local da imagem `submission-df6994a` | 1.23ms | 0% | 5908.68 | Melhor run local conhecida, ainda não reproduzida oficialmente |

Branch de investigação: `perf/noon-tuning`.

## Ciclo 23h40: encerramento do dia anterior

Verificações:

```bash
git status --short --branch
git log --oneline -5
```

Resultado:

- Branch limpa e sincronizada com `origin/perf/noon-tuning`.
- Último commit antes da nova rodada: `20db03e optimize single chunk request path`.
- O fast path para payload em chunk único ficou mantido como candidato experimental, mas ainda sem nova submissão oficial por ter resultado local misto.

## Ciclo 23h50: parser com `string_view` local para merchants

Hipótese: o parser ainda fazia cópias temporárias em `known_merchants` e `merchant.id`, mesmo que esses dados sejam usados apenas dentro de `parse_payload` para calcular `payload.known_merchant`. Trocar esses temporários para `std::string_view` poderia reduzir alocações/cópias sem alterar o `Payload` final.

Escopo testado:

- `known_merchants`: `std::vector<std::string>` -> `std::vector<std::string_view>`.
- `merchant_id`: `std::string` -> `std::string_view`.
- `Payload` final preservado com `std::string` para timestamps e MCC, evitando risco de lifetime após o retorno de `parse_payload`.

Validação funcional:

```bash
cmake --build cpp/build --target benchmark-request-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Observação de ferramenta:

- A tentativa inicial de rodar `benchmark-request-cpp` completo foi interrompida porque esse binário também entra no caminho pesado de `parse_classify` exato após as métricas de parser.
- Para não estressar a máquina sem necessidade, a decisão foi validar com k6 local, que mede o efeito real na stack IVF atual.

Resultado k6 local:

```bash
DOCKER_HOST=unix:///run/docker.sock docker compose up -d --build --force-recreate
DOCKER_HOST=unix:///run/docker.sock ./run-local-k6.sh
```

| Config | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| parser `string_view` local para merchants | 1.31ms | 0 | 0 | 0 | 5883.11 |

Decisão: **rejeitado e revertido**. A mudança é semanticamente segura e preserva acurácia, mas piorou a p99 contra a submissão oficial `#1714` (`1.29ms / 5888.51`) e contra as melhores runs locais da variante atual. A provável explicação é que a economia de cópia temporária é pequena demais perto do custo de simdjson + vectorize + IVF, e a alteração de tipos não melhora o caminho crítico real.

Próxima linha mais promissora:

- Evitar micro-otimizações de parser que não removam a cópia principal para `simdjson::padded_string`.
- Priorizar mudanças estruturais de maior impacto: parser manual seletivo integrado à vetorização, ou reduzir overhead de LB/API inspirado nos líderes `io_uring`/LB custom.

## Ciclo 00h20: `-march=x86-64-v3` em libs locais

Hipótese: o binário principal já compila com `-mavx2 -mfma -march=x86-64-v3`, mas as bibliotecas estáticas locais `usockets` e `simdjson_singleheader` não recebiam explicitamente `-march=x86-64-v3`. Como ambas participam do hot path de rede/JSON, testar o mesmo requisito de CPU nelas poderia reduzir overhead.

Escopo testado:

- `target_compile_options(usockets PRIVATE -march=x86-64-v3)`.
- `target_compile_options(simdjson_singleheader PRIVATE -march=x86-64-v3)`.
- Nenhuma alteração de algoritmo, dados, compose ou API.

Validação:

```bash
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
DOCKER_HOST=unix:///run/docker.sock docker compose up -d --build --force-recreate
DOCKER_HOST=unix:///run/docker.sock ./run-local-k6.sh
```

Resultado funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultado k6 local:

| Config | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `-march=x86-64-v3` em `usockets` + `simdjson_singleheader` | 2.76ms | 0 | 0 | 0 | 5558.95 |

Decisão: **rejeitado e revertido**. A acurácia permaneceu perfeita, mas a p99 degradou fortemente. Provável causa: a otimização explícita nas libs interfere mal com o perfil real de código/dispatch ou aumenta pressão de código/cache sem reduzir o gargalo dominante. Manter `-march=x86-64-v3` apenas no binário principal continua sendo o ponto validado.

## Ciclo 00h45: `-fno-rtti` no binário principal

Hipótese: o binário principal não usa `dynamic_cast` nem `typeid`; remover RTTI poderia reduzir metadado/código e, em cenário de p99 muito apertada, talvez aliviar levemente cache/instruções sem alterar semântica.

Escopo testado:

- Adicionado `-fno-rtti` apenas em `target_compile_options(rinha-backend-2026-cpp ...)`.
- `usockets`, `simdjson_singleheader`, tools e testes ficaram sem essa flag.
- Sem alteração de algoritmo, API, compose, recursos ou dados.

Validação funcional:

```bash
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais com imagem reconstruída:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `-fno-rtti` #1 | 1.19ms | 0 | 0 | 0 | 5924.00 |
| `-fno-rtti` #2 | 1.29ms | 0 | 0 | 0 | 5888.42 |
| `-fno-rtti` #3 | 1.24ms | 0 | 0 | 0 | 5907.94 |

Comparação:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Issue oficial `#1714` | 1.29ms | 0% | 5888.51 |
| Melhor local pré-`-fno-rtti` conhecido | 1.23ms | 0% | 5908.68 |
| Melhor local com `-fno-rtti` | 1.19ms | 0% | 5924.00 |

Decisão: **manter como candidato experimental, ainda sem submissão oficial**. O sinal é melhor do que os experimentos anteriores e preserva `0%` falhas, mas a sequência também mostra variância (`1.19 -> 1.29 -> 1.24`). Para virar nova submissão, precisa de validação por imagem pública e repetição acima da issue `#1714`; uma única run `1.19ms` não é suficiente para declarar ganho sustentável.

### Validação pública do candidato `-fno-rtti`

Após o push do commit `9ddccaf`, a workflow `Publish submission image` publicou a imagem pública `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission`.

Workflow:

- Run: `25474242896`.
- Resultado: sucesso.
- Duração: `1m20s`.

Validei a imagem pública por compose override temporário, sem alterar arquivos versionados:

```bash
DOCKER_HOST=unix:///run/docker.sock docker compose -f docker-compose.yml -f /tmp/rinha-public-override.yml -p perf-public-fno-rtti pull
DOCKER_HOST=unix:///run/docker.sock docker compose -f docker-compose.yml -f /tmp/rinha-public-override.yml -p perf-public-fno-rtti up -d --force-recreate
DOCKER_HOST=unix:///run/docker.sock ./run-local-k6.sh
```

Resultados públicos:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| imagem pública `submission` #1 | 1.15ms | 0 | 0 | 0 | 5940.80 |
| imagem pública `submission` #2 | 1.27ms | 0 | 0 | 0 | 5895.40 |
| imagem pública `submission` #3 | 1.28ms | 0 | 0 | 0 | 5894.23 |

Comparação contra a melhor submissão oficial atual:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Issue oficial `#1714` | 1.29ms | 0% | 5888.51 |
| Pior run pública `-fno-rtti` | 1.28ms | 0% | 5894.23 |
| Melhor run pública `-fno-rtti` | 1.15ms | 0% | 5940.80 |

Decisão: **promover para submissão oficial**. Diferente das runs locais isoladas, a imagem pública repetiu três vezes acima da issue oficial `#1714`, sempre com `0%` falhas. Próximo passo: publicar tag imutável `submission-9ddccaf`, atualizar a branch `submission` para essa imagem e abrir issue oficial na Rinha.

### Publicação oficial do candidato `-fno-rtti`

Tag imutável:

- Adicionada ao workflow `Publish submission image`: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-9ddccaf`.
- Commit de publicação/registro na branch experimental: `6eb703c`.
- Workflow de publicação: `25474524962`.
- Resultado da workflow: sucesso em `1m26s`.
- Manifest verificado: `linux/amd64` presente.

Branch oficial de entrega:

- Branch: `submission`.
- Commit: `a4ce657` (`point submission to no rtti image`).
- `docker-compose.yml` atualizado para `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-9ddccaf`.
- `docker compose config --quiet`: OK.

Validação final da branch `submission` com a imagem imutável:

| Fonte | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| branch `submission` / `submission-9ddccaf` | 1.28ms | 0 | 0 | 0 | 5891.27 |

Issue oficial aberta:

- URL: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2009`.
- Título: `rinha/test andrade-cpp-ivf`.
- Body: `https://github.com/viniciusdsandrade/rinha-de-backend-2026`.

Decisão: **submissão enviada**. A validação final ficou acima da issue `#1714` (`5888.51`) e preservou `0%` falhas. A expectativa realista é ganho oficial modesto se a engine cair perto de `1.27-1.28ms`, ou ganho forte se repetir a melhor run pública (`1.15ms`).

## Ciclo 01h10: tentativa `-fno-exceptions`

Hipótese: depois do ganho pequeno com `-fno-rtti`, testar `-fno-exceptions` no binário principal poderia reduzir metadados/caminhos frios e talvez melhorar marginalmente o perfil de cache/instruções.

Escopo testado:

- Adicionado `-fno-exceptions` apenas em `target_compile_options(rinha-backend-2026-cpp ...)`.
- Mantido `-fno-rtti`.
- Sem alteração de algoritmo, compose, dados ou contrato HTTP.

Resultado:

```text
cpp/src/main.cpp:57:14: error: exception handling disabled, use '-fexceptions' to enable
   57 |     } catch (...) {
      |              ^~~
```

Causa: o binário ainda possui `catch (...)` no parser de variáveis de ambiente (`uint_env_or_default`) ao redor de `std::stoul`. Seria possível reescrever esse trecho para parsing sem exceções, mas ele roda apenas em startup e não participa do hot path do `/fraud-score`.

Decisão: **rejeitado e revertido**. Não há evidência de ganho mensurável porque o experimento nem compila. A refatoração necessária para permitir `-fno-exceptions` é segura, mas provavelmente tem retorno prático nulo para p99; portanto não vale encerrar o dia abrindo esse risco.

## Ciclo 01h35: remover unwind tables do binário principal

Hipótese: manter exceções habilitadas, mas remover tabelas de unwind do binário principal (`-fno-unwind-tables` e `-fno-asynchronous-unwind-tables`), poderia reduzir metadados/caminhos frios sem tocar no contrato nem no hot path. Diferente de `-fno-exceptions`, essa mudança não exige reescrever código e deve preservar os `catch` existentes.

Escopo testado:

- Adicionado `-fno-unwind-tables`.
- Adicionado `-fno-asynchronous-unwind-tables`.
- Mantido `-fno-rtti`.
- Flags aplicadas apenas em `target_compile_options(rinha-backend-2026-cpp ...)`.
- Sem alteração de algoritmo, compose, recursos, índice IVF, parser, API ou dados.

Validação funcional:

```bash
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais com imagem reconstruída:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| unwind tables off #1 | 1.22ms | 0 | 0 | 0 | 5912.33 |
| unwind tables off #2 | 1.14ms | 0 | 0 | 0 | 5942.54 |
| unwind tables off #3 | 1.14ms | 0 | 0 | 0 | 5944.66 |

Comparação:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Issue oficial `#1714` | 1.29ms | 0% | 5888.51 |
| Submissão enviada `submission-9ddccaf` validação final | 1.28ms | 0% | 5891.27 |
| Pior run local unwind tables off | 1.22ms | 0% | 5912.33 |
| Melhor run local unwind tables off | 1.14ms | 0% | 5944.66 |

Decisão: **promover para candidato público**. A mudança é pequena, compila, preserva acurácia perfeita e as três runs locais ficaram acima da submissão atual. Próximo passo: publicar a imagem via workflow da branch experimental, validar a imagem pública e só então decidir se vira nova tag imutável para `submission`.

### Validação pública do candidato sem unwind tables

Commit candidato:

- `a5ef277` (`test no unwind tables app build`).

Workflow de publicação da imagem pública mutável:

- Run: `25475005868`.
- Resultado: sucesso.
- Duração: `1m49s`.
- Imagem publicada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission`.

Validação da imagem pública:

```bash
DOCKER_HOST=unix:///run/docker.sock docker compose -f docker-compose.yml -f /tmp/rinha-public-override.yml -p perf-public-unwind pull
DOCKER_HOST=unix:///run/docker.sock docker compose -f docker-compose.yml -f /tmp/rinha-public-override.yml -p perf-public-unwind up -d --force-recreate
DOCKER_HOST=unix:///run/docker.sock ./run-local-k6.sh
```

Observação operacional: o primeiro `curl /ready` recebeu `connection reset` por bater durante o startup. O retry posterior respondeu `ready`, e o k6 rodou sem erros HTTP.

Resultados públicos:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| imagem pública `submission`/unwind #1 | 1.14ms | 0 | 0 | 0 | 5943.01 |
| imagem pública `submission`/unwind #2 | 1.14ms | 0 | 0 | 0 | 5943.36 |

Comparação contra a submissão anterior:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Issue oficial `#1714` | 1.29ms | 0% | 5888.51 |
| Validação final `submission-9ddccaf` | 1.28ms | 0% | 5891.27 |
| Pior run pública sem unwind tables | 1.14ms | 0% | 5943.01 |

Decisão: **promover para tag imutável e branch `submission`**. O candidato tem sinal melhor que `submission-9ddccaf` tanto em runs locais quanto em runs públicas, sem regressão de acurácia. Próximo passo: publicar `submission-a5ef277`, atualizar `docker-compose.yml` da branch `submission` e reaproveitar a issue oficial aberta se ela ainda não tiver sido processada.

### Publicação oficial do candidato sem unwind tables

Tag imutável:

- Tag: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a5ef277`.
- Commit que adicionou a tag ao workflow e registrou a promoção: `e7bab71`.
- Workflow: `25475215641`.
- Resultado: sucesso.
- Duração: `1m29s`.
- Manifest verificado: `linux/amd64` presente.

Branch oficial de entrega:

- Branch: `submission`.
- Commit: `a2c45b9` (`point submission to no unwind image`).
- `docker-compose.yml` atualizado de `submission-9ddccaf` para `submission-a5ef277`.
- `docker compose config --quiet`: OK.
- Push para `origin/submission`: OK.

Validação final da branch `submission`:

| Fonte | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| branch `submission` / `submission-a5ef277` | 1.15ms | 0 | 0 | 0 | 5939.70 |

Estado da issue oficial:

- Issue: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2009`.
- Estado no momento da troca: `OPEN`.
- Comentários no momento da troca: nenhum.

Decisão: **submissão efetiva atualizada antes do processamento da issue**. Como a issue `#2009` ainda estava aberta e sem resultado, a branch `submission` agora aponta para a melhor imagem disponível. Se a engine clonar o estado atual do fork quando processar a issue, deve testar `submission-a5ef277`.

## Ciclo 02h15: `-fno-plt` no binário principal

Hipótese: adicionar `-fno-plt` ao binário principal poderia reduzir indireções em chamadas para símbolos de bibliotecas dinâmicas. A mudança é pequena, reversível e não altera semântica de negócio.

Escopo testado:

- Adicionado `-fno-plt`.
- Mantidos `-fno-unwind-tables`, `-fno-asynchronous-unwind-tables` e `-fno-rtti`.
- Flags aplicadas apenas em `target_compile_options(rinha-backend-2026-cpp ...)`.
- Sem alteração de algoritmo, parser, compose, recursos, índice IVF, API ou dados.

Validação funcional:

```bash
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j2
ctest --test-dir cpp/build --output-on-failure
```

Resultado funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais com imagem reconstruída:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `-fno-plt` #1 | 1.13ms | 0 | 0 | 0 | 5947.44 |
| `-fno-plt` #2 | 1.13ms | 0 | 0 | 0 | 5946.45 |
| `-fno-plt` #3 | 1.14ms | 0 | 0 | 0 | 5944.47 |

Comparação:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Branch `submission` / `submission-a5ef277` | 1.15ms | 0% | 5939.70 |
| Pior run local `-fno-plt` | 1.14ms | 0% | 5944.47 |
| Melhor run local `-fno-plt` | 1.13ms | 0% | 5947.44 |

Decisão: **promover para candidato público**. O ganho é pequeno, mas as três runs ficaram acima da branch `submission` recém-atualizada, sem regressão de acurácia. Próximo passo: publicar imagem pública, validar duas runs públicas e só então decidir se vale substituir `submission-a5ef277`.

### Validação pública do candidato `-fno-plt`

Commit candidato:

- `d8be840` (`test no plt app build`).

Workflow de publicação da imagem pública mutável:

- Run: `25475654615`.
- Resultado: sucesso.
- Duração: `1m16s`.
- Imagem publicada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission`.

Resultados públicos:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| imagem pública `submission`/`-fno-plt` #1 | 1.14ms | 0 | 0 | 0 | 5941.26 |
| imagem pública `submission`/`-fno-plt` #2 | 1.14ms | 0 | 0 | 0 | 5942.06 |

Comparação contra o candidato sem `-fno-plt`:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Pior pública sem `-fno-plt` (`submission-a5ef277`) | 1.14ms | 0% | 5943.01 |
| Melhor pública com `-fno-plt` | 1.14ms | 0% | 5942.06 |

Decisão: **rejeitado e revertido**. Localmente a flag parecia levemente positiva (`5944-5947`), mas no artefato público ela não superou a versão sem `-fno-plt`. Como a margem é pequena e o objetivo é performance sustentável, a melhor submissão permanece `submission-a5ef277` (`-fno-rtti` + sem unwind tables, sem `-fno-plt`).

## Ciclo 02h45: function/data sections + linker GC

Hipótese: compilar `usockets`, `simdjson_singleheader` e o binário principal com `-ffunction-sections -fdata-sections`, além de linkar o binário com `-Wl,--gc-sections`, poderia remover código morto e reduzir pressão de instruções/cache.

Escopo testado:

- `usockets`: `-ffunction-sections -fdata-sections`.
- `simdjson_singleheader`: `-ffunction-sections -fdata-sections`.
- `rinha-backend-2026-cpp`: `-ffunction-sections -fdata-sections`.
- Link do binário principal: `-Wl,--gc-sections`.
- Mantidos `-fno-rtti`, `-fno-unwind-tables` e `-fno-asynchronous-unwind-tables`.
- Sem alteração de algoritmo, parser, compose, índice IVF, API ou dados.

Validação funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultado k6 local com imagem reconstruída:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| sections + GC #1 | 1.15ms | 0 | 0 | 0 | 5940.37 |

Comparação:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Branch `submission` / `submission-a5ef277` | 1.15ms | 0% | 5939.70 |
| Pior pública sem `-fno-plt` (`submission-a5ef277`) | 1.14ms | 0% | 5943.01 |
| sections + GC | 1.15ms | 0% | 5940.37 |

Decisão: **rejeitado e revertido**. A mudança não quebrou funcionalidade, mas também não entregou ganho concreto. Como mexe em flags de libs e linker para retorno abaixo da melhor pública, não é sustentável manter.

## Ciclo 03h10: captura de `AppState` por ponteiro cru

Hipótese: evitar captura/cópia de `std::shared_ptr<AppState>` no hot path HTTP poderia remover uma pequena pressão de refcount/objeto nas lambdas de request do uWebSockets. O `shared_ptr` continua existindo no chamador e mantém o estado vivo durante `app.run()`, mas as lambdas passam a capturar apenas `const AppState*`.

Escopo testado:

- `run_server` recebe `const std::shared_ptr<AppState>& state_owner`.
- O ponteiro `const AppState* state = state_owner.get()` é criado uma vez antes de registrar as rotas.
- As lambdas de `POST /fraud-score` capturam `state` por valor como ponteiro cru.
- Sem alteração de algoritmo, parser, compose, índice IVF, API, dados ou resposta.

Validação funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais com imagem reconstruída:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| raw `AppState*` #1 | 1.14ms | 0 | 0 | 0 | 5944.24 |
| raw `AppState*` #2 | 1.13ms | 0 | 0 | 0 | 5946.23 |
| raw `AppState*` #3 | 1.14ms | 0 | 0 | 0 | 5942.89 |

Comparação:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Branch `submission` / `submission-a5ef277` | 1.15ms | 0% | 5939.70 |
| Pior pública sem `-fno-plt` (`submission-a5ef277`) | 1.14ms | 0% | 5943.01 |
| Melhor pública sem `-fno-plt` (`submission-a5ef277`) | 1.14ms | 0% | 5943.36 |
| Pior run local raw `AppState*` | 1.14ms | 0% | 5942.89 |
| Melhor run local raw `AppState*` | 1.13ms | 0% | 5946.23 |

Decisão: **promover para candidato público**. O ganho ainda é estreito e pode ser ruído, mas duas das três runs locais superaram a melhor validação pública da submissão atual e a mudança é pequena, sustentável e de baixo risco de correção. Próximo passo: publicar a imagem mutável, validar via artefato público e só então decidir se deve substituir `submission-a5ef277`.

### Validação pública do candidato raw `AppState*`

Commit candidato:

- `cd3e915` (`test raw app state capture`).

Workflow de publicação da imagem pública mutável:

- Run: `25476354724`.
- Resultado: sucesso.
- Duração: `1m24s`.
- Imagem publicada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission`.

Resultados públicos:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| imagem pública `submission`/raw `AppState*` #1 | 1.15ms | 0 | 0 | 0 | 5940.19 |
| imagem pública `submission`/raw `AppState*` #2 | 1.14ms | 0 | 0 | 0 | 5943.45 |
| imagem pública `submission`/raw `AppState*` #3 | 1.12ms | 0 | 0 | 0 | 5950.45 |

Comparação contra a submissão atual:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Branch `submission` / `submission-a5ef277` | 1.15ms | 0% | 5939.70 |
| Melhor pública sem raw `AppState*` (`submission-a5ef277`) | 1.14ms | 0% | 5943.36 |
| Melhor pública raw `AppState*` | 1.12ms | 0% | 5950.45 |

Decisão: **promover para tag imutável e branch `submission`**. A primeira pública ficou abaixo do melhor `a5ef277`, mas a segunda empatou/superou marginalmente e a terceira abriu ganho material. Como a alteração é pequena, sem impacto de contrato e com zero falhas em todas as runs, vale preparar `submission-cd3e915` e validar o compose final da branch `submission`.

### Promoção para `submission-cd3e915`

Tag imutável:

- Workflow: `25476605994`.
- Resultado: sucesso.
- Duração: `1m32s`.
- Manifest: `linux/amd64` presente para `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-cd3e915`.

Branch oficial do fork:

- Branch: `submission`.
- Commit: `46e9aeb` (`point submission to raw state image`).
- `docker-compose.yml`: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-cd3e915`.
- `docker compose config --quiet`: OK.
- Push: concluído em `origin/submission`.

Validação final do compose da branch `submission`:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| branch `submission` / `submission-cd3e915` #1 | 1.15ms | 0 | 0 | 0 | 5938.77 |
| branch `submission` / `submission-cd3e915` #2 | 1.14ms | 0 | 0 | 0 | 5944.96 |
| branch `submission` / `submission-cd3e915` #3 | 1.14ms | 0 | 0 | 0 | 5944.68 |

Decisão final do ciclo: **manter `submission-cd3e915` como melhor submissão preparada**. O resultado tem variância local, mas duas das três validações finais superaram o melhor público anterior de `submission-a5ef277` (`5943.36`) e a melhor pública do novo candidato chegou a `5950.45`. A issue oficial de teste permanece aberta (`#2009`), sem comentários até o momento, então a branch `submission` atualizada ainda deve ser a versão consumida quando a engine processar a solicitação.

## Ciclo 03h35: `known_merchants` sem alocação no parser

Hipótese: o parser copiava `customer.known_merchants` para `std::vector<std::string>` apenas para calcular `unknown_merchant`. Como o dataset local usa entre 2 e 5 merchants conhecidos por payload, trocar o caminho comum para `std::array<std::string_view, 8>` remove alocações/cópias por request sem alterar o contrato.

Investigação do dataset local:

```text
min=2, max=5, avg=3.4964 known_merchants por request
histograma: len=2 -> 13529, len=3 -> 13599, len=4 -> 13560, len=5 -> 13412
```

Escopo testado:

- `parse_customer` passa a preencher `std::array<std::string_view, 8>` e contador no caminho comum.
- Para preservar correção se o dataset oficial trouxer mais de 8 merchants conhecidos, há fallback de overflow em `std::vector<std::string_view>`.
- `parse_merchant` guarda `merchant.id` em `std::string_view` temporário em vez de `std::string`.
- `Payload` permanece igual; apenas a checagem final de `known_merchant` usa as views ainda válidas durante o parse.
- Sem alteração de algoritmo, compose, índice IVF, API, resposta ou dados.

Validação funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais com imagem reconstruída:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| fixed `known_merchants` #1 | 1.12ms | 0 | 0 | 0 | 5949.11 |
| fixed `known_merchants` #2 | 1.12ms | 0 | 0 | 0 | 5949.50 |
| fixed `known_merchants` #3 | 1.13ms | 0 | 0 | 0 | 5947.51 |
| fixed `known_merchants` + overflow seguro | 1.13ms | 0 | 0 | 0 | 5947.39 |

Comparação:

| Referência | p99 | Falhas | Score |
|---|---:|---:|---:|
| Branch `submission` / `submission-cd3e915` melhor final | 1.14ms | 0% | 5944.96 |
| Melhor pública raw `AppState*` | 1.12ms | 0% | 5950.45 |
| Pior run local fixed `known_merchants` | 1.13ms | 0% | 5947.39 |
| Melhor run local fixed `known_merchants` | 1.12ms | 0% | 5949.50 |

Decisão: **promover para candidato público**. Diferente de algumas flags de compilação, esta remove trabalho real do hot path de parse, preserva correção e sustentou três runs locais acima da validação final da branch `submission`. Próximo passo: publicar imagem pública e validar com GHCR antes de mexer novamente na branch `submission`.

### Validação pública e decisão final do `known_merchants`

Commit candidato:

- `0ea2767` (`optimize known merchants parsing`).

Workflow de publicação da imagem pública mutável:

- Run: `25477227618`.
- Resultado: sucesso.
- Duração: `1m31s`.
- Imagem publicada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission`.

Resultados públicos:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| imagem pública `known_merchants` #1 | 1.15ms | 0 | 0 | 0 | 5940.03 |
| imagem pública `known_merchants` #2 | 1.14ms | 0 | 0 | 0 | 5944.55 |

Decisão: **rejeitado e revertido no branch experimental**. Apesar do sinal local forte, a validação pública não superou `submission-cd3e915` de forma convincente. A hipótese segue tecnicamente interessante, mas, pelo critério de ganho sustentável, não deve substituir a submissão atual. A branch `submission` permanece em `submission-cd3e915`.

## Ciclo 04h15: pré-cálculo de `mcc_risk` no parse

Hipótese: calcular `merchant_mcc_risk` uma vez durante o parse e carregar esse float no `Payload` evitaria a cadeia de comparações de string em `vectorize`.

Escopo testado:

- Adicionar `merchant_mcc_risk` ao `Payload`.
- Calcular o risco no parse de `merchant.mcc`.
- Trocar `mcc_risk(payload.merchant_mcc)` por `payload.merchant_mcc_risk` no `vectorize`.
- Sem alteração de contrato, algoritmo, compose, índice IVF, resposta ou dados.

Validação funcional:

```text
100% tests passed, 0 tests failed out of 1
```

Resultados locais:

| Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| mcc risk no parse #1 | 1.13ms | 0 | 0 | 0 | 5945.39 |
| mcc risk no parse #2 | 1.13ms | 0 | 0 | 0 | 5946.26 |

Decisão: **rejeitado e revertido sem publicação pública**. A mudança é correta, mas o ganho local ficou marginal e abaixo do nível mínimo para justificar nova imagem/submissão. A melhor submissão preparada continua `submission-cd3e915`.

## Ciclo 04h30: busca web por código dos líderes

Hipótese: os três primeiros colocados do ranking parcial poderiam ter repositórios públicos já indexados com insights diretos de estrutura, índice ou runtime.

Busca realizada:

- `thiagorigonatti-c rinha de backend 2026 github`.
- `jairoblatt-rust rinha de backend 2026 github`.
- `joojf rinha de backend 2026 github`.

Resultado: **sem achado acionável**. Os resultados retornaram ruído de busca, páginas genéricas e repositórios não relacionados aos três líderes consultados. A página pública do ranking confirma a existência do preview da edição 2026, mas não entrega código-fonte dos participantes. Decisão: não gastar nova rodada nessa trilha sem URL direta de repositório; o melhor uso da janela restante é preservar a submissão `submission-cd3e915` e só testar hipóteses locais com retorno técnico claro.

## Fechamento do ciclo estendido até 03h30

Estado conferido às `2026-05-07 03:32:41 -03`:

- Branch experimental: `perf/noon-tuning`, limpa e alinhada com `origin/perf/noon-tuning`.
- Últimos reports pushados: `a4b1778` (`report leader code search`), `4674068` (`report rejected mcc risk experiment`), `013fc2e` (`reject known merchants parser change`), `d9cdc30` (`report raw state submission`).
- Branch `submission`: limpa e alinhada com `origin/submission`.
- Commit da submissão preparada: `46e9aeb` (`point submission to raw state image`).
- Imagem da submissão preparada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-cd3e915`.
- Melhor evidência pública/local do ciclo: raw `AppState*` com melhor pública `1.12ms / 5950.45`, validação final da branch `submission` em `5938.77`, `5944.96`, `5944.68`, sempre `0%` falhas.
- Issue oficial: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2009`, estado `OPEN`, sem comentários até o fechamento.
- Containers de experimento: derrubados após auditoria para liberar a porta `9999`.

Conclusão operacional: o melhor estado preparado continua sendo `submission-cd3e915`. Nenhum experimento posterior entregou ganho público/local sustentável o bastante para substituir essa submissão.
