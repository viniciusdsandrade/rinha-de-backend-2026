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
