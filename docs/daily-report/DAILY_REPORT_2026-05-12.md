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

## Ciclo 00h15: política de probes e desempate do top-5

Investigação:

- O `jairoblatt/rinha-2026-rust` recente usa `FAST_NPROBE=5`, `FULL_NPROBE=24`, retry seletivo apenas quando o fast retorna `2/5` ou `3/5`, e não possui reparo por bounding box equivalente ao nosso.
- Nossa submissão atual usa `FAST_NPROBE=1`, `FULL_NPROBE=1`, `BBOX_REPAIR=1`, `repair_min=1`, `repair_max=4`, além de reparos extremos pontuais para preservar `0%` falhas.

Screening offline de configuração no índice atual `index-1280.bin`:

| Configuração | ns/query | FP | FN | failure_rate |
|---|---:|---:|---:|---:|
| Atual `1/1 + bbox + repair 1..4` | 12442.5 | 0 | 0 | 0% |
| Jairo-like `5/24 sem bbox repair 2..3` | 38201.1 | 1 | 2 | 0.0055% |
| Jairo-like `5/24 + bbox repair 2..3` | 38995.8 | 0 | 0 | 0% |
| `1/1 + bbox + repair 2..3` | 10161.9 | 22 | 28 | 0.0924% |
| `1/1 + bbox + repair 1..3` | 10206.7 | 22 | 0 | 0.0407% |
| `1/1 + bbox + repair 2..4` | 10158.6 | 0 | 28 | 0.0518% |
| `1/1 + bbox + repair 1..4` sem reparo extremo | 7599.21 | 1 | 2 | 0.0055% |

Decisão de configuração:

Rejeitado. Copiar `FAST=5/FULL=24` do Jairo é muito mais caro no nosso índice. Reduzir janela de reparo melhora tempo offline, mas introduz FP/FN; pela fórmula oficial, erros derrubam score de detecção e não compensam ganhos pequenos de p99. Manter `1/1 + bbox + repair 1..4 + reparo extremo`.

Hipótese adicional:

O desempate por `orig_id` no `Top5` custa comparações no hot path. Como o dataset rotulado usa kNN exato e distâncias quantizadas podem empatar raramente, talvez remover o desempate preservasse acurácia e reduzisse cauda.

Experimento:

- Patch temporário: `Top5::better()` passou a comparar apenas `distance < worst`, e `refresh_worst()` deixou de desempatar por `orig_id`.
- Validação: `ctest --test-dir cpp/build --output-on-failure` passou.
- Benchmark offline: `fp=0`, `fn=0`, `failure_rate=0%`, `ns_per_query=9325.23`.
- k6 local run 1: `p99=1.10ms`, `final_score=5959.83`, `0%` falhas.
- k6 local run 2: `p99=1.13ms`, `final_score=5947.56`, `0%` falhas.

Decisão:

Rejeitado e revertido. Embora correto nos testes e aparentemente melhor no microbenchmark, o k6 não reproduziu ganho sobre as melhores runs locais recentes do estado já submetido (`1.03ms` e `1.07ms`). Sem margem reproduzível, não justifica nova submissão nem risco de mudar ordenação de empate.

Aprendizado:

O gargalo atual não parece estar no desempate do top-5 nem na janela de probes. A diferença para `jairoblatt` continua apontando para arquitetura de servidor/LB (`FD-passing + io_uring`) e/ou para layout/kernel de índice mais estrutural, não para microajustes de seleção.

## Ciclo 00h45: FD-passing compatível com `so-no-forevis:v1.0.0`

Hipótese:

O ganho recente do `jairoblatt-rust` (`p99=1.05ms`, `final_score=5978.43`) não vem de trocar nginx por um LB qualquer, mas do protocolo específico do `jrblatt/so-no-forevis:v1.0.0`: o LB aceita TCP na porta `9999`, usa `io_uring` e entrega file descriptors já aceitos para as APIs por sockets Unix de controle (`<api.sock>.ctrl`) via `SCM_RIGHTS`. Se a nossa API C++ suportar esse protocolo, podemos reduzir overhead de proxy sem trocar o kernel KNN nem o parser de fraude.

Implementação experimental:

- Mantido o listener Unix normal `/sockets/apiN.sock` para compatibilidade.
- Adicionado listener de controle `/sockets/apiN.sock.ctrl`.
- A thread de controle recebe FDs por `recvmsg(... SCM_RIGHTS ...)`.
- Os FDs recebidos são colocados em modo não-bloqueante e repassados ao loop epoll principal por pipe interno.
- O loop epoll passa a aceitar conexões tanto pelo UDS tradicional quanto pelo pipe de FDs transferidos.
- Compose temporário trocado para `jrblatt/so-no-forevis:v1.0.0`, com `UPSTREAMS=/sockets/api1.sock,/sockets/api2.sock`, `WORKERS=1`, `BUF_SIZE=4096`, `0.20 CPU / 30MB`.
- `security_opt: seccomp:unconfined` foi necessário no LB; sem isso o binário panica ao criar runtime `io_uring` com `Operation not permitted`, exatamente o tipo de requisito observado na solução do Jairo.

Correções durante a investigação:

- Primeira tentativa com `.ctrl` falhou porque o listener de controle herdou `SOCK_NONBLOCK`; a thread saía em `EAGAIN` antes do LB conectar.
- Corrigido para tratar `EAGAIN/EWOULDBLOCK` com espera curta e continuar aceitando.

Validação:

- `cmake --build cpp/build --target rinha-backend-2026-cpp-manual rinha-backend-2026-cpp-tests -j$(nproc)` passou.
- `ctest --test-dir cpp/build --output-on-failure` passou.
- Smoke com `jrblatt/so-no-forevis:v1.0.0`: `GET /ready` respondeu `204`.

Resultados k6 locais:

| Variante | p99 | failure_rate | final_score |
|---|---:|---:|---:|
| FD-passing + `so-no-forevis:v1.0.0` run 1 | 0.98ms | 0% | 6000 |
| FD-passing + `so-no-forevis:v1.0.0` run 2 | 0.96ms | 0% | 6000 |

Decisão:

Aceito como melhor candidato técnico do ciclo até agora. Esta é a primeira rodada desde a submissão anterior que melhora de forma material e reproduzível: o p99 local caiu abaixo de `1ms`, saturando `p99_score=3000` sem introduzir FP, FN ou erro HTTP.

Aprendizado:

A leitura dos líderes estava correta: o gap remanescente não estava no KNN, no parser JSON ou em microflags, mas no caminho de proxy/servidor. O `so-no-forevis` não era drop-in porque exigia o protocolo de FD-passing; ao implementar esse contrato na API C++, o ganho apareceu imediatamente e com margem suficiente para justificar preparação de nova submissão oficial.

## Ciclo 00h35-00h45: promoção da candidata FD-passing para `submission`

Objetivo:

- Transformar o melhor resultado parcial do ciclo (`FD-passing + so-no-forevis`) em uma submissão oficial reproduzível.
- Evitar o erro operacional anterior: a issue oficial precisa ter `rinha/test andrade-cpp-ivf` na descrição da issue, não apenas em comentário posterior.

