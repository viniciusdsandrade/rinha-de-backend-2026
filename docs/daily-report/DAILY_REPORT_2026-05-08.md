# Daily Report - 2026-05-08

## Contexto inicial

Objetivo do ciclo: continuar até 18h00 com o fluxo **investigação -> hipótese -> experimento -> report**, buscando melhoria de performance sustentável para a submissão C++ atual.

Branch de trabalho:

```text
perf/noon-tuning
```

Estado inicial verificado:

```text
2026-05-08 16:00:33 -03
worktree limpo
HEAD 70e715c report compose compliance audit
```

Melhor estado aceito herdado de 2026-05-07:

```text
Servidor HTTP manual C++/epoll + buffer fixo
nginx stream 1.27 via Unix sockets
IVF 1280 clusters, train_sample 65536, 6 iterações
IVF repair 1..4
Melhor run aceita: 5921.67, 0% falhas
Baseline aceito reproduzido: 5920.04, 0% falhas
```

Critério de aceitação desta rodada:

- Aceitar apenas mudanças que preservem 0% de falhas ou que tragam ganho técnico claramente justificável sem piorar o contrato.
- Rejeitar picos isolados sem reprodução.
- Registrar tanto achados positivos quanto negativos.
- Evitar repetir classes já rejeitadas no dia anterior: nginx workers, `multi_accept`, HAProxy, nginx HTTP proxy, headers, `Content-Length` fast path, `memmove(0)`, `kReadChunk`, redistribuições CPU já testadas, clusters/treino IVF que geraram FP/FN.

## Ciclo 16h08: checagem do runner oficial e preparação de imagem

Investigação: a issue antiga do runner (`#2026`) está fechada, mas apontava reincidência em issues novas. Foram checadas as issues `#2123` a `#2126`.

Resultado:

```text
#2123, #2125 e #2126 já receberam JSON de resultado depois da falha inicial de clone.
#2124 ainda ficou apenas com erro de clone, mas a engine voltou a executar ao menos parte da fila.
Não há issue aberta para viniciusdsandrade.
```

Hipótese: como o runner voltou a produzir resultados, vale preparar uma submissão oficial nova com o melhor estado aceito (`manual HTTP + buffer fixo`), mas somente depois de publicar uma imagem pública GHCR com a branch atual.

Ação:

```text
.github/workflows/publish-submission-image.yml
adicionar tag ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a477d55
```

Status: em preparação. Próximo passo: commit/push da tag do workflow e acompanhar publicação da imagem.

## Ciclo 16h15: publicar imagem e atualizar branch `submission`

Ação executada:

```text
commit 0bff099 add current submission image tag
GitHub Actions run 25574111307
workflow Publish submission image
resultado: success
tag publicada: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a477d55
```

Branch `submission` atualizada no worktree `/home/andrade/Desktop/rinha-de-backend-2026-rust`:

```text
docker-compose.yml:
  image: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a477d55

info.json:
  stack: ["c++20", "epoll", "unix-socket", "simdjson", "nginx", "avx2", "fma", "ivf", "int16"]
```

Verificação:

```text
docker compose -p submission-check config --quiet
COMPOSE_CONFIG_OK
```

Commit/push:

```text
2b25c5f point submission to manual epoll image
origin/submission atualizado
```

Issue oficial aberta:

```text
https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2316
title/body: rinha/test andrade-cpp-ivf
```

Status: aguardando resultado da engine oficial.

## Ciclo 16h23: validação local da imagem pública submetida

Hipótese: antes de confiar na issue oficial, validar localmente a imagem pública `submission-a477d55` exatamente como a branch `submission` aponta. Isso cobre falhas de publicação/tag/arquitetura que a imagem `:local` não detecta.

Primeira tentativa:

```text
run-local-k6.sh executado a partir da branch submission mínima
falhou porque test/test.js não existe nessa branch
aprendizado: compose deve vir da branch submission, mas o k6 local deve rodar do worktree completo
```

Validação correta:

```text
docker compose -p submission-public pull
docker compose -p submission-public up -d --no-build
curl /ready
curl POST /fraud-score com .entries[0].request
run-local-k6.sh a partir de perf/noon-tuning
```

Smoke:

```text
READY
HTTP/1.1 200 OK
Content-Length: 36

{"approved":false,"fraud_score":1.0}
```

Resultado k6 com imagem pública:

| Imagem | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a477d55` | 1.21ms | 0% | 5917.98 |

Decisão: **válido para submissão**. A imagem pública está acessível, é executável no compose da branch `submission`, mantém 0% falhas e reproduz o patamar esperado do melhor estado aceito.

## Ciclo 16h32: CPU intermediária `api=0.405`, `nginx=0.19`

Hipótese: os extremos já testados (`api0.40/nginx0.20` e `api0.42/nginx0.16`) foram ruins, mas um ponto intermediário poderia reduzir leve contenção no nginx sem tirar CPU demais das APIs.

Patch temporário:

```yaml
api1/api2: 0.41 CPU -> 0.405 CPU
nginx:     0.18 CPU -> 0.19 CPU
total:     1.00 CPU
```

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `api0.405/nginx0.19` | 1.24ms | 0% | 5907.43 |

Decisão: **rejeitado e revertido**. O ponto intermediário piorou mais que o estado aceito e confirmou que a distribuição `api=0.41 + 0.41`, `nginx=0.18` segue sendo o melhor balanço de recursos medido.

## Ciclo 16h41: diagnóstico da fila da issue oficial

Investigação: a issue `#2316` permaneceu aberta sem comentários enquanto algumas issues próximas foram processadas.

Evidência:

```text
#2319 CLOSED, #2309 CLOSED
#2316 OPEN sem comentários
várias issues recentes também OPEN (#2313, #2314, #2315, #2317, #2318, #2320)
```

Checagem do participante oficial:

```text
gh api repos/zanfranceschi/rinha-de-backend-2026/contents/participants/viniciusdsandrade.json?ref=main
[
  {
    "id": "andrade-cpp-ivf",
    "repo": "https://github.com/viniciusdsandrade/rinha-de-backend-2026"
  }
]
```

Observação: os arquivos locais `participants/viniciusdsandrade.json` ainda aparecem como `andrade-rust`, mas o arquivo oficial no upstream está correto. A engine usa o cadastro oficial, então a issue `rinha/test andrade-cpp-ivf` está semanticamente correta.

Decisão: **não abrir issue duplicada**. Manter `#2316` aberta e aguardar a fila/runner.

## Ciclo 16h55: screening PGO offline

Hipótese: `-fprofile-generate/-fprofile-use` poderia reorganizar branches e layout do binário para reduzir alguns microssegundos no hot path do parser/classificador.

Procedimento:

```text
build instrumentado com -fprofile-generate=/tmp/rinha-pgo-profile
treino offline com prepare-ivf-cpp + benchmark-ivf-cpp + benchmark-request-cpp
build posterior com -fprofile-use=/tmp/rinha-pgo-profile -fprofile-correction
comparação offline contra build base atual
```

Resultado do kernel IVF:

| Variante | ns/query | FP | FN | parse_errors |
|---|---:|---:|---:|---:|
| PGO | 10123.7 | 0 | 0 | 0 |
| Base atual | 10007.1 | 0 | 0 | 0 |

Resultado: **rejeitado**.

Aprendizados:

- O PGO piorou o kernel IVF em ~1.17% no benchmark offline, sem alteração de acurácia.
- A etapa `benchmark-request-cpp` exata com `repeat=5` ficou cara demais para screening leve e foi abortada após o IVF já sinalizar regressão.
- Mesmo que houvesse ganho pontual, treinar PGO com `test/test-data.json` é tecnicamente questionável para submissão sustentável, por risco de overfitting ao dataset público local.
- Decisão operacional: não integrar PGO no Dockerfile nem gerar nova imagem de submissão a partir desse caminho.

## Ciclo 17h18: `-march=haswell -mtune=haswell`

Hipótese: o host oficial descrito para a Rinha é `amd64` antigo com AVX2; trocar o alvo genérico `x86-64-v3` por `haswell` poderia melhorar branch/layout de instruções sem mexer no algoritmo.

Patch temporário:

```text
RINHA_X86_FLAGS = -mavx2 -mfma -march=haswell -mtune=haswell
```

Validação offline:

| Variante | ns/query | FP | FN | parse_errors |
|---|---:|---:|---:|---:|
| `haswell` temporário | 9598.45 | 0 | 0 | 0 |
| base antes do patch | 10063.5 | 0 | 0 | 0 |

Após aplicar no CMake local e reconstruir:

```text
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1

benchmark-ivf-cpp
ns_per_query=7684.64 fp=0 fn=0 parse_errors=0
```

