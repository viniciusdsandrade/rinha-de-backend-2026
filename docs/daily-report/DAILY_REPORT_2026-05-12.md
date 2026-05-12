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