Execução:

- A branch `submission` recebeu a implementação C++ FD-passing e o compose com `jrblatt/so-no-forevis:v1.0.0`.
- O `Dockerfile` da `submission` foi ajustado para baixar `resources/references.json.gz` do repositório oficial durante o build, porque a branch enxuta de submissão não carrega `resources/`.
- A publicação local via `docker buildx --push` falhou por falta de escopo efetivo `write:packages` na credencial Docker/GHCR.
- A publicação foi refeita com o workflow `Publish GHCR image`, que usa `GITHUB_TOKEN` com `packages: write`.
- Imagem publicada: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-076c74a`.
- `docker-compose.yml` da branch `submission` passou a apontar para essa imagem pública e para o LB público `jrblatt/so-no-forevis:v1.0.0`, ambos com `platform: linux/amd64`.
- Commit da branch `submission`: `9bca93f point submission to fd passing image`.

Validação:

- `docker manifest inspect ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-076c74a`: manifest `linux/amd64` confirmado.
- `docker compose pull` na branch `submission`: imagem da API e LB baixadas com sucesso.
- `docker compose up -d` na branch `submission`: APIs e LB subiram.
- `GET /ready`: `204` após 2s.

Benchmark oficial-like:

| Fonte do teste | Dataset | p99 | failure_rate | FP | FN | final_score | Decisão |
|---|---:|---:|---:|---:|---:|---:|---|
| Checkout antigo `/home/andrade/Desktop/rinha-de-backend-2026` em `submission-2` | 14.500 reqs | 1.22ms | 1.67% | 123 | 117 | 3590.41 | Descartado: dataset/referências antigos e divergentes |
| Worktree atual `perf/noon-tuning` | 54.100 reqs | 1.03ms | 0% | 0 | 0 | 5986.72 | Aceito |
| Worktree atual `perf/noon-tuning` | 54.100 reqs | 1.04ms | 0% | 0 | 0 | 5982.64 | Aceito |

Aprendizado:

- O resultado ruim inicial não veio da imagem publicada, mas de executar `./run.sh` em um checkout antigo com `test/test-data.json` e `resources/references.json.gz` divergentes.
- Para esta rodada, o benchmark de referência local deve ser o worktree atualizado `perf/noon-tuning`, cujo dataset tem 54.100 requisições e bate com a linha de investigação recente.
- A candidata FD-passing continua forte: duas runs atualizadas mantiveram `0%` de falhas e score local próximo do teto.

Submissão oficial:

- Issue aberta no repositório oficial: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/3535
- Título: `rinha/test andrade-cpp-ivf`
- Descrição: `rinha/test andrade-cpp-ivf`
- Resultado oficial: `p99=1.06ms`, `failure_rate=0%`, `FP=0`, `FN=0`, `HTTP errors=0`, `final_score=5976.27`.
- Runtime oficial: `cpu=1`, `mem=350`, duas APIs `0.40 CPU / 160MB`, LB `0.20 CPU / 30MB`, `instances-number-ok?=true`, `unlimited-services=null`, `Privileged=false`.
- Commit avaliado pela engine: `9bca93f`.

Decisão:

- Submissão válida e superior à submissão anterior `andrade-cpp-ivf` registrada no ranking parcial (`p99=2.83ms`, `final_score=5548.91`).
- O ganho oficial em score foi de `+427.36` pontos e a latência oficial caiu de `2.83ms` para `1.06ms`.

## Ciclo 00h55: redistribuição de CPU entre API e LB

Contexto:

- O ranking preview atualizado após a issue `#3535` colocou nossa submissão em 2º lugar: `p99=1.06ms`, `final_score=5976.27`.
- O 1º lugar no momento é `jairoblatt-rust`, com `p99=1.05ms`, `final_score=5978.43`.
- A diferença é de apenas `2.16` pontos; a única margem útil está em reduzir a cauda de `1.06ms` para `<=1.05ms` ou, idealmente, abaixo de `1ms`.

Hipótese:

Com FD-passing ativo, o LB deixa de fazer proxy HTTP tradicional e passa a ser menos custoso. A cauda residual pode estar mais no processamento das APIs do que no LB. Portanto, mover uma pequena fatia de CPU do LB para as APIs pode melhorar p99 sem quebrar o limite total de `1 CPU`.

Experimento:

- Antes: APIs `0.40 + 0.40`, LB `0.20`, total `1.00 CPU`.
- Depois: APIs `0.42 + 0.42`, LB `0.16`, total `1.00 CPU`.
- Memória mantida: APIs `160MB + 160MB`, LB `30MB`, total `350MB`.
- Nenhuma alteração de código nem imagem.

Resultados locais no worktree atualizado:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.42/0.42/0.16` run 1 | 0.83ms | 0% | 0 | 0 | 6000 |
| `0.42/0.42/0.16` run 2 | 0.89ms | 0% | 0 | 0 | 6000 |

Decisão:

- Aceito e promovido para a branch `submission`.
- Commit da branch `submission`: `4e317cf tune fd passing cpu split`.
- Nova issue oficial aberta: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/3537
- Título e descrição: `rinha/test andrade-cpp-ivf`.
- Resultado oficial: `p99=1.04ms`, `failure_rate=0%`, `FP=0`, `FN=0`, `HTTP errors=0`, `final_score=5983.81`.
- Runtime oficial: APIs `0.42 CPU / 160MB` cada, LB `0.16 CPU / 30MB`, total `1 CPU / 350MB`, `instances-number-ok?=true`, `unlimited-services=null`, `Privileged=false`.
- Commit avaliado pela engine: `4e317cf`.

Aprendizado:

- O split de CPU é uma alavanca real nessa arquitetura. O LB com FD-passing aparentemente tolera menos CPU do que os `0.20` copiados do Jairo, enquanto as APIs se beneficiam diretamente da folga adicional.
- Esta é uma melhoria sustentável: mantém topologia, imagem pública, bridge, duas APIs, LB sem lógica de negócio, `Privileged=false`, `1 CPU` e `350MB`.
- Em relação à submissão oficial imediatamente anterior (`#3535`, `p99=1.06ms`, `final_score=5976.27`), o ganho foi de `+7.54` pontos e `-0.02ms` no p99.
- Em relação ao então líder `jairoblatt-rust` (`p99=1.05ms`, `final_score=5978.43`), o resultado oficial `#3537` ficou `+5.38` pontos acima.

## Ciclo 01h05: limite inferior de CPU do LB

Hipótese:

Se `0.42/0.42/0.16` foi melhor que `0.40/0.40/0.20`, talvez deslocar mais CPU do LB para as APIs (`0.43/0.43/0.14`) reduza ainda mais o p99.

Experimento:

- APIs `0.43 + 0.43`, LB `0.14`, total `1.00 CPU`.
- Mesma imagem, mesmo índice, mesmo LB, sem alteração de código.
- `GET /ready`: `204`.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.43/0.43/0.14` | 1.07ms | 0% | 0 | 0 | 5969.32 |

Decisão:

- Rejeitado.
- O compose experimental foi revertido para `0.42/0.42/0.16`.

Aprendizado:

- O ponto útil parece estar perto de `0.16 CPU` para o LB. Abaixo disso, a cauda volta a subir mesmo com mais CPU nas APIs.
- Próximos splits candidatos, se necessário, devem testar a vizinhança fina (`0.415/0.415/0.17` ou `0.425/0.425/0.15`) em vez de deslocamentos maiores.

## Ciclo 01h10-01h15: ajuste fino `0.425/0.425/0.15`

Hipótese:

O split `0.425/0.425/0.15` fica no meio entre o aceito `0.42/0.42/0.16` e o rejeitado `0.43/0.43/0.14`. Talvez ele preserve CPU suficiente no LB e ainda entregue um pouco mais de folga às APIs.

Resultados locais:

| Variante | Run | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|---:|
| `0.425/0.425/0.15` | 1 | 1.01ms | 0% | 0 | 0 | 5997.00 |
| `0.425/0.425/0.15` | 2 | 1.00ms | 0% | 0 | 0 | 5999.08 |
| `0.425/0.425/0.15` | 3 | 1.07ms | 0% | 0 | 0 | 5971.60 |

Decisão:

- Não promover por enquanto.
- O split é promissor, mas instável demais para justificar nova submissão oficial contra o estado atual `#3537` (`p99=1.04ms`, `final_score=5983.81`).

Aprendizado:

- O teto local aparece ocasionalmente perto de `1ms`, mas a terceira run mostra que a redução adicional do LB para `0.15 CPU` ainda pode aumentar variância de cauda.
- A próxima hipótese mais limpa é testar o lado oposto da vizinhança fina: `0.415/0.415/0.17`, priorizando um pouco mais de estabilidade no LB.

## Ciclo 01h20: ajuste fino `0.415/0.415/0.17`

Hipótese:

O split `0.415/0.415/0.17` poderia preservar mais CPU no LB do que `0.425/0.425/0.15`, reduzindo variância de cauda, ainda com um pouco mais de CPU nas APIs do que o split original `0.40/0.40/0.20`.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.415/0.415/0.17` | 1.07ms | 0% | 0 | 0 | 5971.12 |

Decisão:

- Rejeitado após uma run, porque já ficou abaixo do oficial atual `#3537` e empatou a faixa ruim de `0.43/0.43/0.14`.

Aprendizado:

- A região boa parece estreita e não monotônica. A vizinhança útil continua centrada em `0.42/0.42/0.16`; deslocar CPU em qualquer direção maior que poucos milésimos piora a cauda.

## Ciclo 01h25: ajuste fino `0.422/0.422/0.156`

Hipótese:

Um deslocamento microscópico a partir do melhor oficial (`0.42/0.42/0.16`) talvez preservasse estabilidade do LB e desse mais CPU às APIs.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.422/0.422/0.156` | 1.05ms | 0% | 0 | 0 | 5980.51 |

Decisão:

- Rejeitado.
- Apesar de correto, ficou abaixo da submissão oficial atual (`#3537`, `p99=1.04ms`, `final_score=5983.81`).

Aprendizado:

- O split `0.42/0.42/0.16` continua sendo o melhor compromisso medido.
- A próxima busca deve sair do split de CPU e mirar parâmetros do LB/servidor, mantendo o melhor split como base.

## Ciclo 01h30: `BUF_SIZE=2048` no LB

Hipótese:

Com requisições/respostas pequenas, reduzir `BUF_SIZE` do `so-no-forevis` de `4096` para `2048` poderia reduzir trabalho/cache footprint no LB sem truncar payloads.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.42/0.42/0.16`, `BUF_SIZE=2048` | 1.05ms | 0% | 0 | 0 | 5979.91 |

Decisão:

- Rejeitado.
- Resultado inferior ao oficial atual e à melhor configuração local com `BUF_SIZE=4096`.

Aprendizado:

- Reduzir o buffer do LB não melhora a cauda; possivelmente aumenta fragmentação/custo de leitura em alguns payloads.
- Testar apenas o lado oposto (`8192`) antes de encerrar esta família.

## Ciclo 01h35: `BUF_SIZE=8192` no LB

Hipótese:

Se `2048` piorou por possível fragmentação, aumentar o buffer para `8192` poderia reduzir leituras parciais e suavizar cauda.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.42/0.42/0.16`, `BUF_SIZE=8192` | 1.05ms | 0% | 0 | 0 | 5980.52 |

Decisão:

- Rejeitado.
- Restaurar `BUF_SIZE=4096`.

Aprendizado:

- A configuração do Jairo (`BUF_SIZE=4096`) também é o melhor ponto para nossa API C++ nas medições locais.
- A família `BUF_SIZE` não superou a submissão oficial atual; não merece nova issue.

## Ciclo 01h40: `WORKERS=2` no LB

Hipótese:

Mais um worker no `so-no-forevis` poderia melhorar distribuição/aceitação em paralelo e reduzir cauda.

Resultado:

- Configuração: split `0.42/0.42/0.16`, `BUF_SIZE=4096`, `WORKERS=2`.
- Os containers subiram, mas `GET /ready` via `localhost:9999` ficou pendurado em vez de retornar `204`.
- Não houve logs úteis no compose.
- O teste foi abortado antes de k6 para evitar run inválida.

Decisão:

- Rejeitado.
- Restaurado `WORKERS=1`.

Aprendizado:

- `WORKERS=2` não é uma otimização segura para a nossa integração FD-passing atual. Pode haver acoplamento interno do LB com sockets de controle/entrega de FDs que funciona corretamente com `WORKERS=1`, que é também a configuração do líder Jairo.

## Ciclo 01h45: `ulimits.nofile=65535`

Investigação externa:

- O 3º colocado no ranking preview, `steixeira93-zig-v2`, usa um LB próprio em Zig com `io_uring`, APIs também em Zig e `ulimits.nofile=65535` nos serviços.
- O compose dele mantém o split clássico `0.40/0.40/0.20` e `seccomp:unconfined`.

Hipótese:

Aumentar `nofile` poderia ajudar nossa integração FD-passing sob alta concorrência, reduzindo risco de limite de file descriptors no LB/API.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.42/0.42/0.16` + `ulimits.nofile=65535` | 1.06ms | 0% | 0 | 0 | 5974.79 |

Decisão:

- Rejeitado e removido.

Aprendizado:

- Não há evidência de FD exhaustion na nossa stack atual; aumentar `nofile` não reduziu cauda e piorou a run local.
- O insight útil do Zig não é `ulimit`, mas a confirmação independente de que o topo do ranking converge para LB próprio/io_uring/UDS, exatamente a direção já adotada via `so-no-forevis`.

## Ciclo 02h00: buffer fixo de resposta no servidor manual

Hipótese:

Trocar `std::string out` por um buffer fixo por conexão poderia reduzir overhead de hot path, já que as respostas são estáticas, pequenas e quantizadas em seis variações.

Validação:

- `cmake --build cpp/build --target rinha-backend-2026-cpp-manual rinha-backend-2026-cpp-tests -j$(nproc)` passou.
- `ctest --test-dir cpp/build --output-on-failure` passou.
- `GET /ready`: `204`.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| Buffer fixo de saída | 1.06ms | 0% | 0 | 0 | 5973.27 |

Decisão:

- Rejeitado e revertido.

Aprendizado:

- O custo de `std::string` reservada no buffer de saída não é gargalo material.
- A piora sugere que a versão atual já está bem ajustada pelo compilador/libstdc++ ou que a mudança aumenta footprint por conexão sem reduzir o trecho crítico.

## Ciclo 02h15: `-ffast-math` no runtime manual

Hipótese:

Aplicar `-ffast-math -fno-math-errno` apenas no target `rinha-backend-2026-cpp-manual` poderia reduzir custo do kernel IVF sem alterar a imagem/infra.

Validação:

- `cmake --build cpp/build --target rinha-backend-2026-cpp-manual rinha-backend-2026-cpp-tests -j$(nproc)` passou.
- `ctest --test-dir cpp/build --output-on-failure` passou.
- `GET /ready`: `204`.

Resultados locais:

| Variante | Run | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|---:|
| `-ffast-math` runtime | 1 | 1.04ms | 0% | 0 | 0 | 5984.94 |
| `-ffast-math` runtime | 2 | 1.08ms | 0% | 0 | 0 | 5967.81 |

Decisão:

- Rejeitado e revertido.

Aprendizado:

- A primeira run pareceu levemente positiva, mas não reproduziu.
- Como a segunda run caiu abaixo do oficial atual, a flag não é sustentável o bastante para submissão.

## Ciclo 02h30: remover `FD_CLOEXEC` dos FDs recebidos

Hipótese:

A solução do Jairo recebe FDs via `SCM_RIGHTS`, coloca o stream em não-bloqueante e não chama `FD_CLOEXEC`. Como nosso processo não executa `exec`, o `fcntl(F_SETFD, FD_CLOEXEC)` por conexão poderia ser um syscall desnecessário.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| Sem `FD_CLOEXEC` em FD recebido | 1.07ms | 0% | 0 | 0 | 5968.93 |

Decisão:

- Rejeitado e revertido.

Aprendizado:

- O custo de `FD_CLOEXEC` não é o limitador mensurável da cauda.
- Manter `FD_CLOEXEC` é mais seguro e não prejudica o melhor resultado oficial conhecido.

## Ciclo 02h40: split conservador `0.41/0.41/0.18`

Hipótese:

Se o LB estivesse ocasionalmente no limite com `0.16 CPU`, devolver parte da CPU para ele (`0.18`) poderia reduzir variância mesmo com APIs ligeiramente menores.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.41/0.41/0.18` | 1.05ms | 0% | 0 | 0 | 5980.12 |

Decisão:

- Rejeitado.
- Restaurado `0.42/0.42/0.16`.

Aprendizado:

- O split oficial atual continua sendo o melhor ponto medido.
- O mapeamento local ficou coerente: `0.41/0.18`, `0.415/0.17`, `0.422/0.156`, `0.425/0.15` e `0.43/0.14` não sustentaram melhora sobre `0.42/0.16`.

## Ciclo 02h50: reduzir `kMaxPending` de 16KB para 2KB

Investigação:

- Payload oficial médio: aproximadamente `435` bytes.
- Payload oficial máximo: `469` bytes.
- Nenhum payload oficial passa de `2048` bytes.

Hipótese:

Reduzir o buffer pendente por conexão de `16KB` para `2KB` poderia diminuir footprint/cache por conexão sem afetar os payloads oficiais.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `kMaxPending=2048` | 1.06ms | 0% | 0 | 0 | 5974.89 |

Decisão:

- Rejeitado e revertido para `16KB`.

Aprendizado:

- O buffer maior não é o gargalo material.
- A redução provavelmente aumenta sensibilidade a headers/pacotes fragmentados ou remove alguma folga benéfica do loop de leitura.

## Ciclo 02h55: folga de CPU total `0.995`

Hipótese:

Deixar uma pequena folga de CPU total poderia reduzir throttling/cgroup contention. Configuração testada: APIs `0.42 + 0.42`, LB `0.155`, total `0.995 CPU`.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.42/0.42/0.155` | 1.07ms | 0% | 0 | 0 | 5971.38 |

Decisão:

- Rejeitado.
- Restaurado LB `0.16`.

Aprendizado:

- A pequena folga de CPU não compensou a perda de CPU do LB.
- O limite útil do LB continua em `0.16`, não abaixo disso.

## Ciclo 03h00: warmup sintético do índice antes do `/ready`

Hipótese:

Inspirado pelo `knn::warmup()` do Jairo, executar algumas consultas sintéticas no índice antes de aceitar tráfego poderia reduzir cold-cache nas primeiras centenas de requisições e melhorar o p99.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| Warmup sintético de 256 queries | 1.07ms | 0% | 0 | 0 | 5970.31 |

Decisão:

- Rejeitado e revertido.

Aprendizado:

- Warmup genérico não ajudou; pode inclusive poluir cache com padrões diferentes dos payloads reais.
- Se a linha de warmup voltar, precisa usar payloads reais do `test-data` ou uma seleção representativa empacotada no build, não queries sintéticas arbitrárias.

## Ciclo 02h25: fast path para `Content-Length`

Hipótese:

Adicionar um caminho rápido para `Content-Length: ` exato evitaria a varredura genérica case-insensitive dos headers em toda requisição.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| Fast path `Content-Length: ` | 1.06ms | 0% | 0 | 0 | 5975.06 |

Decisão:

- Rejeitado e revertido.

Aprendizado:

- O parsing genérico de header não é gargalo material no p99 atual.
- A cauda remanescente está mais associada a proxy/scheduler/cgroup ou variação do runner do que a microcustos de header.

## Ciclo 02h35: `ioctl(FIONBIO)` para nonblocking em FD recebido

Hipótese:

Substituir `F_GETFL` + `F_SETFL` por `ioctl(FIONBIO)` reduziria de dois para um syscall ao preparar cada FD recebido por `SCM_RIGHTS`.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `ioctl(FIONBIO)` | 1.06ms | 0% | 0 | 0 | 5976.38 |

Decisão:

- Rejeitado e revertido.

Aprendizado:

- Preparação de FD por conexão não explica o p99 atual.
- A economia teórica de syscall não apareceu no teste k6.

## Ciclo 02h31: build com Clang no runtime C++

Hipótese:

Compilar o binário C++ manual com Clang poderia gerar código mais favorável para o hot path de parse + IVF/KNN do que o GCC atual, sem mudar o comportamento de runtime.

Execução:

- Adicionado `clang` temporariamente no estágio builder do `Dockerfile`.
- Configuração temporária: `CC=clang CXX=clang++ cmake ... -DCMAKE_BUILD_TYPE=Release`.
- Imagem `rinha-backend-2026-cpp-api:local` recompilada com sucesso.
- Stack subiu corretamente; `/ready` respondeu `204` após 2s.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| Build Clang | 1.04ms | 0% | 0 | 0 | 5981.26 |

Decisão:

- Rejeitado.
- Apesar de correto e competitivo, não superou a melhor submissão oficial atual `#3537`, que obteve `p99=1.04ms` e `final_score=5983.81`.
- Patch do `Dockerfile` revertido para manter a branch no baseline aceito.

