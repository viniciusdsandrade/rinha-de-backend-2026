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
