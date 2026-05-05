# Daily Report - 2026-05-04

## Contexto

Branch de investigação: `perf/noon-tuning`.

Objetivo do ciclo: continuar o fluxo hipótese -> experimento -> resultado -> report para buscar melhoria sustentável de performance na submissão C++/uWebSockets/IVF da Rinha de Backend 2026.

## Ciclo 20h: servidor HTTP manual via epoll/UDS

Hipótese: parte do gap restante poderia estar no overhead do uWebSockets para um contrato HTTP extremamente pequeno. Repositórios de topo usam servidores HTTP muito enxutos e respostas pré-montadas, então foi testado um servidor manual opcional em C++ usando `epoll`, Unix Domain Socket e respostas constantes, preservando o parser/classificador existente.

Alteração experimental:

```text
RINHA_MANUAL_HTTP=1
GET /ready -> resposta HTTP manual
POST /fraud-score -> parse do corpo via implementação atual + resposta JSON pré-montada
```

Validação funcional antes da carga:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed, 0 failed
```

Smoke real na porta `9999`:

```http
HTTP/1.1 200 OK
Content-Length: 0
Connection: keep-alive
```

POST de exemplo:

```http
HTTP/1.1 200 OK
Content-Length: 36
Connection: keep-alive

{"approved":false,"fraud_score":1.0}
```

### Problema de harness

O `./run.sh` oficial local falhou antes de iniciar o teste porque o k6 não conseguiu baixar o módulo remoto:

```text
https://jslib.k6.io/k6-summary/0.0.1/index.js
dial tcp 216.40.34.41:443: i/o timeout
```

Esse import não é usado pelo `handleSummary` atual. Para não editar o teste oficial do repositório, foi criada uma cópia temporária fora do repo sem a linha do import remoto. A primeira tentativa temporária também precisou ser descartada porque o diretório temporário não tinha `test/` para gravar `test/results.json`.

### Resultados válidos com harness temporário corrigido

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Manual HTTP sem `Content-Type` | 1.62ms | 0 | 0 | 0 | 5789.88 |
| Manual HTTP sem `Content-Type` | 1.72ms | 0 | 0 | 0 | 5764.41 |
| Controle uWebSockets, mesmo harness temporário | 1.70ms | 0 | 0 | 0 | 5770.34 |

Leitura: a janela ficou claramente mais lenta que o baseline local já observado anteriormente (`~1.22ms-1.25ms`). Como o controle uWebSockets também caiu para `1.70ms`, a comparação absoluta está contaminada por ambiente/harness. Ainda assim, o servidor manual não demonstrou ganho robusto: ficou entre `1.62ms` e `1.72ms`, sem reproduzir a faixa boa histórica.

Decisão: rejeitado por KISS. O patch adicionava uma superfície grande de código HTTP manual e não entregou ganho inquestionável. A implementação foi removida e o compose voltou para o caminho uWebSockets atual.

Validação após remover o experimento:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
Resultado: 100% tests passed, 0 failed
```

## Próximas hipóteses

1. Criar um harness local estável sem dependência de `jslib.k6.io`, mas sem alterar o arquivo oficial `test/test.js`, para evitar bloqueios de rede durante experimentos longos.
2. Revalidar baseline uWebSockets em uma janela fria com 3 runs antes de aceitar qualquer microganho.
3. Priorizar experimentos menores que o servidor manual completo: headers/resposta, recycle de buffers, tuning de compose e ajustes localizados no parser/classificador.

## Ciclo 20h20: runner k6 local sem dependência remota

Hipótese: a investigação estava sendo bloqueada por uma dependência externa (`jslib.k6.io`) que não participa do cálculo de score. Um runner auxiliar local permitiria repetir o benchmark sem editar `test/test.js` nem depender de rede durante cada experimento.

Implementação:

```text
run-local-k6.sh
```

O script cria uma cópia temporária de `test/test.js`, remove apenas a linha de import de `k6-summary`, copia `test/test-data.json`, cria o diretório temporário `test/` necessário para o `handleSummary`, roda `k6`, copia `test/results.json` de volta para o repo e imprime o JSON com `jq`.

Validação:

```text
bash -n run-local-k6.sh
./run-local-k6.sh
```