Aprendizado:

- A escolha GCC vs Clang não moveu a cauda de forma suficiente para justificar rebuild e nova publicação de imagem.
- O gargalo remanescente continua mais provável em variação de proxy/scheduler/cgroup do que em geração de código do compilador.

## Ciclo 02h32: split de CPU estilo Jairo `0.40/0.40/0.20`

Hipótese:

Como o Jairo usa o mesmo LB `jrblatt/so-no-forevis:v1.0.0` com divisão aproximada de APIs `0.40 CPU` e LB `0.20 CPU`, transferir CPU das APIs para o LB poderia reduzir cauda de aceitação/repasse de FDs.

Execução:

- APIs alteradas temporariamente de `0.42 CPU` para `0.40 CPU` cada.
- LB alterado temporariamente de `0.16 CPU` para `0.20 CPU`.
- Total preservado em `1.00 CPU`.
- Stack subiu corretamente; `/ready` respondeu `204` após 1s.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `0.40/0.40/0.20` | 1.07ms | 0% | 0 | 0 | 5972.49 |

Decisão:

- Rejeitado.
- Restaurado split aceito `0.42/0.42/0.16`.

Aprendizado:

- Nosso C++ manual + IVF parece mais sensível à CPU disponível nas APIs do que o stack Rust/monoio do Jairo.
- Aumentar o LB acima de `0.16 CPU` tira capacidade útil do KNN e piora a cauda.

## Ciclo 02h35: controle do baseline aceito `0.42/0.42/0.16`

Objetivo:

Após duas hipóteses rejeitadas, refreezar o baseline aceito para separar regressão real de ruído do runner local.

Execução:

- Restaurado `docker-compose.yml` com APIs `0.42 CPU / 160MB` cada e LB `0.16 CPU / 30MB`.
- Stack recriado do zero.
- `/ready` respondeu `204` após 2s.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| Baseline aceito `0.42/0.42/0.16` | 1.06ms | 0% | 0 | 0 | 5975.07 |

Leitura:

- O baseline local atual ficou abaixo da submissão oficial `#3537` (`p99=1.04ms`, `final_score=5983.81`).
- Não há evidência suficiente para abrir nova issue com o mesmo estado.
- A melhor execução conhecida continua sendo a submissão oficial `#3537`.

## Ciclo 02h40: `TCP_NODELAY` no FD recebido por `SCM_RIGHTS`

Hipótese:

Como a API recebe o socket TCP aceito pelo LB via FD passing, aplicar `TCP_NODELAY` no FD ao adicioná-lo no epoll poderia reduzir latência de envio da resposta.

Execução:

- Primeira tentativa falhou no build por include incompleto (`IPPROTO_TCP` sem `netinet/in.h`).
- Corrigido temporariamente com `netinet/in.h` + `netinet/tcp.h`.
- Aplicado `setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, ...)` em `add_connection`.
- Imagem reconstruída com sucesso.
- Stack subiu corretamente; `/ready` respondeu `204` após 2s.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `TCP_NODELAY` na API | 1.05ms | 0% | 0 | 0 | 5979.13 |

Decisão:

- Rejeitado e revertido.
- O resultado melhorou o baseline local do momento, mas não superou a submissão oficial `#3537` (`5983.81`) nem justificou rebuild/publicação.

Aprendizado:

- O custo de um syscall extra por conexão não se paga de forma clara no cenário atual.
- A cauda não parece ser dominada por Nagle/delay de flush da resposta.

## Ciclo pós-retomada: leitura crítica do `PLANO_PERFORMANCE.md`

Contexto:

Foi lido o plano externo `/home/andrade/Desktop/rinha-de-backend-2026/PLANO_PERFORMANCE.md` elaborado pelo Claude Code. O documento é útil como mapa de hipóteses, mas está parcialmente defasado frente ao estado atual:

- O plano ainda descreve a submissão antiga `#2625` com uWebSockets/nginx e p99 oficial `1.20ms`.
- O estado aceito atual já é C++ manual + IVF + FD passing + `jrblatt/so-no-forevis:v1.0.0`.
- A melhor submissão oficial conhecida é `#3537`, com `p99=1.04ms`, `failure_rate=0%` e `final_score=5983.81`.

Itens do plano aproveitados no fluxo atual:

- Evitar repetir itens do catálogo de não-fazer sem diferença metodológica.
- Tratar microbench/microajuste como suspeito até sobreviver ao k6 repetido.
- Priorizar hipóteses ligadas a zero-alloc/footprint por conexão e syscalls na cauda.
- Comparar qualquer achado com a submissão oficial aceita, não apenas com uma run local isolada.

Itens considerados já superados:

- Migração para IVF como hipótese futura: já está aplicada na solução atual.
- Troca uWS/nginx: a solução atual já saiu desse caminho e usa servidor manual + FD passing.
- CPU split de campeões: testado na prática nesta rodada e rejeitado para o nosso stack.

Decisão:

- O plano seguirá como fonte de hipóteses, não como checklist literal.
- Qualquer nova submissão só será aberta se houver resultado melhor que `#3537` com evidência repetível ou ganho material inequívoco.

## Ciclo pós-retomada: reduzir `conn.out.reserve(512)` para `128`

Hipótese:

As respostas HTTP têm menos de 80 bytes. Reduzir a reserva inicial de `conn.out` de `512` para `128` poderia diminuir footprint/cache por conexão sem alterar o protocolo, alinhado ao princípio de zero-alloc/footprint menor do plano externo.

Execução:

- Alterado temporariamente `conn.out.reserve(512)` para `conn.out.reserve(128)` em `cpp/src/manual_main.cpp`.
- Imagem reconstruída com sucesso.
- Stack subiu corretamente; `/ready` respondeu `204` após 2s.
- Foram feitas 3 runs consecutivas para verificar reprodutibilidade.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.97ms | 0% | 0 | 0 | 6000.00 |
| 2 | 1.07ms | 0% | 0 | 0 | 5972.52 |
| 3 | 1.05ms | 0% | 0 | 0 | 5978.01 |

Decisão:

- Rejeitado e revertido para `512`.
- A primeira run foi excelente, mas não reproduziu.
- A média/estabilidade não supera a submissão oficial `#3537` (`5983.81`).
- Não abrir issue com essa variante; seria aposta em outlier local.

Aprendizado:

- O tamanho inicial do buffer de resposta não é alavanca estável no p99 atual.
- A regra de repetir imediatamente qualquer resultado `6000` evitou uma submissão baseada em ruído.

## Ciclo 10h35: `EPOLLET` nas conexões do servidor manual

Hipótese:

O loop do servidor manual já drena `recv()` até `EAGAIN` e tenta esvaziar `send()` até `EAGAIN`, portanto usar edge-triggered nas conexões (`EPOLLET`) poderia reduzir notificações redundantes do epoll sem mudar algoritmo, parser ou índice.

Execução:

- Aplicado temporariamente `EPOLLET` em `update_events()` e em `add_connection()`.
- Mantidos listen socket e pipe de transferência em level-triggered.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204` após 2s.
- Executadas 3 runs consecutivas para separar ganho real de variância.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.04ms | 0% | 0 | 0 | 5981.32 |
| 2 | 1.03ms | 0% | 0 | 0 | 5985.38 |
| 3 | 1.05ms | 0% | 0 | 0 | 5980.47 |

Decisão:

- Rejeitado e revertido.
- A segunda run superou a submissão oficial `#3537` (`5983.81`), mas o conjunto das três runs não sustentou ganho.
- Não abrir issue: a evidência indica outlier/variância, não melhora material.

Aprendizado:

- `EPOLLET` é funcional no servidor manual atual, mas não melhora a cauda de forma estável.
- O gargalo residual parece estar na faixa de jitter do ambiente/proxy/scheduler; mudanças de epoll isoladas não bastam para empurrar consistentemente abaixo de `1.04ms`.

## Ciclo 10h50: remover `EPOLLRDHUP` das conexões

Hipótese:

Durante keep-alive, `EPOLLRDHUP` não deveria ser necessário para o hot path: fechamento real também é detectado por `recv()==0`. Remover o interesse e a máscara fatal de `EPOLLRDHUP` poderia reduzir notificações extras de half-close.

Execução:

- Removido temporariamente `EPOLLRDHUP` de `update_events()` e `add_connection()`.
- Removido temporariamente `EPOLLRDHUP` da máscara fatal `(EPOLLERR | EPOLLHUP | EPOLLRDHUP)`.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204` após 2s.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.04ms | 0% | 0 | 0 | 5983.40 |
| 2 | 1.06ms | 0% | 0 | 0 | 5976.00 |

Decisão:

- Rejeitado e revertido.
- A primeira run quase empatou com a submissão oficial `#3537`, mas ainda ficou abaixo; a segunda confirmou ausência de ganho sustentável.
- Não abrir issue.

Aprendizado:

- `EPOLLRDHUP` não é custo dominante.
- A remoção não melhora a estabilidade; manter o comportamento explícito de fechamento é mais seguro.

## Ciclo 11h00: `connections.reserve(4096)`

Hipótese:

A `std::unordered_map<int, std::unique_ptr<Connection>>` que mantém conexões ativas não reservava buckets no startup. Reservar `4096` entradas poderia evitar rehash/allocation durante ramp ou churn de conexões.

Execução:

- Adicionado temporariamente `connections.reserve(4096)` logo após a criação do mapa.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204` após 2s.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `connections.reserve(4096)` | 1.04ms | 0% | 0 | 0 | 5982.19 |

Decisão:

- Rejeitado e revertido.
- Não superou a submissão oficial `#3537` (`5983.81`).

Aprendizado:

- Rehash do mapa de conexões não aparece como causa material da cauda atual.
- O gargalo residual não está no crescimento inicial da tabela de conexões.

## Ciclo 11h05: `BUF_SIZE=1024` no `so-no-forevis`

Hipótese:

O maior entry serializado do `test/test-data.json` tem cerca de 533 bytes. Reduzir o buffer do LB de `4096` para `1024` poderia diminuir footprint/cache no `so-no-forevis` sem truncar payloads reais.

Execução:

- Confirmado tamanho máximo de entry local: `533` bytes.
- Alterado temporariamente `BUF_SIZE` de `4096` para `1024`.
- Stack recriado sem rebuild de API.
- `/ready` respondeu `204` após 2s.

Resultado local:

| Variante | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `BUF_SIZE=1024` | 1.04ms | 0% | 0 | 0 | 5982.46 |

Decisão:

- Rejeitado e revertido para `4096`.
- Não superou a submissão oficial `#3537` (`5983.81`).

Aprendizado:

- Reduzir o buffer do LB abaixo de `2048/4096` não gerou ganho.
- O tamanho de buffer do LB já foi suficientemente varrido (`1024`, `2048`, `4096`, `8192`) e `4096` permanece o melhor baseline operacional.

## Ciclo 11h15: `MSG_CMSG_CLOEXEC` no recebimento de FD

Hipótese:

O experimento anterior de remover `FD_CLOEXEC` foi rejeitado por piorar a cauda, mas ainda havia uma variação semanticamente correta: receber o FD passado por `SCM_RIGHTS` já com close-on-exec usando `MSG_CMSG_CLOEXEC` no `recvmsg()`. Isso preserva `CLOEXEC` e remove o syscall separado `fcntl(fd, F_SETFD, FD_CLOEXEC)` por conexão.

Inspiração:

- O código C recente de `r-delorean/c-de-casa` usa `recvmsg(..., MSG_DONTWAIT | MSG_CMSG_CLOEXEC)` no recebimento de FDs.
- A diferença metodológica é importante: não é remover `CLOEXEC`; é aplicar `CLOEXEC` no próprio `recvmsg`.

Execução:

- Alterado `recvmsg(socket_fd, &message, 0)` para `recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC)`.
- Removido o `fcntl(*fd, F_SETFD, FD_CLOEXEC)` posterior.
- Mantido o `fcntl(F_SETFL, O_NONBLOCK)` para preservar nonblocking.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204` após 2s.
- Executadas 3 runs consecutivas.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.04ms | 0% | 0 | 0 | 5984.18 |
| 2 | 1.03ms | 0% | 0 | 0 | 5986.23 |
| 3 | 1.03ms | 0% | 0 | 0 | 5986.44 |

Decisão:

- Aprovado para promoção.
- Todas as runs superaram a submissão oficial anterior `#3537` (`p99=1.04ms`, `final_score=5983.81`).
- Próximo passo: aplicar em `submission`, publicar nova imagem e abrir issue oficial.

Aprendizado:

- Esta é uma otimização sustentável porque reduz syscall preservando a propriedade de segurança desejada.
- A melhora é pequena, mas ao contrário de `EPOLLET` e `reserve(128)`, reproduziu em 3 runs consecutivas.

## Promoção 11h20: submissão `MSG_CMSG_CLOEXEC`

Promoção aplicada:

- Branch `submission` atualizada com o patch `MSG_CMSG_CLOEXEC`.
- Commit de código na `submission`: `00ee6c1 use cloexec on fd recvmsg`.
- Imagem publicada via workflow `Publish GHCR image`: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-00ee6c1`.
- `docker-compose.yml` da `submission` atualizado para a nova tag.
- Commit de compose na `submission`: `f6dec96 point submission to cloexec image`.
- Stack publicado validado localmente com a imagem GHCR puxada.

Validação local da imagem publicada:

| Estado | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `submission-00ee6c1` via GHCR | 1.04ms | 0% | 0 | 0 | 5984.50 |

Submissão oficial:

- Issue aberta: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/3668`.
- Título e descrição usados exatamente como exigido: `rinha/test andrade-cpp-ivf`.
- Resultado oficial ainda pendente no momento deste registro.