Validação k6 em compose:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `haswell` temporário | 1.31ms | 0% | 5884.28 |

Resultado: **rejeitado e revertido**.

Aprendizado: o ganho offline não sobreviveu ao stack completo. O gargalo/p99 observado pelo k6 segue dominado por interação compose/nginx/scheduler/HTTP, não apenas pelo kernel IVF isolado. Como a submissão pública anterior mediu `5917.98` e o melhor local aceito do ciclo anterior foi `5921.67`, `haswell` não é promoção segura.

## Ciclo 17h45: base `debian:trixie` / GCC 14

Hipótese: a imagem atual compila no `debian:bookworm` com GCC 12, enquanto os testes locais fora do Docker usam toolchain mais novo. Migrar builder/runtime para `debian:trixie` poderia melhorar codegen e bibliotecas de runtime sem mexer no algoritmo, no compose, na topologia ou na acurácia.

Patch:

```diff
- FROM debian:bookworm AS builder
+ FROM debian:trixie AS builder

- FROM debian:bookworm-slim AS runtime
+ FROM debian:trixie-slim AS runtime
```

Validação k6 em compose:

| Run | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `trixie` run 1 | 1.15ms | 0% | 5938.30 |
| `trixie` run 2 | 1.13ms | 0% | 5947.63 |

Comparação contra referências recentes:

| Estado | p99 | Falhas | final_score |
|---|---:|---:|---:|
| imagem pública submetida `submission-a477d55` | 1.21ms | 0% | 5917.98 |
| melhor local aceito anterior | ~1.20ms | 0% | 5921.67 |
| `trixie` melhor run local | 1.13ms | 0% | 5947.63 |

Resultado: **aceito para promoção**.

Aprendizados:

- A troca é sustentável: muda apenas toolchain/base image, sem alterar decisão, índice, protocolo, recursos ou parâmetros de busca.
- O ganho reproduziu em duas runs consecutivas e manteve `0%` falhas.
- A melhora aproximada contra a imagem pública submetida é `+29.65` pontos na melhor run local (`5947.63 - 5917.98`).
- Próximo passo: commitar/pushar, publicar nova imagem GHCR e atualizar branch `submission` para apontar para o novo tag antes de abrir nova issue oficial se o runner da issue anterior continuar sem resposta.

Promoção:

```text
commit perf/noon-tuning: 60daa3d use trixie toolchain for submission image
commit perf/noon-tuning: a0a400d add trixie submission image tag
GitHub Actions: Publish submission image #25575633025 success
imagem: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-60daa3d
commit submission: 8d8a2f6 point submission to trixie image
```

Validação da imagem pública a partir da branch `submission`:

| Imagem pública | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `submission-60daa3d` | 1.13ms | 0% | 5947.40 |

Decisão: **submissão preparada**. O novo tag público reproduziu praticamente a melhor run local e superou a submissão pública anterior (`5917.98`) por `+29.42` pontos no mesmo ambiente local.

## Ciclo 18h05: resultado oficial da issue antiga e nova submissão

A issue oficial `#2316` foi processada, mas usou o estado anterior da branch `submission`:

```text
issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2316
commit testado: 2b25c5f
imagem testada: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a477d55
p99: 1.20ms
falhas: 0%
final_score: 5921.80
```

Como a branch `submission` já havia sido atualizada para `8d8a2f6` apontando para `submission-60daa3d`, foi aberta uma nova issue para testar a imagem melhor:

```text
issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2328
body/title: rinha/test andrade-cpp-ivf
estado esperado da branch submission: 8d8a2f6
imagem esperada: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-60daa3d
melhor validação local pública: p99 1.13ms, 0% falhas, final_score 5947.40
```

## Ciclo 18h18: `trixie` + `haswell`

Hipótese: após a vitória do GCC 14/base `trixie`, retestar `-march=haswell -mtune=haswell` poderia combinar o melhor dos dois caminhos.

Resultado k6:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `trixie + haswell` | 1.16ms | 0% | 5935.29 |
| `trixie + x86-64-v3` melhor pública local | 1.13ms | 0% | 5947.40 |

Resultado: **rejeitado e revertido**.

Aprendizado: com GCC 14, manter `x86-64-v3` genérico foi melhor no stack completo. O `haswell` continua competitivo offline/isolado, mas perde no k6 end-to-end.