Resultado de smoke/benchmark no estado uWebSockets atual:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| uWebSockets atual via `run-local-k6.sh` | 1.78ms | 0 | 0 | 0 | 5749.22 |

Leitura: o runner resolveu o bloqueio operacional, mas a máquina/janela continuou mais lenta que a faixa histórica boa (`~1.22ms-1.25ms`). Portanto, esse número serve como baseline da janela atual, não como regressão do código.

Decisão: aceitar o runner como ferramenta de investigação branch-local. Ele não altera o teste oficial e reduz dependência de rede em ciclos longos.

## Ciclo 20h25: benchmark offline do hot path e varredura IVF

Hipótese: com o k6 contaminado por ambiente, benchmarks internos poderiam apontar gargalos com menos ruído antes de qualquer nova alteração em Docker.

Benchmark de decomposição:

```text
./cpp/build/benchmark-request-cpp test/test-data.json resources/references.json.gz 3 54100
```

Resultado:

| Métrica | ns/query |
|---|---:|
| `body_append_default` | 40.13 |
| `body_append_reserve768` | 39.34 |
| `dom_padded_parse` | 343.49 |
| `dom_reserve768_parse` | 632.99 |
| `parse_payload` | 703.26 |
| `parse_vectorize` | 771.62 |
| `parse_classify` | 451672 |

Leitura: parser/vetorização é irrelevante perto do classificador quando medido isoladamente. O resultado também confirma novamente que `reserve(768)` e parse com string reservada não merecem voltar: o ganho de append é sub-ns/baixo, e o parse reservado é pior.

### Varredura de configuração IVF no índice atual

Índice atual copiado do container:

```text
refs=3000000
clusters=1280
index_memory_mb=94.6933
```

Resultados com `benchmark-ivf-cpp`:

| Configuração | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `fast=1 full=1 bbox=1 repair=1..4` | 17741.1 | 0 | 0 | manter |
| `fast=1 full=1 bbox=0 repair=1..4` | 14611.7 | 254 | 262 | rejeitar |
| `fast=1 full=1 bbox=1 repair=2..3` | 20438.8 | 44 | 56 | rejeitar |
| `fast=1 full=1 bbox=1 repair=2..4` | 20633.3 | 0 | 56 | rejeitar |
| `fast=1 full=1 bbox=1 repair=1..3` | 24379.3 | 44 | 0 | rejeitar |
| `fast=1 full=2 bbox=1 repair=1..4` | 22891.2 | 0 | 0 | rejeitar, mais lento |
| `fast=1 full=1 bbox=1 repair=0..5` | 75221.8 | 0 | 0 | rejeitar, muito mais lento |

Leitura: desligar `bbox_repair` é mais rápido, mas os erros de detecção destruiriam mais score do que qualquer ganho de p99. A configuração atual continua sendo o melhor ponto sem erro.

### Varredura de quantidade de clusters

Foram preparados índices alternativos com os mesmos parâmetros de treino (`sample=65536`, `iterations=6`) e apenas `clusters` variando.

| Clusters | Configuração | ns/query | FP | FN | Decisão |
|---:|---|---:|---:|---:|---|
| 1024 | `fast=1 full=1 bbox=1 repair=1..4` | 18763.7 | 2 | 4 | rejeitar |
| 1024 | `fast=1 full=2 bbox=1 repair=1..4` | 16677.8 | 2 | 4 | rejeitar |
| 1536 | `fast=1 full=1 bbox=1 repair=1..4` | 17251.4 | 4 | 0 | rejeitar |
| 1536 | `fast=1 full=1 bbox=0 repair=1..4` | 13750.1 | 262 | 268 | rejeitar |
| 2048 | `fast=1 full=1 bbox=1 repair=1..4` | 19253.2 | 6 | 6 | rejeitar |
| 2048 | `fast=1 full=1 bbox=0 repair=1..4` | 15318.8 | 286 | 296 | rejeitar |

Leitura: `1536` parece levemente mais rápido offline, mas já introduz erro. Pela fórmula, mesmo poucos FP/FN quebram o teto de `detection_score=3000`, então a troca não é sustentável. O índice `1280` segue sendo o melhor compromisso medido: 0 erro e custo competitivo.

