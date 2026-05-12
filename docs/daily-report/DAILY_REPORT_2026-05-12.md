# Daily Report - 2026-05-12

## Ciclo 00h03: atualização operacional do objetivo

Objetivo operacional atualizado: continuar o fluxo de investigação, hipótese, experimento, resultado e report até `2026-05-12 03:00 -03`.

Observação de ferramenta: o goal ativo existe, mas a ferramenta disponível só permite marcar como completo; não há operação para alterar prazo. Portanto, o novo cutoff foi registrado aqui como regra operacional do ciclo.

Estado inicial:

- Worktree: `/home/andrade/.config/superpowers/worktrees/rinha-de-backend-2026/perf-noon-tuning`.
- Branch: `perf/noon-tuning`.
- HEAD inicial: `2dbde3b report timestamp fast path rejection`.
- Estado git inicial: limpo e sincronizado com `origin/perf/noon-tuning`.

## Ciclo 00h05: reauditoria dos líderes e screening do LB `so-no-forevis:v1.0.0`

Investigação:

- Issue oficial `#911` do `thiagorigonatti`: `p99=1.00ms`, `failure_rate=0%`, `final_score=6000`, imagem `thiagorigonatti/rinha-2026:1.0.20` + LB `thiagorigonatti/tornado:0.0.2`, commit `b4449d5`.
- O repositório `thiagorigonatti/rinha-2026` continua inacessível via GitHub nesta máquina (`Could not resolve to a Repository`), e `thiagorigonatti/tornado:0.0.2` continua privado ou sem acesso público para manifest inspect.
- Issue oficial recente `#3443` do `jairoblatt-rust`: `p99=1.05ms`, `failure_rate=0%`, `final_score=5978.43`, imagem `jrblatt/rinha-2026-rust:v1.0.1` + LB `jrblatt/so-no-forevis:v1.0.0`, commit `959119f`.
- O repositório `jairoblatt/rinha-2026-rust` está público e foi clonado em `/tmp/rinha-2026-rust-jairo` para inspeção.

Achados técnicos no `jairoblatt/rinha-2026-rust`:

- O compose oficial recente usa `0.40 CPU / 160MB` por API e `0.20 CPU / 30MB` para o LB.
- O LB `jrblatt/so-no-forevis:v1.0.0` é público e possui manifest `linux/amd64`.
- A API Rust usa `monoio/io_uring`, `mimalloc`, respostas HTTP estáticas, parser JSON manual, IVF quantizado em blocos de 8 lanes, prefetch explícito e retry seletivo `FAST_NPROBE=5`, `FULL_NPROBE=24`.
- O servidor Rust possui módulo `fd.rs`; isso indica integração por passagem de file descriptor com o LB, não apenas upstream UDS tradicional.

Hipótese:

Talvez o ganho oficial recente do Jairo viesse de melhorias no LB `so-no-forevis:v1.0.0`, e não apenas da API Rust. Como nosso teste antigo usou `v0.0.2`, valeu um screening drop-in com a versão nova.

Experimento:

- Troca temporária de `nginx:1.27-alpine` para `jrblatt/so-no-forevis:v1.0.0`.
- APIs mantidas em C++ sem alteração.
- Recursos ajustados temporariamente para o pacote do Jairo: APIs `0.40 CPU / 160MB`, LB `0.20 CPU / 30MB`.
- `docker compose up --build -d` subiu os três containers.
- `GET /ready` via `localhost:9999` não retornou resposta HTTP; curl terminou com `http_code=000`.

Decisão:

Rejeitado e revertido antes de rodar k6. O LB `so-no-forevis:v1.0.0` não é um substituto drop-in para nossa API C++ atual.

Aprendizado:

O diferencial do `so-no-forevis` está acoplado ao protocolo interno da API do Jairo, com FD-passing. Para aproveitar essa linha seria necessário implementar compatibilidade de recebimento de file descriptors no nosso servidor, o que equivale a uma mudança estrutural de servidor/LB. Não faz sentido tratar como micro-otimização isolada nem substituir o nginx diretamente.
