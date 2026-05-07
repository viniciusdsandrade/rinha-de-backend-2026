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