Decisão: não alterar `Dockerfile`, índice nem variáveis IVF.

## Ciclo 20h35: investigação em repositórios líderes e ordem de prune AVX2

Fontes consultadas:

- `https://github.com/thiagorigonatti/rinha-2026`
- `https://github.com/joojf/rinha-2026`

Achados relevantes:

- A solução C líder usa C + `io_uring` + HAProxy, respostas HTTP pré-montadas, UDS e IVF quantizado.
- A solução Rust usa parser manual de bytes, monoio, respostas HTTP pré-montadas e busca IVF quantizada.
- Ambas evitam parser JSON genérico no hot path e tratam o score como contagem inteira de fraudes.
- A implementação C líder usa uma ordem manual de dimensões no scan escalar (`5,6,2,0,7,8,11,12,9,10,1,13,3,4`) para tentar cortar cedo por dimensões discriminativas.

Hipótese testada: aplicar a mesma ordem de dimensões no nosso `scan_blocks_avx2`, mantendo o cálculo exato/quantizado e só mudando a ordem de acumulação antes do prune parcial.

Validação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp rinha-backend-2026-cpp-tests -j4
ctest --test-dir cpp/build --output-on-failure
```

Resultado:

| Variante | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| Ordem original AVX2 | 17741.1 | 0 | 0 | referência da rodada |
| Ordem estilo líder C no prune AVX2 | 21701.9 | 0 | 0 | rejeitar |

Leitura: a ideia faz sentido no escalar, mas piorou nosso AVX2. A hipótese provável é que o nosso layout transposto por blocos e o ponto de prune depois de 8 dimensões favorecem a ordem sequencial atual; reordenar acessos aumenta custo/cache ou piora o código gerado mais do que ajuda o corte.

Decisão: rejeitado e revertido. Nenhuma alteração de código foi mantida.

## Ciclo 20h45: HAProxy HTTP com UDS como substituto do nginx

Hipótese: a solução C líder usa HAProxy em HTTP mode com UDS e `http-reuse always`. Talvez o HAProxy pudesse reduzir overhead do LB em relação ao nginx atual.

Alteração experimental:

```text
nginx:1.27-alpine -> haproxy:3.3
backend via unix@/sockets/api1.sock e unix@/sockets/api2.sock
balance roundrobin
http-reuse always
```

Validação funcional:

```text
docker compose config
docker compose up -d --force-recreate --remove-orphans
curl http://localhost:9999/ready
Resultado: ready após duas conexões resetadas durante startup
```

Benchmark:

```text
./run-local-k6.sh
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| HAProxy 3.3 HTTP/UDS | 36.96ms | 0 | 0 | 0 | 4432.31 |
| nginx atual na mesma janela, referência recente | 1.78ms | 0 | 0 | 0 | 5749.22 |

Leitura: HAProxy nessa configuração é muito pior localmente. Pode funcionar bem na solução C líder por causa do servidor `io_uring` e do desenho completo do stack, mas como swap isolado de LB para nossa API uWebSockets/UDS não é sustentável.

Decisão: rejeitado e revertido para nginx. Nenhuma alteração de infra foi mantida.

## Ciclo 21h00: `seccomp=unconfined` em APIs e LB

Hipótese: soluções líderes usam `security_opt: seccomp=unconfined`, principalmente para caminhos de I/O mais baixo nível. Mesmo sem `io_uring`, remover o filtro seccomp padrão do Docker poderia reduzir overhead marginal de syscalls no caminho nginx/API.

Alteração:

```yaml
security_opt:
  - seccomp=unconfined
```

Aplicada em:

- `api1`/`api2` via âncora comum.
- `nginx`.

Validação:

```text
docker compose config
docker compose up -d --force-recreate --remove-orphans
curl http://localhost:9999/ready
Resultado: ready
```

Resultados:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Sem `seccomp=unconfined`, baseline anterior da janela | 1.78ms | 0 | 0 | 0 | 5749.22 |
| Com `seccomp=unconfined`, run 1 | 1.60ms | 0 | 0 | 0 | 5796.73 |
| Com `seccomp=unconfined`, run 2 | 1.59ms | 0 | 0 | 0 | 5798.91 |
| A/B reverso sem `seccomp=unconfined` | 1.63ms | 0 | 0 | 0 | 5788.78 |