## Ciclo 11h30: `MSG_CMSG_CLOEXEC + ioctl(FIONBIO)`

Hipótese:

Depois de aprovar `MSG_CMSG_CLOEXEC`, testar novamente `ioctl(FIONBIO)` fazia sentido porque a causa mudou: agora o FD já chega com `CLOEXEC`, então substituir `F_GETFL + F_SETFL` por um único `ioctl(FIONBIO)` reduziria mais um syscall por FD recebido.

Execução:

- Adicionado temporariamente `#include <sys/ioctl.h>`.
- Substituído temporariamente o par `fcntl(F_GETFL)` + `fcntl(F_SETFL, O_NONBLOCK)` por `ioctl(fd, FIONBIO, &one)`.
- Mantido `MSG_CMSG_CLOEXEC`.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204` após 2s.
- Executadas 3 runs consecutivas.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.03ms | 0% | 0 | 0 | 5986.94 |
| 2 | 1.04ms | 0% | 0 | 0 | 5982.90 |
| 3 | 1.03ms | 0% | 0 | 0 | 5985.96 |

Decisão:

- Rejeitado e revertido.
- Apesar de duas runs boas, a média ficou pior que `MSG_CMSG_CLOEXEC` puro e houve queda abaixo da submissão oficial anterior.
- Não promover.

Aprendizado:

- A otimização de nonblocking por `ioctl` continua instável mesmo depois de remover o syscall de `F_SETFD`.
- O melhor estado técnico permanece `MSG_CMSG_CLOEXEC` + `fcntl(F_GETFL/F_SETFL)` para `O_NONBLOCK`.

## Ciclo 11h40: drenagem em lote do pipe de FDs

Hipótese:

Depois de reduzir um syscall no recebimento do FD, outro possível custo de conexão era a drenagem do pipe de notificação. O código lia um `int` por syscall; ler até 64 FDs por `read()` poderia reduzir overhead em bursts de conexões.

Execução:

- Alterado temporariamente `drain_transferred_fds()` para usar `std::array<int, 64>`.
- Cada `read()` consumia até `64 * sizeof(int)` bytes.
- Mantido o mesmo pipe e o mesmo `add_connection()`.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204` após 2s.
- Executadas 3 runs consecutivas.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.03ms | 0% | 0 | 0 | 5987.15 |
| 2 | 1.04ms | 0% | 0 | 0 | 5983.21 |
| 3 | 1.03ms | 0% | 0 | 0 | 5986.66 |

Decisão:

- Rejeitado e revertido.
- Embora duas runs tenham sido fortes, uma run ficou abaixo da submissão oficial anterior e o ganho médio ficou praticamente empatado com `MSG_CMSG_CLOEXEC` puro.
- Não promover por margem insuficiente.

Aprendizado:

- Há algum sinal de que o caminho de FD handoff ainda influencia a cauda, mas a drenagem em lote não é estável o bastante.
- A alteração também aumenta risco de lidar mal com leituras parciais do pipe; sem ganho claro, o caminho simples permanece melhor.

## Ciclo 11h50: baseline re-freeze e buffer fixo de resposta

Hipótese:

O servidor manual ainda mantinha `std::string out` por conexão, com `reserve(512)` no `add_connection()` e `append()` para cada resposta. Substituir isso por um buffer fixo poderia remover alocação heap e reduzir trabalho no hot path de resposta HTTP.

Baseline antes do patch:

| Estado | p99 | failure_rate | FP | FN | final_score |
|---|---:|---:|---:|---:|---:|
| `MSG_CMSG_CLOEXEC` puro, rebuild limpo | 1.03ms | 0% | 0 | 0 | 5987.23 |

Execução:

- Alterado temporariamente `Connection::out` de `std::string` para `std::array<char, 4096>`.
- Adicionado `out_len/out_pos` e `append_output()` com `memcpy()`.
- Removido temporariamente `conn->out.reserve(512)`.
- Mantidos LB, CPU split, IVF, parser e `MSG_CMSG_CLOEXEC` inalterados.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204`.
- Executadas 3 runs consecutivas.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.05ms | 0% | 0 | 0 | 5978.90 |
| 2 | 1.06ms | 0% | 0 | 0 | 5976.45 |
| 3 | 1.03ms | 0% | 0 | 0 | 5987.32 |

Decisão:

- Rejeitado e revertido.
- Correto funcionalmente, mas pior em 2 de 3 runs.
- Não promover: o ganho de remover heap não compensou o aumento de footprint/cache por conexão.

Aprendizado:

- `std::string` com `reserve(512)` não é gargalo dominante neste estado.
- Em p99 próximo de `1ms`, reduzir alocação aparente pode piorar locality. O hot path atual com resposta curta e string reservada é suficientemente competitivo.
- A issue oficial `#3668` da submissão `submission-00ee6c1` segue aberta sem comentário do runner neste checkpoint.

## Ciclo 12h05: remover `unordered_map` do ownership de conexões

Hipótese:

O loop epoll já usa `event.data.ptr` com `Connection*`; o `unordered_map<int, unique_ptr<Connection>>` servia principalmente como ownership e para `erase()` no fechamento. Remover o mapa poderia eliminar hash/insert/erase por conexão sem mudar protocolo, parser, índice ou LB.

Execução:

- Removido temporariamente `#include <unordered_map>`.
- `add_connection()` passou a criar `Connection` e liberar ownership para o ponteiro registrado no epoll.
- `close_connection()` passou a fazer `delete conn` após `epoll_ctl(DEL)` e `close(fd)`.
- `drain_transferred_fds()` e accept local passaram a chamar `add_connection(epoll_fd, fd)` sem mapa.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204`.
- Executadas 5 runs porque a primeira bateria teve sinal misto.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.05ms | 0% | 0 | 0 | 5980.35 |
| 2 | 1.02ms | 0% | 0 | 0 | 5989.29 |
| 3 | 1.03ms | 0% | 0 | 0 | 5986.21 |
| 4 | 1.04ms | 0% | 0 | 0 | 5982.53 |
| 5 | 1.03ms | 0% | 0 | 0 | 5988.26 |

Decisão:

- Rejeitado e revertido.
- A melhor run isolada foi excelente, mas a distribuição não melhora de forma sustentável o estado aceito.
- Não promover: a variação inclui runs piores que a submissão oficial anterior e não sustenta vantagem clara sobre o baseline re-freezado (`1.03ms`, `5987.23`).

Aprendizado:

- O custo de `unordered_map` não é o limitador estável da cauda.
- A conexão direta por ponteiro não quebrou funcionalidade, mas também não reduziu a dispersão do p99.
- O próximo caminho deve focar em decisões que alterem menos a locality/memória por conexão ou em evidência externa de stacks que estão abaixo de `1.03ms`.

## Ciclo 12h20: `memmem()` para detectar fim dos headers HTTP

Hipótese:

A revisão do código do Jairo mostrou parsing de HTTP mais agressivo com `memchr/memmem`. Nosso `find_header_end()` ainda fazia loop manual byte a byte procurando `\r\n\r\n`. Trocar para `memmem()` da libc poderia reduzir instruções no hot path sem mudar contrato HTTP, parser JSON, IVF ou infraestrutura.

Execução:

- Alterado `find_header_end()` para usar `memmem(buffer, len, "\r\n\r\n", 4)`.
- Mantidos `Content-Length`, roteamento, respostas, LB, split de CPU e índice inalterados.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204`.
- Executadas 6 runs porque as 3 primeiras já indicaram melhora, mas ainda com uma run em `1.04ms`.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.03ms | 0% | 0 | 0 | 5987.67 |
| 2 | 1.04ms | 0% | 0 | 0 | 5983.16 |
| 3 | 1.02ms | 0% | 0 | 0 | 5993.43 |
| 4 | 1.02ms | 0% | 0 | 0 | 5992.63 |
| 5 | 1.01ms | 0% | 0 | 0 | 5994.40 |
| 6 | 1.02ms | 0% | 0 | 0 | 5992.47 |