Leitura: o ganho é pequeno, mas reproduziu na mesma janela e não alterou comportamento de detecção. O A/B reverso não voltou para o pior `1.78ms`, então parte do ganho inicial era ambiente; ainda assim, `seccomp=unconfined` ficou consistentemente melhor que o controle imediato (`1.59-1.60ms` vs `1.63ms`).

Risco regulatório: a regra oficial proíbe `privileged` e `network_mode: host`; não há proibição explícita de `security_opt`. As submissões líderes consultadas também usam esse ajuste. Ainda assim, por ser uma opção de segurança do container, deve ser revalidada antes de promover para `submission`.

Decisão inicial: aceitar como candidato branch-local para reamostragem; não abrir submissão oficial só por esse ganho marginal sem novo bloco robusto.

## Ciclo 21h10: `ulimits.nofile=65535`

Hipótese: soluções líderes configuram `nofile=65535`; mesmo que o teste não pareça bater limite de descritores, aumentar o limite poderia reduzir risco de cauda em pico de conexões.

Alteração experimental:

```yaml
ulimits:
  nofile:
    soft: 65535
    hard: 65535
```

Aplicada em APIs e nginx, sobre o estado já com `seccomp=unconfined`.

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `seccomp=unconfined` sem `ulimits`, referência | 1.59ms-1.60ms | 0 | 0 | 0 | 5796.73-5798.91 |
| `seccomp=unconfined` + `ulimits.nofile=65535` | 1.62ms | 0 | 0 | 0 | 5789.72 |

Leitura: não houve sinal de ganho; ficou pior que o estado `seccomp` puro. O teste local não parece limitado por `nofile`.

Decisão: rejeitado e revertido. Manter apenas `seccomp=unconfined`.

## Ciclo 21h25: escopo mínimo do `seccomp=unconfined`

Hipótese: talvez o ajuste de `seccomp=unconfined` precisasse ficar apenas nas APIs, reduzindo superfície de configuração no LB.

Alteração experimental:

```text
api1/api2: seccomp=unconfined
nginx: seccomp padrão do Docker
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `seccomp=unconfined` em APIs + nginx | 1.59ms-1.60ms | 0 | 0 | 0 | 5796.73-5798.91 |
| `seccomp=unconfined` apenas nas APIs | 1.64ms | 0 | 0 | 0 | 5785.66 |

Leitura: reduzir o escopo para apenas APIs piorou a cauda. O pequeno ganho observado parece depender do nginx também, ou pelo menos da recriação completa com ambos sem seccomp.

Decisão: rejeitar escopo parcial e restaurar `seccomp=unconfined` em APIs e nginx para mais uma medição.

## Ciclo 21h35: reamostragem final do `seccomp=unconfined`

Hipótese: se o ganho fosse real, uma nova medição com `seccomp=unconfined` em APIs e nginx deveria continuar próxima de `1.59ms-1.60ms`.

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `seccomp=unconfined` em APIs + nginx, reamostragem final | 1.63ms | 0 | 0 | 0 | 5786.57 |
| Controle reverso sem `seccomp=unconfined` | 1.63ms | 0 | 0 | 0 | 5788.78 |

Leitura: a reamostragem final empatou com o controle sem `seccomp`. O ganho inicial existiu na janela, mas não sustentou evidência suficiente para ser chamado de inquestionável.

Decisão final: rejeitado por sustentabilidade. `docker-compose.yml` voltou ao estado sem `security_opt`; manter apenas o aprendizado no relatório.

## Ciclo 21h00: nginx HTTP proxy vs stream L4

Hipótese: a solução Rust líder usa nginx em modo `http` com upstream keepalive. Talvez manter conexões upstream HTTP pudesse ganhar contra o `stream` L4 atual.

Alteração experimental:

```nginx
http {
    upstream api {
        server unix:/sockets/api1.sock;
        server unix:/sockets/api2.sock;
        keepalive 256;
    }

    server {
        listen 9999 backlog=4096;
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

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| nginx `http` + upstream keepalive | 45.88ms | 0 | 0 | 0 | 4338.37 |
| nginx `stream` L4 atual, referência da janela | 1.63ms-1.78ms | 0 | 0 | 0 | 5749.22-5788.78 |

Leitura: para nossa API uWebSockets atrás de UDS, o `stream` L4 é muito superior. O modo `http` adiciona trabalho no LB e explode p99, mesmo sem erro HTTP.

Decisão: rejeitado e revertido. Manter nginx `stream`.

## Ciclo 21h05: IVF com menos clusters

Hipótese: a solução C líder usa um IVF mais grosso; talvez menos clusters reduzissem custo de seleção de centroides sem perder precisão na nossa implementação.

Foram preparados índices com `sample=65536`, `iterations=6` e variação apenas em `clusters`.

Resultado:

| Clusters | ns/query | FP | FN | Decisão |
|---:|---:|---:|---:|---|
| 256 | 44810.0 | 2 | 2 | rejeitar |
| 512 | 27211.1 | 0 | 2 | rejeitar |
| 768 | 20698.2 | 4 | 0 | rejeitar |
| 1280 atual | 17741.1 | 0 | 0 | manter |

Leitura: reduzir clusters aumentou o tamanho dos grupos e deixou o repair mais caro, além de introduzir erros. A escolha atual `1280` segue sendo o único ponto da varredura ampla com 0 erro e custo competitivo.

Decisão: não alterar índice nem `Dockerfile`.

## Ciclo 21h10: treino IVF mais caro com `1280` clusters

Hipótese: mantendo `1280` clusters, aumentar iterações de k-means ou amostra de treino poderia melhorar a qualidade dos clusters, reduzir repair/scan e preservar 0 erro.

Resultado:

| Variante de build | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| `clusters=1280 sample=65536 iterations=6` atual | 17741.1 | 0 | 0 | manter |
| `clusters=1280 sample=65536 iterations=8` | 18292.4 | 0 | 2 | rejeitar |
| `clusters=1280 sample=131072 iterations=6` | 20169.8 | 4 | 4 | rejeitar |

Leitura: treinar mais não melhorou o índice para o dataset de teste; pelo contrário, introduziu erros e aumentou custo. A versão atual parece melhor calibrada para o conjunto rotulado local.

Decisão: não alterar parâmetros de build do índice.

## Ciclo 21h15: `-mtune=native`

Hipótese: manter `-march=x86-64-v3`, mas adicionar `-mtune=native`, poderia melhorar o agendamento de instruções sem introduzir novas instruções além do requisito AVX2 efetivo.

Alteração experimental:

```cmake
-mavx2 -mfma -march=x86-64-v3 -mtune=native
```

Aplicada apenas ao binário principal e ao `benchmark-ivf-cpp` para medição offline.

Resultado:

| Variante | ns/query | FP | FN | Decisão |
|---|---:|---:|---:|---|
| Build atual, referência offline registrada | 17741.1 | 0 | 0 | manter |
| `-mtune=native` | 19634.1 | 0 | 0 | rejeitar |
| Após revert, sanity check | 18356.9 | 0 | 0 | restaurado |

Leitura: não houve ganho; a diferença ficou contra a mudança. Além disso, `-mtune=native` é dependente do host de build, então mesmo um ganho pequeno exigiria muito mais cautela antes de submissão.

Decisão: rejeitado e revertido. Flags continuam em `-mavx2 -mfma -march=x86-64-v3`.

## Ciclo 21h20: benchmark do estado restaurado

Depois das reversões de HAProxy, nginx HTTP, `seccomp`, `ulimits`, clusters alternativos e flags, foi executada uma medição do estado limpo atual para registrar a referência da janela.

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Estado restaurado atual | 1.63ms | 0 | 0 | 0 | 5787.14 |

Leitura: a janela permaneceu mais lenta que o melhor histórico local (`~1.22ms-1.25ms`), mas o estado limpo está coerente com os controles recentes (`1.63ms`). Não há evidência de regressão persistente de código após as reversões.

## Ciclo 21h15: reamostragem de CPU split `0.42/0.42/0.16`

Hipótese: o split antigo `api=0.42` cada e `nginx=0.16` já foi competitivo em outras janelas; poderia voltar a ganhar no estado atual.

Resultado:

| Split | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| APIs `0.41/0.41`, nginx `0.18`, referência restaurada | 1.63ms | 0 | 0 | 0 | 5787.14 |
| APIs `0.42/0.42`, nginx `0.16` | 1.68ms | 0 | 0 | 0 | 5773.49 |

Leitura: tirar CPU do nginx piorou a cauda nesta janela. O LB ainda precisa da fatia `0.18` para segurar o ramp local.

Decisão: rejeitado e revertido para `0.41/0.41/0.18`.

## Ciclo 21h20: nginx sem `reuseport`

Hipótese: como o nginx usa apenas `worker_processes 1`, `reuseport` poderia ser redundante no `listen` e talvez simplificar a socket externa.

Alteração experimental:

```nginx
listen 9999 backlog=4096;
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `listen 9999 reuseport backlog=4096`, referência | 1.63ms | 0 | 0 | 0 | 5787.14 |
| `listen 9999 backlog=4096` | 1.64ms | 0 | 0 | 0 | 5786.48 |

Leitura: remover `reuseport` não trouxe ganho e ficou ligeiramente pior. Mesmo com um worker, manter a configuração atual não custa score na janela.

Decisão: rejeitado e revertido. Manter `reuseport`.

## Ciclo 21h35: nginx com `worker_processes 2`

Hipótese: com `reuseport` ativo na porta externa, permitir dois workers no nginx poderia reduzir fila no LB durante a rampa do k6 sem colocar lógica de aplicação no balanceador.

Alteração experimental:

```nginx
worker_processes 2;
```

Resultado:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| `worker_processes 1`, referência restaurada | controle | 1.63ms | 0 | 0 | 0 | 5787.14 |
| `worker_processes 2` | 1 | 1.61ms | 0 | 0 | 0 | 5792.06 |
| `worker_processes 2` | 2 | 1.57ms | 0 | 0 | 0 | 5803.79 |

Leitura: o ganho é pequeno, mas apareceu em duas medições consecutivas e não alterou contrato, topologia, recursos declarados, nem lógica de negócio do LB. Como a mudança é apenas operacional e reversível, fica como candidato aceito provisório para a branch de investigação.

Decisão: manter `worker_processes 2` por enquanto e continuar medindo em novos ciclos. Ainda não supera a melhor submissão oficial/histórica, portanto não justifica nova issue oficial isoladamente.

## Ciclo 21h30: nginx `multi_accept off` com 2 workers

Hipótese: depois de aceitar provisoriamente `worker_processes 2`, manter `multi_accept on` poderia causar rajadas de accept em um worker e piorar a distribuição efetiva entre os workers. Com `reuseport` ativo, testar `multi_accept off` é uma mudança pequena para reduzir essa possibilidade sem alterar lógica de aplicação.

Alteração experimental:

```nginx
events {
    worker_connections 4096;
    multi_accept off;
    use epoll;
}
```

Resultado:

| Variante | Run | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|---:|
| `worker_processes 2`, `multi_accept on` | melhor repetida | 1.57ms | 0 | 0 | 0 | 5803.79 |
| `worker_processes 2`, `multi_accept off` | 1 | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `worker_processes 2`, `multi_accept off` | 2 | 1.21ms | 0 | 0 | 0 | 5915.55 |

Leitura: o ganho foi grande o suficiente para sair da faixa normal de ruído desta janela e foi reproduzido em duas execuções consecutivas. O resultado volta ao patamar dos melhores históricos locais e supera a submissão oficial anterior registrada localmente (`p99 1.44ms`, `final_score 5842.99`), ainda sem introduzir erros de detecção ou HTTP.

Decisão: aceitar `worker_processes 2` + `multi_accept off` como melhor configuração de LB encontrada hoje. Próximo passo: validar se a configuração deve ser promovida para `submission` e, se mantiver o desempenho, abrir nova issue oficial.

## Ciclo 21h40: promoção e validação na branch `submission`

A configuração aceita foi promovida para a branch oficial `submission` no commit `4d5bedb`:

```nginx
worker_processes 2;

events {
    worker_connections 4096;
    multi_accept off;
    use epoll;
}
```

Validação executada contra o compose da própria branch `submission`, usando a imagem pública já declarada no `docker-compose.yml` e o `nginx.conf` publicado:

| Branch/estado | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `submission` publicada, commit `4d5bedb` | 1.22ms | 0 | 0 | 0 | 5912.31 |

Leitura: o resultado reproduz o ganho observado no worktree de investigação e supera a submissão oficial anterior registrada localmente (`p99 1.44ms`, `final_score 5842.99`). A mudança é restrita ao nginx, preserva limites de CPU/memória, `bridge`, duas APIs, LB sem lógica de aplicação e imagem pública.

Decisão: preparar nova issue oficial de submissão usando a branch `submission` atual.

## Ciclo 21h45: nova issue oficial aberta

Com a branch `submission` publicada em `4d5bedb` e validada localmente em `p99 1.22ms` / `final_score 5912.31`, foi aberta uma nova issue oficial para execução no runner da Rinha:

- Issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/1314
- Título: `andrade-cpp-ivf`
- Corpo: `rinha/test andrade-cpp-ivf`

Expectativa: a avaliação oficial deve buscar o repositório público `https://github.com/viniciusdsandrade/rinha-de-backend-2026`, branch `submission`, e registrar no comentário o commit avaliado. O commit esperado é `4d5bedb`.

Resultado oficial retornado:

| Issue | Commit | p99 | FP | FN | HTTP errors | final_score |
|---|---|---:|---:|---:|---:|---:|
| #1314 | `4d5bedb` | 1.43ms | 0 | 0 | 0 | 5844.41 |

Comparação com a submissão anterior `#770`:

| Issue | Commit | p99 | final_score | Delta score |
|---|---|---:|---:|---:|
| #770 | `e3fdd2b` | 1.44ms | 5842.99 | referência |
| #1314 | `4d5bedb` | 1.43ms | 5844.41 | +1.42 |

Leitura: a melhoria local (`1.18ms-1.22ms`) não se transferiu integralmente para o runner oficial, mas a submissão oficial nova ainda melhorou levemente o score e manteve 0 falhas. O achado é válido, porém o runner oficial mostra que a mudança é incremental, não uma quebra de patamar pública.

## Ciclo 21h55: reamostragem de split CPU após nginx 2 workers

Hipótese: depois do ganho com `worker_processes 2` + `multi_accept off`, talvez o nginx precisasse de menos CPU e a API se beneficiasse de voltar para `0.42/0.42/0.16`, que havia sido rejeitado antes no desenho antigo do LB.

Alteração experimental:

```yaml
api1/api2: cpus "0.42"
nginx:     cpus "0.16"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `0.41/0.41/0.18`, melhor run com nginx novo | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `0.41/0.41/0.18`, validação em `submission` | 1.22ms | 0 | 0 | 0 | 5912.31 |
| `0.42/0.42/0.16` | 1.22ms | 0 | 0 | 0 | 5913.50 |

Leitura: o split alternativo é competitivo, mas não supera a melhor configuração e reduz a fatia do nginx justamente depois de termos melhorado o comportamento do accept. Como o ganho é inexistente na prática, a versão mais conservadora continua sendo `0.41/0.41/0.18`.

Decisão: rejeitado e revertido. Manter CPU split publicado.

## Ciclo 22h00: nginx `worker_processes 3`

Hipótese: se dois workers melhoraram o accept externo, talvez três workers ainda reduzissem fila no LB. O teste manteve `multi_accept off`, `reuseport` e o split CPU publicado.

Alteração experimental:

```nginx
worker_processes 3;
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `worker_processes 2`, melhor run | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `worker_processes 2`, validação em `submission` | 1.22ms | 0 | 0 | 0 | 5912.31 |
| `worker_processes 3` | 1.23ms | 0 | 0 | 0 | 5910.22 |

Leitura: 3 workers preserva estabilidade, mas não melhora p99. Provavelmente aumenta coordenação/competição dentro da mesma fatia de `0.18` CPU do nginx.

Decisão: rejeitado e revertido. Manter `worker_processes 2`.

## Ciclo 22h10: mais CPU para nginx (`0.40/0.40/0.20`)

Hipótese: como o ganho oficial foi pequeno, talvez o runner oficial estivesse mais sensível ao LB do que o ambiente local. Aumentar nginx de `0.18` para `0.20` e reduzir APIs para `0.40/0.40` testaria se a borda precisava de mais fatia de CPU.

Alteração experimental:

```yaml
api1/api2: cpus "0.40"
nginx:     cpus "0.20"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `0.41/0.41/0.18`, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `0.41/0.41/0.18`, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `0.40/0.40/0.20` | 1.28ms | 0 | 0 | 0 | 5892.61 |

Leitura: mais CPU no nginx não compensou a perda de CPU nas APIs. O classificador ainda precisa da fatia atual para segurar cauda; a suspeita de LB limitado por CPU não se confirmou localmente.

Decisão: rejeitado e revertido. Manter `0.41/0.41/0.18`.

## Ciclo 22h20: nginx `worker_connections 1024`

Hipótese: a carga local não deveria exigir `4096` conexões por worker; reduzir para `1024` poderia diminuir estruturas internas do nginx sem afetar capacidade.

Alteração experimental:

```nginx
worker_connections 1024;
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `worker_connections 4096`, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `worker_connections 4096`, validação em `submission` | 1.22ms | 0 | 0 | 0 | 5912.31 |
| `worker_connections 1024` | 1.22ms | 0 | 0 | 0 | 5912.89 |

Leitura: a redução não trouxe ganho claro. Como `4096` dá mais margem em ambiente oficial sem custo observado, não há motivo para reduzir.

Decisão: rejeitado e revertido. Manter `worker_connections 4096`.

## Ciclo 22h30: nginx `backlog=8192`

Hipótese: se o runner oficial tiver rajadas de conexão mais agressivas, aumentar a fila de accept externa de `4096` para `8192` poderia reduzir recusas/esperas na borda sem mexer nas APIs.

Alteração experimental:

```nginx
listen 9999 reuseport backlog=8192;
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `backlog=4096`, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `backlog=4096`, validação em `submission` | 1.22ms | 0 | 0 | 0 | 5912.31 |
| `backlog=8192` | 1.22ms | 0 | 0 | 0 | 5912.23 |

Leitura: aumentar backlog não trouxe ganho. A fila externa não aparece como gargalo no ambiente local, e o valor atual já é amplo para o cenário.

Decisão: rejeitado e revertido. Manter `backlog=4096`.

## Ciclo 22h40: nginx `worker_cpu_affinity auto`

Hipótese: fixar automaticamente os dois workers do nginx em CPUs distintas poderia reduzir migração de CPU e estabilizar a cauda do LB.

Alteração experimental:

```nginx
worker_processes 2;
worker_cpu_affinity auto;
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Sem afinidade, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| Sem afinidade, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `worker_cpu_affinity auto` | 1.51ms | 0 | 0 | 0 | 5820.79 |

Leitura: pinning piorou a cauda local, provavelmente por interagir mal com quota de CPU em container e scheduler do Docker. O nginx se comporta melhor deixando o kernel agendar livremente dentro da cota.

Decisão: rejeitado e revertido. Não usar afinidade manual no nginx.

## Ciclo 22h50: imagem `nginx:1.29-alpine`

Hipótese: uma versão mais nova do nginx poderia trazer melhorias no módulo `stream` ou no pacote Alpine sem mudar a configuração da aplicação. Antes do teste, foi verificado que as tags `nginx:1.29-alpine` e `nginx:1.28-alpine` existem; tag flutuante `nginx:alpine` foi descartada por timeout de registry e por ser menos reprodutível.

Alteração experimental:

```yaml
nginx:
  image: nginx:1.29-alpine
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `nginx:1.27-alpine`, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `nginx:1.27-alpine`, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `nginx:1.29-alpine` | 1.22ms | 0 | 0 | 0 | 5912.65 |

Leitura: a imagem nova é funcional, mas não melhora o p99 local. Como troca de imagem aumenta superfície de variância oficial e não mostrou ganho, não vale promover.

Decisão: rejeitado e revertido. Manter `nginx:1.27-alpine`.