Decisão:

- Aceito e mantido no branch experimental.
- Promover para `submission`: a alteração é pequena, sem risco de acurácia, sem relaxar regra de negócio, e melhorou a janela local de maneira material.
- Melhor run local do ciclo até agora: `p99=1.01ms`, `final_score=5994.40`, `0%` falhas.

Aprendizado:

- O parsing HTTP ainda tinha margem real mesmo depois das otimizações de LB/FD-passing.
- Ao contrário de mexidas em ownership ou buffer de resposta, a troca por `memmem()` reduz trabalho sem aumentar footprint por conexão.
- A meta teórica local agora se aproxima do teto de `6000`; qualquer próximo ganho precisa mirar a última centésima de ms ou estabilidade para saturar `p99<=1ms`.

## Promoção 12h35: submissão `memmem()` para headers HTTP

Contexto:

A issue `#3668`, aberta para a imagem `submission-00ee6c1` com `MSG_CMSG_CLOEXEC`, fechou com regressão no runner oficial:

| Issue | Imagem | p99 | failure_rate | FP | FN | final_score | Decisão |
|---|---|---:|---:|---:|---:|---:|---|
| `#3668` | `submission-00ee6c1` | 1.07ms | 0% | 0 | 0 | 5972.53 | Não substitui `#3537` |

Promoção:

- Branch `submission` recebeu o patch `memmem()` em `cpp/src/manual_main.cpp`.
- Commit do código: `754954e use memmem for header delimiter`.
- Imagem publicada via workflow `Publish GHCR image`: `ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-754954e`.
- `docker-compose.yml` da branch `submission` atualizado para a nova imagem.
- Commit do compose: `22cdc06 point submission to memmem image`.

Validação local da imagem publicada:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.04ms | 0% | 0 | 0 | 5984.28 |
| 2 | 1.03ms | 0% | 0 | 0 | 5987.45 |
| 3 | 1.04ms | 0% | 0 | 0 | 5984.88 |

Submissão oficial:

- Issue aberta: `https://github.com/zanfranceschi/rinha-de-backend-2026/issues/3693`.
- Título e descrição usados exatamente como exigido: `rinha/test andrade-cpp-ivf`.
- Resultado oficial pendente neste checkpoint.

Decisão:

- Vale submeter apesar da validação GHCR local mais modesta que o build local: as 3 runs publicadas ficaram acima do melhor oficial anterior `#3537` (`5983.81`) e a alteração é tecnicamente segura.
- Risco reconhecido: a issue `#3668` mostrou que o runner oficial pode variar bastante. O resultado oficial de `#3693` precisa ser aguardado antes de considerar esta versão como nova melhor submissão.

## Ciclo 12h50: parser direto de `Content-Length`

Hipótese:

Depois do ganho com `memmem()` no delimitador de headers, o próximo ponto parecido era o parser de `Content-Length`. O código ainda percorria linhas, usava `find(':')`, normalização case-insensitive e trim; uma busca direta por `content-length:` poderia reduzir custo por request.

Execução:

- Alterado temporariamente `parse_content_length()` para buscar `c/C` com `memchr()` e comparar `content-length:` de forma case-insensitive.
- Mantido requisito de início de linha (`pos == 0` ou caractere anterior `\n`) para evitar casar valores de outros headers.
- Mantido `memmem()` em `find_header_end()`.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204`.
- Executadas 3 runs consecutivas.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.03ms | 0% | 0 | 0 | 5985.73 |
| 2 | 1.03ms | 0% | 0 | 0 | 5986.57 |
| 3 | 1.04ms | 0% | 0 | 0 | 5984.52 |

Decisão:

- Rejeitado e revertido.
- Não superou o `memmem()` puro e ainda introduziu warnings de helpers mortos durante o build.
- Não promover.

Aprendizado:

- O gargalo de parsing que importava era encontrar o fim dos headers; o custo adicional do parser antigo de `Content-Length` não apareceu no p99.
- Manter a versão mais simples reduz risco e evita complexidade inútil.
- A issue oficial `#3693` continua aberta sem comentário no momento deste registro.

## Ciclo 13h05: fila de respostas por `string_view`

Hipótese:

O código do Jairo evita copiar respostas para um buffer dinâmico e escreve slices estáticos. O experimento anterior com buffer fixo piorou por footprint/cache, mas uma fila pequena de `std::string_view` por conexão poderia remover cópia de resposta sem aumentar tanto a memória.

Execução:

- Alterado temporariamente `Connection::out` de `std::string` para `std::array<std::string_view, 16>`.
- `append_response()` passou a enfileirar views estáticos para as respostas pré-montadas.
- `flush_output()` passou a enviar cada `string_view` respeitando escrita parcial.
- Removido temporariamente `conn->out.reserve(512)`.
- Mantidos `memmem()` no delimitador, LB, split de CPU, parser JSON e IVF.
- Imagem reconstruída com sucesso.
- Stack recriado; `/ready` respondeu `204`.
- Executadas 3 runs consecutivas.

Resultados locais:

| Run | p99 | failure_rate | FP | FN | final_score |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.02ms | 0% | 0 | 0 | 5990.26 |
| 2 | 1.02ms | 0% | 0 | 0 | 5989.93 |
| 3 | 1.02ms | 0% | 0 | 0 | 5990.73 |

Decisão:

- Rejeitado e revertido.
- O resultado foi competitivo e estável, mas não superou materialmente o `memmem()` puro, que já teve runs melhores (`5993+` e `5994.40`).
- Não promover: a versão aumenta complexidade e introduz limite explícito de respostas enfileiradas por conexão sem ganho claro.

Aprendizado:

- Evitar a cópia de resposta ajuda a manter `p99` em `1.02ms`, mas não parece ser a última barreira para saturar `1.00ms`.
- O `std::string` reservado continua aceitável; o maior ganho confirmado permanece no parser de fim de header.
- A issue oficial `#3693` segue aberta sem comentário neste checkpoint.
