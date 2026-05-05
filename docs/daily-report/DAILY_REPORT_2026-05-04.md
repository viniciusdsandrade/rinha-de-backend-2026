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

## Fechamento da rodada noturna

Estado final aplicado na branch `perf/noon-tuning` e promovido para `submission`:

```nginx
worker_processes 2;

events {
    worker_connections 4096;
    multi_accept off;
    use epoll;
}
```

```yaml
api1/api2: cpus "0.41", memory "165MB"
nginx:     cpus "0.18", memory "20MB"
```

Melhores resultados da rodada:

| Ambiente | Estado | p99 | FP | FN | HTTP errors | final_score |
|---|---|---:|---:|---:|---:|---:|
| Local | melhor run com `worker_processes 2` + `multi_accept off` | 1.18ms | 0 | 0 | 0 | 5927.14 |
| Local | validação na branch `submission` publicada | 1.22ms | 0 | 0 | 0 | 5912.31 |
| Local | controle final restaurado | 1.23ms | 0 | 0 | 0 | 5908.34 |
| Oficial | issue #1314, commit `4d5bedb` | 1.43ms | 0 | 0 | 0 | 5844.41 |

Comparativo oficial:

| Submissão | Commit | p99 | final_score |
|---|---|---:|---:|
| #770 anterior | `e3fdd2b` | 1.44ms | 5842.99 |
| #1314 nova | `4d5bedb` | 1.43ms | 5844.41 |

Ganho oficial confirmado: `+1.42` pontos e `-0.01ms` de p99, mantendo 0% de falhas.

Resumo das decisões:

| Experimento | Decisão |
|---|---|
| `worker_processes 2` | aceito |
| `multi_accept off` | aceito |
| `worker_processes 3` | rejeitado |
| `worker_processes auto` | rejeitado |
| splits CPU `0.42/0.42/0.16`, `0.40/0.40/0.20`, `0.405/0.405/0.19`, `0.415/0.415/0.17` | rejeitados |
| `worker_connections 1024` | rejeitado |
| `backlog=8192` | rejeitado |
| `worker_cpu_affinity auto` | rejeitado |
| `nginx:1.29-alpine` | rejeitado |
| `worker_rlimit_nofile` + `ulimits` + `somaxconn` | rejeitado |

Leitura final: a única melhoria sustentável encontrada nesta rodada foi ajustar o comportamento de accept do nginx com 2 workers fixos e `multi_accept off`. Localmente isso parece grande, mas no runner oficial virou um ganho pequeno; portanto os próximos saltos provavelmente exigem mexer no caminho API/classificador ou numa estratégia de LB substancialmente diferente, não mais em microtuning de nginx.

## Ciclo 23h35: timeouts menores no nginx

Hipótese: reduzir timeouts do nginx poderia diminuir retenção de conexões problemáticas e suavizar cauda sem impactar requisições normais.

Alteração experimental:

```nginx
proxy_connect_timeout 50ms;
proxy_timeout 5s;
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Estado publicado, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| Estado publicado, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| Timeouts menores | 1.24ms | 0 | 0 | 0 | 5905.61 |

Leitura: não houve ganho e a configuração ficou pior que a melhor banda local. Como o teste oficial não apresentou HTTP errors, encurtar timeouts não ataca um problema real observado.

Decisão: rejeitado e revertido. Manter `proxy_connect_timeout 1s` e `proxy_timeout 30s`.

## Ciclo 23h45: AVX2 no cálculo de centróide IVF

Hipótese: a busca do centróide mais próximo em `IvfIndex::fraud_count_once_fixed<1>` ainda percorre `1280 × 14` dimensões em escalar. Como os centróides estão em layout SoA (`centroids_[dim * clusters + cluster]`), uma versão AVX2 em blocos de 8 centróides poderia reduzir o custo antes do scan dos blocos.

Alteração experimental temporária:

- Adicionar `nearest_probe_avx2()` com `_mm256_loadu_ps` e FMA sobre 8 centróides por vez.
- Usar essa rotina apenas quando `MaxNprobe == 1`, que é a configuração publicada (`fast_nprobe=1`, `full_nprobe=1`).

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Referência offline anterior do índice atual | ~18k | 0 | 0 | 0 | base histórica |
| AVX2 centróide, run 1 | 28613.2 | 0 | 0 | 0 | rejeitar |
| AVX2 centróide, run 2 | 29203.1 | 0 | 0 | 0 | rejeitar |
| Após revert, sanity check local | 39491.9 | 0 | 0 | 0 | ambiente ruidoso |

Leitura: a versão AVX2 não mostrou ganho; pelo contrário, as primeiras duas medições ficaram bem piores que a referência histórica. O sanity após revert ficou ainda mais lento, indicando ruído forte de máquina nessa janela, mas não há evidência positiva suficiente para manter a mudança. Como a regra da rodada é melhoria sustentável e inquestionável, a decisão correta é descartar.

Decisão: rejeitado e revertido. Nenhuma mudança de código foi mantida.

## Ciclo 23h55: janela de repair IVF menor

Hipótese: a configuração publicada repara resultados de borda com `repair_min=1` e `repair_max=4`. Reduzir essa janela poderia cortar trabalho do classificador e melhorar p99, desde que mantivesse 0 erro.

Resultado offline sobre `test/test-data.json`, `54100` amostras:

| repair_min | repair_max | ns/query | FP | FN | parse_errors | Decisão |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 4 | 45754.3 | 0 | 0 | 0 | manter |
| 1 | 3 | 50069.3 | 22 | 0 | 0 | rejeitar |
| 2 | 4 | 44165.2 | 0 | 28 | 0 | rejeitar |
| 2 | 3 | 44921.2 | 22 | 28 | 0 | rejeitar |

Leitura: reduzir a janela de repair quebra a precisão. Mesmo quando uma variante fica numericamente mais rápida, qualquer FP/FN derruba o `detection_score`, então a troca não é aceitável.

Decisão: rejeitado. Manter `IVF_REPAIR_MIN_FRAUDS=1` e `IVF_REPAIR_MAX_FRAUDS=4`.

## Ciclo 00h05: repair direto sem `boundary_full`

Hipótese: a configuração atual faz uma busca rápida sem repair e, se o resultado cai na fronteira, repete com bbox repair. Forçar `IVF_BOUNDARY_FULL=false` faria o repair diretamente na primeira passagem, evitando a segunda busca em casos de borda.

Alteração experimental:

```yaml
IVF_BOUNDARY_FULL: "false"
IVF_BBOX_REPAIR: "true"
IVF_REPAIR_MIN_FRAUDS: "1"
IVF_REPAIR_MAX_FRAUDS: "4"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Estado publicado, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| Estado publicado, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `boundary_full=false` | 1.65ms | 0 | 0 | 0 | 5783.70 |

Leitura: a precisão foi preservada, mas o p99 piorou bastante. Reparar toda consulta de primeira custa mais do que repetir seletivamente só as consultas de borda.

Decisão: rejeitado e revertido. Manter `IVF_BOUNDARY_FULL=true`.

## Ciclo 00h15: `IVF_FULL_NPROBE=2`

Hipótese: aumentar apenas o nprobe da passagem de repair para 2 poderia reduzir casos difíceis sem afetar a busca rápida inicial. Offline, essa variante preservou 0 erros e apareceu competitiva em uma janela ruidosa, então mereceu k6 real.

Resultados offline:

| fast_nprobe | full_nprobe | bbox_repair | FP | FN | ns/query |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | true | 0 | 0 | 45754.3 |
| 1 | 2 | true | 0 | 0 | 41681.7 |
| 2 | 2 | true | 0 | 0 | 49086.4 |
| 2 | 2 | false | 41 | 38 | 43086.2 |

Resultado k6:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Estado publicado, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| Estado publicado, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `IVF_FULL_NPROBE=2` | 1.37ms | 0 | 0 | 0 | 5863.46 |

Leitura: a medição offline não se transferiu para o k6. Aumentar `full_nprobe` mantém precisão, mas amplia trabalho nas requisições de borda e piora a cauda.

Decisão: rejeitado e revertido. Manter `IVF_FULL_NPROBE=1`.

## Ciclo 00h25: leitura do 2º colocado e LB `so-no-forevis`

Fonte consultada: `https://github.com/jairoblatt/rinha-2026-rust`.

Achados principais do segundo colocado:

| Área | Achado | Aplicabilidade para nós |
|---|---|---|
| Runtime API | Rust + `monoio`/`io_uring` sobre Unix sockets | estrutural; exigiria trocar uWebSockets/runtime |
| LB | imagem pública `jrblatt/so-no-forevis:v0.0.2`, TCP na porta 9999 e upstreams UDS | testável como drop-in |
| Segurança | `seccomp=unconfined` é necessário para `io_uring` | necessário para o LB subir localmente |
| HTTP | parser HTTP manual com buffer por conexão, pipeline e `writev` de respostas pré-montadas | estrutural; nosso uWS não expõe o mesmo hot path |
| KNN | `K=4096`, `FAST_NPROBE=8`, `FULL_NPROBE=24`, retry quando resultado rápido é 2 ou 3 | arquitetura de índice diferente; não transferiu diretamente aos nossos testes de nprobe |
| Vetorização | arredondamento em 4 casas antes da busca | nosso IVF já quantiza query em escala 10000 para blocos; impacto provável só na seleção de centróide |

Experimento drop-in do LB:

```yaml
nginx:
  image: jrblatt/so-no-forevis:v0.0.2
  environment:
    UPSTREAMS: /sockets/api1.sock,/sockets/api2.sock
    PORT: "9999"
    BUF_SIZE: "4096"
    WORKERS: "1"
  security_opt:
    - seccomp=unconfined
```

Sem `seccomp=unconfined`, o LB falhou ao iniciar com `failed to build IoUring runtime: Operation not permitted`.

Resultado k6 com o LB funcionando:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| nginx publicado, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| nginx publicado, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `jrblatt/so-no-forevis:v0.0.2` | 1.64ms | 0 | 0 | 0 | 5784.67 |

Leitura: o LB próprio do segundo colocado não é um ganho drop-in para nossa stack. Ele provavelmente foi calibrado para o servidor monoio/manual dele; com uWebSockets atrás de UDS, nosso nginx `stream` ajustado segue melhor.

Decisão: rejeitado e revertido. Manter nginx.

## Síntese comparativa dos dois primeiros colocados

Fontes consultadas:

- `https://github.com/thiagorigonatti/rinha-2026`
- `https://github.com/jairoblatt/rinha-2026-rust`

Comparação com nossa solução atual:

| Tema | 1º `thiagorigonatti-c` | 2º `jairoblatt-rust` | Nossa solução | Aprendizado |
|---|---|---|---|---|
| Servidor API | C manual + `io_uring` | Rust `monoio`/`io_uring` | C++ uWebSockets | o maior gap estrutural parece estar no runtime/HTTP |
| LB | HAProxy 3.3 HTTP/UDS | LB próprio `so-no-forevis`/UDS | nginx `stream`/UDS | HAProxy e LB do Jairo não foram ganhos drop-in para nós |
| HTTP response | resposta HTTP completa pré-montada | resposta HTTP completa pré-montada | body JSON constante via uWS | para capturar esse ganho, precisa servidor manual ou controle total de write |
| Parser HTTP | manual, `Content-Length` direto | manual, pipeline + `writev` | uWS parser | overhead fixo do framework ainda existe |
| Parser JSON | seletivo/manual | seletivo/manual | `simdjson` seletivo via `parse_payload` | parser não é o gargalo principal nas nossas medições, mas manual ajuda quando servidor inteiro é manual |
| Índice | IVF 256, `nprobe=1`, bbox repair | IVF 4096, `FAST=8`, `FULL=24`, retry 2/3 | IVF 1280, `nprobe=1`, bbox repair seletivo 1..4 | geometria não transfere diretamente; nossas varreduras preservam 1280 como melhor ponto |
| SIMD | AVX2/FMA manual | AVX2/FMA manual | AVX2/FMA manual | já estamos na mesma família de instruções |
| Recursos | API `0.40/0.40`, LB `0.20` | API `0.40/0.40`, LB `0.20` | API `0.41/0.41`, LB `0.18` | splits líderes foram testados e não melhoraram nossa stack |
| `seccomp`/`ulimits` | usa | usa | não usa | já testado; sem ganho sustentável, exceto requerido para LB do Jairo |

Insights acionáveis:

| Prioridade | Ideia | Tipo | Status |
|---:|---|---|---|
| 1 | Reabrir servidor manual, mas agora com respostas HTTP completas pré-montadas, pipeline e `writev` como Jairo | estrutural | candidato futuro |
| 2 | Implementar parser JSON manual só se acoplado ao servidor manual | estrutural | candidato futuro |
| 3 | Testar arredondamento da query em 4 casas antes da seleção de centróide | pequeno | candidato barato |
| 4 | Tentar índice IVF 4096/FAST8/FULL24 no nosso kernel | médio/caro | baixa chance, pois 2048 já falhou/piorou antes |
| 5 | Trocar LB por HAProxy ou `so-no-forevis` | drop-in | rejeitado |
| 6 | `seccomp`, `ulimits`, `somaxconn`, splits CPU líderes | drop-in | rejeitado |

Leitura final desta comparação: não há knob simples restante copiado dos líderes que melhore nossa stack atual. O caminho para salto material é aproximar o hot path deles: servidor manual/io_uring ou pelo menos epoll com HTTP completo pré-montado, batch/pipeline e escrita vetorizada. Essa é uma mudança maior que precisa de rodada própria e benchmarkado contra a branch `submission` atual.

## Ciclo 00h35: arredondamento da query em 4 casas

Hipótese: o segundo colocado arredonda dimensões contínuas em 4 casas antes da busca. Como nosso índice usa quantização `×10000`, arredondar a query antes da seleção de centróide poderia alinhar melhor a geometria da consulta.

Alteração experimental temporária:

- Adicionar `round4(x) = round(x * 10000) * 0.0001`.
- Aplicar nos campos contínuos normalizados da vetorização.

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Estado atual, referência ruidosa da janela | 45754.3 | 0 | 0 | 0 | base |
| `round4` na vetorização | 53523.5 | 0 | 0 | 0 | rejeitar |

Leitura: a precisão foi preservada, mas o custo de `round` no hot path piorou a medição. Na implementação Rust do Jairo esse arredondamento está acoplado a um parser/vetorizador manual; na nossa stack ele adiciona custo sem retorno.

Decisão: rejeitado e revertido. Manter vetorização sem `round4` explícito.

## Ciclo 00h45: prefetch no scanner AVX2

Hipótese: o scanner do segundo colocado faz prefetch de blocos futuros no loop AVX2. Adicionar `_mm_prefetch` em `scan_blocks_avx2` para `block + 8` poderia reduzir miss de cache durante o repair.

Alteração experimental temporária:

```cpp
const std::uint32_t prefetch_block = block + 8U;
if (prefetch_block < end_block) {
    const std::size_t prefetch_base = prefetch_block * kDimensions * kBlockLanes;
    _mm_prefetch(reinterpret_cast<const char*>(blocks_ptr + prefetch_base), _MM_HINT_T0);
    _mm_prefetch(reinterpret_cast<const char*>(blocks_ptr + prefetch_base + (7U * kBlockLanes)), _MM_HINT_T0);
}
```

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Referência histórica do índice atual | ~18k | 0 | 0 | 0 | base histórica |
| Prefetch, run 1 | 40609.2 | 0 | 0 | 0 | rejeitar |
| Prefetch, run 2 | 35271.0 | 0 | 0 | 0 | rejeitar |

Leitura: preservou precisão, mas não deu sinal positivo. O prefetch explícito parece redundante ou contraproducente no nosso layout/kernel; no código do Jairo ele opera sobre outra forma de blocos e outro runtime.

Decisão: rejeitado e revertido. Não levar para k6.

## Ciclo 00h55: controle k6 após leitura dos líderes

Objetivo: confirmar a configuração restaurada depois dos testes derivados dos dois primeiros colocados.

Resultado:

| Estado | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Configuração restaurada pós-experimentos | 2.10ms | 0 | 0 | 0 | 5678.32 |

Leitura: a branch estava limpa e restaurada, então esse resultado aponta para degradação/ruído do host local nesta janela, não para uma mudança aceita. Como está abaixo do oficial #1314 e abaixo dos melhores controles locais, não há base para nova submissão.

Decisão: não submeter. Manter a melhor submissão oficial atual (#1314, commit `4d5bedb`, `final_score 5844.41`).

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

## Ciclo 23h10: flags Haswell inspiradas nos líderes

Hipótese: os dois primeiros colocados usam alvo Haswell explicitamente. O primeiro colocado em C compila com `-O3 -march=haswell -mtune=haswell -flto -fomit-frame-pointer -DNDEBUG`; o segundo, em Rust, usa `target-cpu=haswell` e `target-feature=+avx2,+fma,+f16c,+bmi2,+popcnt`. Como a CPU oficial descrita pelos participantes líderes é Haswell e o projeto já assumiu AVX2/FMA como requisito efetivo, valia medir se especializar o binário C++ geraria ganho no kernel IVF.

Alteração experimental limitada ao alvo `benchmark-ivf-cpp`, sem promover para o serviço:

```cmake
# variante 1
-mavx2 -mfma -march=x86-64-v3 -mtune=haswell -fomit-frame-pointer

# variante 2
-march=haswell -mtune=haswell -fomit-frame-pointer
```

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| baseline restaurado `x86-64-v3` | 39499.6 | 0 | 0 | 0 | manter |
| `x86-64-v3 + mtune=haswell + omit-frame-pointer` | 39819.2 | 0 | 0 | 0 | rejeitar |
| `march=haswell + mtune=haswell + omit-frame-pointer` | 39388.1 | 0 | 0 | 0 | rejeitar por ruído |

Leitura: a variante Haswell completa ficou numericamente 0,28% melhor que o baseline restaurado no mesmo ambiente aquecido, mas isso está abaixo do ruído observado hoje nos benchmarks offline e não justificaria trocar a baseline de compilação nem reduzir portabilidade. A variante menos invasiva foi pior.

Decisão: rejeitado e revertido. Manter `-mavx2 -mfma -march=x86-64-v3` no CMake.

## Ciclo 23h15: reamostragem de `seccomp=unconfined`

Hipótese: `seccomp=unconfined` apareceu nos dois repositórios líderes e já tinha dado um sinal pequeno em uma janela anterior. Como não foi promovido para `submission`, valia revalidar contra o estado aceito atual antes de descartar definitivamente.

Alteração experimental preparada em `docker-compose.yml`:

```yaml
security_opt:
  - seccomp=unconfined
```

Aplicação planejada em `api1`/`api2` e nginx.

Resultado operacional:

```text
docker compose config >/tmp/rinha-compose-seccomp.yml
docker compose up -d --force-recreate --remove-orphans
Error response from daemon: ports are not available: exposing port TCP 0.0.0.0:9999 ... bind: address already in use
```

Investigação:

```text
ss -ltnp 'sport = :9999'
LISTEN 0 4096 0.0.0.0:9999

ps -ef | rg 'docker-proxy|nginx'
root ... nginx: master process nginx -g daemon off;
root ... /usr/bin/docker-proxy -proto tcp -host-ip 0.0.0.0 -host-port 9999 ...
```

Leitura: a porta 9999 está presa por um `docker-proxy`/nginx órfão root de execução anterior, fora dos containers visíveis em `docker ps`. Como não tenho permissão para matar esses PIDs root sem reiniciar o Docker globalmente, e reiniciar Docker no meio do ciclo poderia ser mais disruptivo que o ganho esperado, o teste foi abortado sem resultado de performance.

Decisão: inconclusivo e revertido. O compose voltou ao estado limpo, e os containers parciais desta branch foram removidos com `docker compose down --remove-orphans`.

## Ciclo 23h35: parser sem cópia com `simdjson::pad_with_reserve`

Hipótese: os líderes evitam parser JSON genérico no hot path. Antes de reescrever parser seletivo, foi testada uma melhoria menor e sustentável: evitar a cópia para `simdjson::padded_string` quando o body já está em `std::string`, usando `simdjson::pad_with_reserve(body)`. Para compensar a necessidade de padding/capacity, também foi medida a reserva prévia de `768` bytes no body do handler.

Alterações experimentais:

- Overload `parse_payload(std::string& body, ...)` usando `simdjson::padded_string_view`.
- `body.reserve(768)` no handler `POST /fraud-score`.
- Métricas temporárias em `benchmark-request-cpp`.

Resultados offline em 200 amostras, 20 repetições:

| Métrica | ns/query | Leitura |
|---|---:|---|
| `parse_payload` baseline | 617.609 | caminho atual com `padded_string` |
| `parse_payload_mutable` sem reserva prévia | 635.840 | piora por realocação/padding |
| `body_reserve768_parse_payload_mutable` | 617.062 | diferença de 0,09%, ruído |
| `body_append_default` | 29.9405 | append atual isolado |
| `body_append_reserve768` | 14.4622 | reserva melhora append, mas o ganho absoluto é ~15 ns |

Validação:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp benchmark-request-cpp -j4
Resultado: passou.

./cpp/build/rinha-backend-2026-cpp-tests
Resultado: processo morto com exit 137 em ambiente local já degradado/pressionado; não foi usado como evidência de correção.
```

Leitura: o ganho potencial no parser é muito pequeno e não sobrevive quando medido como fluxo completo `append + parse`. Pior, o overload mutable pode piorar se algum caminho chamar sem reserva suficiente. Como o melhor caso economiza nanos e não há k6 disponível por bloqueio da porta 9999, não é sustentável promover.

Decisão: rejeitado e revertido. O código voltou ao parser atual.

## Ciclo 23h55: A/B de `seccomp=unconfined` em porta local alternativa

Hipótese: como a porta oficial local `9999` ficou presa por processo root órfão, ainda era possível fazer A/B em porta alternativa apenas para laboratório. O compose foi temporariamente alterado para publicar `9998:9999`, e o script k6 foi executado com URL local substituída para `localhost:9998`. Esta alteração nunca seria submissão; serviu apenas para comparação na mesma janela.

Sequência:

1. Baseline atual em `9998`.
2. Mesma configuração com `security_opt: seccomp=unconfined` em APIs e nginx.
3. A/B reverso removendo `seccomp=unconfined`.

Resultado:

| Variante local 9998 | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Baseline inicial | 2.76ms | 0 | 0 | 0 | 5559.50 |
| `seccomp=unconfined` | 2.65ms | 0 | 0 | 0 | 5575.94 |
| Baseline reverso | 2.66ms | 0 | 0 | 0 | 5575.01 |

Leitura: o baseline reverso ficou praticamente igual ao estado com `seccomp`, então o ganho aparente inicial era ruído/aquecimento da janela. Além disso, todos os resultados em `9998` ficaram muito abaixo do melhor local histórico e do resultado oficial #1314, confirmando degradação do host durante a noite.

Decisão: rejeitado. Não promover `seccomp=unconfined`. O compose foi restaurado para a porta oficial `9999`, e os containers de laboratório foram removidos com `docker compose down --remove-orphans`.

## Ciclo 00h15: reorder de dimensões no corte parcial AVX2 do IVF

Hipótese: o scanner AVX2 atual acumula as dimensões `0..7`, faz um corte parcial contra o pior top-5 e só então acumula `8..13`. A solução C líder e nosso escalar usam uma ordem mais discriminativa (`5,6,2,0,7,8,11,12,9,10,1,13,3,4`). Trocar apenas a ordem da primeira passada AVX2 poderia antecipar pruning sem alterar a distância final.

Alteração experimental em `scan_blocks_avx2`:

```cpp
constexpr std::array<std::size_t, 8> kFirstPassDims{5, 6, 2, 0, 7, 8, 11, 12};
constexpr std::array<std::size_t, 6> kSecondPassDims{9, 10, 1, 13, 3, 4};
```

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Reorder AVX2, run 1 | 18176.9 | 0 | 0 | 0 | rejeitar |
| Reorder AVX2, run 2 | 20630.6 | 0 | 0 | 0 | rejeitar |
| Baseline restaurado, mesma janela | 17730.5 | 0 | 0 | 0 | manter |

Leitura: a primeira execução parecia promissora quando comparada com runs degradadas anteriores, mas o A/B real na mesma janela mostrou que o baseline atual é mais rápido. A ordem original `0..7` no corte parcial AVX2 deve permanecer.

Decisão: rejeitado e revertido.

## Ciclo 01h00: checagem web de knobs restantes

Fontes consultadas:

- `https://simdjson.github.io/simdjson/md_doc_2dom.html`
- `https://simdjson.org/api/0.9.0/md_doc_performance.html`
- `https://www.nginx.com/wp-content/uploads/2018/08/nginx-modules-reference-r17.pdf`

Achados:

- A documentação do simdjson confirma que `padded_string_view`/padding sobre buffer próprio pode evitar cópias, mas exige responsabilidade sobre capacidade/padding. Isso bate com o experimento local: a técnica é válida em tese, porém no nosso fluxo `append + parse + DOM + extração` ficou indistinguível do baseline.
- As notas de performance do simdjson recomendam evitar criar muitos `std::string`/`padded_string` e reutilizar buffers. A nossa tentativa de reservar body e usar `pad_with_reserve` foi justamente essa linha, mas o ganho medido foi ~0,09% no melhor caso.
- A referência de nginx indica que, com `reuseport`, não há necessidade de habilitar `accept_mutex`; além disso, já estamos com `listen 9999 reuseport backlog=4096`, `multi_accept off` e `worker_processes 2`, que foram os knobs que sobreviveram ao A/B local.

Decisão: nenhuma nova mudança derivada dessas fontes. Elas reforçam as rejeições já medidas: parser padding não compensa, e ajustes de accept mutex/multi_accept/worker count fora do estado atual têm baixa chance sem novo desenho estrutural.

## Ciclo 01h15: diagnóstico de dois Docker daemons

Problema observado: a porta `9999` permanecia ocupada mesmo após `docker compose down` no contexto padrão.

Investigação:

```text
docker context show
desktop-linux

ss -ltnp 'sport = :9999'
LISTEN 0 4096 0.0.0.0:9999

ps -fp 1808,168120,168144,168173,168181
/usr/bin/dockerd -H fd:// --containerd=/run/containerd/containerd.sock
/usr/bin/containerd-shim-runc-v2 ... -id 352592...
nginx: master process nginx -g daemon off;
/usr/bin/docker-proxy ... -host-port 9999 ...

DOCKER_HOST=unix:///run/docker.sock docker ps -a
352592... perf-noon-tuning-nginx-1 Up ... 0.0.0.0:9999->9999/tcp
```

Leitura: havia dois daemons em uso:

- Docker Desktop (`desktop-linux`), usado pelo `docker` padrão da sessão.
- Docker Engine do sistema (`/run/docker.sock`), que ainda mantinha containers antigos `perf-noon-tuning-*` e prendia a porta `9999`.

Ação:

```text
DOCKER_HOST=unix:///run/docker.sock docker compose down --remove-orphans
```

Resultado: porta `9999` liberada.

Revalidação de benchmark:

| Ambiente | p99 | FP | FN | HTTP errors | final_score | Leitura |
|---|---:|---:|---:|---:|---:|---|
| Docker Desktop `desktop-linux` | 50.12ms | 0 | 0 | 0 | 4299.99 | inválido/degradado |
| Docker Engine do sistema `/run/docker.sock` | 1.64ms | 0 | 0 | 0 | 5785.30 | ambiente local correto para comparar |

Decisão operacional: para experimentos k6 locais nesta máquina, usar explicitamente `DOCKER_HOST=unix:///run/docker.sock`. O Docker Desktop está ativo, mas seus resultados nesta janela não são comparáveis com a submissão/benchmarks históricos.

## Ciclo 00h45: especialização `nprobe=1` no loop de centróides

Hipótese: no caminho dominante (`fast_nprobe=1` e `full_nprobe=1`), não é necessário manter um top-N genérico de centróides; basta guardar o único melhor cluster. Isso removeria `insert_probe()` e arrays de distância no caso `MaxNprobe == 1`.

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Especialização `nprobe=1`, run 1 | 23111.0 | 0 | 0 | 0 | rejeitar |
| Especialização `nprobe=1`, run 2 | 17056.8 | 0 | 0 | 0 | inconclusivo |
| Especialização `nprobe=1`, run 3 | 17665.1 | 0 | 0 | 0 | inconclusivo |
| Baseline restaurado, run 1 | 16856.8 | 0 | 0 | 0 | manter |
| Baseline restaurado, run 2 | 17605.2 | 0 | 0 | 0 | manter |

Leitura: o melhor resultado da variante foi ruído; o baseline restaurado alcançou resultado igual ou melhor na mesma janela. O compilador já otimiza bem o caminho genérico para `MaxNprobe=1`.

Decisão: rejeitado e revertido.

## Ciclo 00h30: corte parcial AVX2 após 4 dimensões

Hipótese: além do corte parcial já existente após 8 dimensões, um corte antecipado após 4 dimensões poderia descartar blocos ruins mais cedo e economizar as dimensões `4..13`. A mudança é exata: só pula bloco quando todas as lanes já excedem o pior top-5 parcial.

Resultado offline:

| Variante | ns/query | FP | FN | parse_errors | Decisão |
|---|---:|---:|---:|---:|---|
| Baseline da janela anterior | 17730.5 | 0 | 0 | 0 | manter |
| Corte extra após 4 dims, run 1 | 18344.4 | 0 | 0 | 0 | rejeitar |
| Corte extra após 4 dims, run 2 | 20710.2 | 0 | 0 | 0 | rejeitar |

Leitura: a comparação extra e a pressão adicional no loop AVX2 custam mais do que economizam. O corte atual após 8 dimensões segue melhor.

Decisão: rejeitado e revertido.

## Ciclo 23h20: margem de FD/backlog no nginx

Hipótese: inspirada por configurações de repositórios líderes, aumentar margem de file descriptors e declarar `somaxconn` no container do nginx poderia ajudar a borda em rajadas oficiais.

Alteração experimental:

```nginx
worker_rlimit_nofile 65535;
```

```yaml
nginx:
  ulimits:
    nofile:
      soft: 65535
      hard: 65535
  sysctls:
    net.core.somaxconn: 4096
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Estado publicado, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| Estado publicado, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| FD/somaxconn no nginx | 1.24ms | 0 | 0 | 0 | 5908.28 |

Leitura: não houve ganho mensurável. A configuração aumenta superfície operacional e já havia sinais anteriores de que `ulimits` não traziam benefício consistente nesta stack.

Decisão: rejeitado e revertido. Manter compose simples sem `ulimits`/`sysctls`.

## Ciclo 23h30: nginx `worker_processes auto`

Hipótese: o runner oficial poderia ter topologia de CPU diferente; deixar nginx escolher `worker_processes auto` testaria se mais workers ajudam em ambiente multi-core.

Alteração experimental:

```nginx
worker_processes auto;
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `worker_processes 2`, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `worker_processes 2`, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `worker_processes auto` | 1.27ms | 0 | 0 | 0 | 5894.85 |

Leitura: mais workers automáticos pioram sob a cota de `0.18` CPU do nginx. O ponto ótimo local medido segue sendo 2 workers fixos.

Decisão: rejeitado e revertido. Manter `worker_processes 2`.

## Ciclo 23h10: split CPU intermediário pró-API (`0.415/0.415/0.17`)

Hipótese: como `0.42/0.42/0.16` ficou competitivo, mas não superior, um meio-termo dando um pouco mais de CPU às APIs sem reduzir tanto o nginx poderia melhorar a cauda.

Alteração experimental:

```yaml
api1/api2: cpus "0.415"
nginx:     cpus "0.17"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `0.41/0.41/0.18`, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `0.42/0.42/0.16` | 1.22ms | 0 | 0 | 0 | 5913.50 |
| `0.415/0.415/0.17` | 1.24ms | 0 | 0 | 0 | 5906.26 |

Leitura: o ajuste fino pró-API não melhorou. A família de splits em torno de `0.41/0.41/0.18` já foi suficientemente varrida nesta janela: `0.40/0.20`, `0.405/0.19`, `0.415/0.17`, `0.42/0.16` e o atual.

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

## Ciclo 23h00: split CPU intermediário (`0.405/0.405/0.19`)

Hipótese: como `0.42/0.42/0.16` e `0.40/0.40/0.20` foram piores, um ponto intermediário poderia dar um pouco mais de CPU ao nginx sem penalizar tanto as APIs.

Alteração experimental:

```yaml
api1/api2: cpus "0.405"
nginx:     cpus "0.19"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `0.41/0.41/0.18`, melhor run local | 1.18ms | 0 | 0 | 0 | 5927.14 |
| `0.41/0.41/0.18`, oficial #1314 | 1.43ms | 0 | 0 | 0 | 5844.41 |
| `0.405/0.405/0.19` | 1.24ms | 0 | 0 | 0 | 5906.48 |

Leitura: o split intermediário também piorou. A cauda parece mais sensível à perda de CPU das APIs do que a qualquer ganho marginal no nginx.

Decisão: rejeitado e revertido. Manter `0.41/0.41/0.18`.

## Ciclo 23h30: controle no Docker Engine do sistema + `reuseport`

Hipótese: depois de separar Docker Desktop de Docker Engine do sistema, era necessário recalibrar a janela local antes de novos testes de LB. Como o estado aceito usa `listen 9999 reuseport backlog=4096`, foi testada uma remoção simples do `reuseport` para validar se ele ainda contribuía positivamente no daemon correto.

Ambiente usado:

```bash
DOCKER_HOST=unix:///run/docker.sock
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Controle aceito com `reuseport` | 1.69ms | 0 | 0 | 0 | 5771.69 |
| `listen 9999 backlog=4096` sem `reuseport` | 1.61ms | 0 | 0 | 0 | 5792.17 |
| Controle reverso com `reuseport` restaurado | 1.60ms | 0 | 0 | 0 | 5794.60 |

Leitura: a primeira medição controle estava fria/pior na janela. Quando o controle foi repetido após a variante, `reuseport` empatou ou superou levemente o teste sem `reuseport`. Portanto, não há evidência de ganho sustentável ao remover `reuseport`.

Decisão: rejeitado. Manter `listen 9999 reuseport backlog=4096`.

## Ciclo 00h00: reserva do buffer HTTP no hot path

Hipótese: cada `POST /fraud-score` acumula o corpo em um `std::string` antes do parse. Como os payloads são pequenos e previsíveis, reservar capacidade inicial poderia evitar realocação no `append` e reduzir levemente o p99.

Alteração experimental:

```cpp
res->onData([res, state, body = [] {
    std::string value;
    value.reserve(768);
    return value;
}()](std::string_view chunk, bool is_last) mutable {
```

Observação operacional: a primeira tentativa de build usou o contexto Docker padrão (`desktop-linux`) por engano. Esse resultado foi descartado porque o Compose de benchmark estava preso ao Docker Engine do sistema (`DOCKER_HOST=unix:///run/docker.sock`). A medição válida abaixo foi feita após reconstruir a imagem explicitamente no daemon correto.

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Controle anterior com `reuseport` | 1.60ms | 0 | 0 | 0 | 5794.60 |
| Buffer com `reserve(768)` | 1.59ms | 0 | 0 | 0 | 5798.46 |
| Controle reverso sem `reserve(768)` | 1.59ms | 0 | 0 | 0 | 5799.46 |

Leitura: a variante empatou com o controle reverso e ficou nominalmente 1 ponto abaixo no score. A realocação do buffer de corpo não aparece como gargalo mensurável no k6 local.

Decisão: rejeitado e revertido. Manter o hot path mais simples com `body = std::string{}`.

## Ciclo 00h25: `MALLOC_ARENA_MAX=1` nas APIs

Hipótese: reduzir o número de arenas da glibc nas duas APIs poderia diminuir fragmentação/memória e estabilizar cauda, sem rebuild de imagem.

Alteração experimental:

```yaml
environment:
  MALLOC_ARENA_MAX: "1"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Controle recente sem `MALLOC_ARENA_MAX` | 1.59ms | 0 | 0 | 0 | 5799.46 |
| APIs com `MALLOC_ARENA_MAX=1` | 1.76ms | 0 | 0 | 0 | 5755.72 |
| Controle pós-reversão | 1.65ms | 0 | 0 | 0 | 5781.42 |

Leitura: limitar a arena piorou a cauda de forma clara. Para essa aplicação, a hipótese mais provável é aumento de contenção no allocator sem benefício prático, já que o hot path foi desenhado para alocar pouco.

Decisão: rejeitado e revertido. Não usar `MALLOC_ARENA_MAX`.

## Ciclo 00h45: nginx stream `tcp_nodelay on`

Hipótese: como as respostas são muito pequenas, habilitar `tcp_nodelay` no servidor `stream` do nginx poderia reduzir atraso de pacotes pequenos no trecho cliente -> LB. O trecho LB -> API continua via unix socket.

Alteração experimental:

```nginx
server {
    listen 9999 reuseport backlog=4096;
    tcp_nodelay on;
    proxy_pass api;
}
```

Validação de configuração:

```bash
nginx -t
# syntax is ok
# test is successful
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Controle sem `tcp_nodelay`, janela recente | 1.65ms | 0 | 0 | 0 | 5781.42 |
| `tcp_nodelay on`, primeira amostra | 1.62ms | 0 | 0 | 0 | 5791.68 |
| Controle reverso sem `tcp_nodelay` | 1.66ms | 0 | 0 | 0 | 5779.49 |
| `tcp_nodelay on`, segunda amostra | 1.63ms | 0 | 0 | 0 | 5788.18 |

Leitura: houve leve vantagem nominal contra os controles imediatamente adjacentes, mas o resultado continuou dentro do ruído local da noite e pior que os melhores controles recentes (`1.59ms`). Como o ganho não superou o critério de sustentabilidade/inquestionabilidade, não vale promover para `submission`.

Decisão: rejeitado e revertido. Manter nginx sem `tcp_nodelay` explícito.

## Ciclo 01h10: backlog do unix socket das APIs no uSockets

Hipótese: o nginx externo já escuta com `backlog=4096`, mas o uSockets usado pelo uWebSockets fixa `listen(..., 512)` para unix domain sockets. Em bursts, a fila menor no trecho nginx -> API poderia contribuir para cauda. Aumentar o backlog do UDS para `4096` alinha API e LB sem mudar contrato, topologia ou algoritmo.

Achado no código:

```c
// cpp/third_party/uWebSockets/uSockets/src/bsd.c
listen(listenFd, 512)
```

Alteração experimental:

```c
listen(listenFd, 4096)
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| UDS backlog `4096`, primeira amostra | 1.58ms | 0 | 0 | 0 | 5800.61 |
| Controle reverso UDS backlog `512` | 1.62ms | 0 | 0 | 0 | 5789.78 |
| UDS backlog `4096`, segunda amostra | 1.57ms | 0 | 0 | 0 | 5803.32 |

Leitura: é o melhor sinal novo da noite até aqui. A melhora ainda é pequena e não supera a submissão oficial #1314 (`p99 1.43ms`, `final_score 5844.41`), mas foi repetida em A/B/A na mesma janela e não introduz risco funcional evidente.

Decisão inicial: manter como candidato na branch exploratória `perf/noon-tuning` para reamostragem. Não promover ainda para `submission` sem validação adicional, porque o ganho local é menor que a variância histórica da máquina.

## Ciclo 01h25: upper-bound do backlog UDS (`8192`)

Hipótese: se `4096` ajudou por reduzir fila curta no UDS, talvez `8192` pudesse absorver bursts ainda melhor.

Alteração experimental:

```c
listen(listenFd, 8192)
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| UDS backlog `4096`, melhor amostra da janela | 1.57ms | 0 | 0 | 0 | 5803.32 |
| UDS backlog `8192` | 1.66ms | 0 | 0 | 0 | 5780.24 |

Leitura: aumentar além de `4096` piorou a cauda. A interpretação mais provável é que `4096` alinha o gargalo com o nginx sem criar fila exagerada; `8192` não reduz trabalho, apenas permite mais acúmulo sob pico.

Decisão inicial: rejeitado e revertido para `4096`.

## Ciclo 01h35: revalidação do backlog UDS `4096`

Após o teste de `8192`, foi necessário reconstruir novamente a imagem com `4096`, porque a imagem local ainda carregava o binário experimental anterior. A reamostragem do suposto candidato `4096` não sustentou o ganho inicial.

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| UDS backlog `4096`, melhor amostra anterior | 1.57ms | 0 | 0 | 0 | 5803.32 |
| UDS backlog `4096`, revalidação pós-`8192` | 1.66ms | 0 | 0 | 0 | 5780.15 |

Leitura: o sinal positivo A/B/A inicial não reproduziu depois da reconstrução/recriação seguinte. Isso coloca o resultado na mesma categoria dos demais ganhos marginais da noite: interessante como investigação, insuficiente como melhoria sustentável.

Decisão final: rejeitado e revertido para o backlog original `512`. Não promover alteração vendored do uSockets.

Fechamento operacional: depois da reversão, a imagem local foi reconstruída explicitamente no Docker Engine do sistema e a pilha foi recriada. O benchmark de limpeza do estado restaurado marcou `p99 1.65ms`, `final_score 5783.32`, 0 FP/FN/HTTP errors. Esse resultado confirma que o ambiente voltou ao comportamento estável da janela, embora ainda abaixo da submissão oficial #1314.

## Ciclo 01h50: remover `res->onAborted`

Hipótese: o handler de `POST /fraud-score` registrava um callback vazio de abort para cada requisição. Como o processamento é síncrono dentro de `onData`, remover essa linha poderia reduzir overhead por request.

Alteração experimental:

```cpp
- res->onAborted([]() {});
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Sem `res->onAborted` | 1.18ms | 0 | 0 | 54057 | -71.14 |

Log relevante:

```text
Error: Returning from a request handler without responding or attaching an abort handler is forbidden!
terminate called without an active exception
nginx: no live upstreams while connecting to upstream
```

Leitura: no uWebSockets, quando o handler retorna sem responder imediatamente e delega a resposta para `onData`, é obrigatório anexar abort handler. Sem isso, as APIs abortaram e o nginx ficou sem upstream vivo. O p99 baixo é irrelevante porque a taxa de erro foi 100%.

Decisão: rejeitado e revertido. Manter `res->onAborted([]() {});`.

Fechamento operacional: após a reversão, a imagem estável foi reconstruída e a pilha recriada no Docker Engine do sistema. O benchmark de limpeza marcou `p99 1.59ms`, `final_score 5799.17`, 0 FP/FN/HTTP errors. O ambiente voltou a responder normalmente.

## Ciclo 02h10: evitar cópia de `shared_ptr` no `onData`

Hipótese: o callback `onData` capturava `std::shared_ptr<AppState>` por request, potencialmente pagando incremento/decremento atômico no hot path. Como o handler externo mantém o `shared_ptr` vivo, o `onData` poderia capturar apenas `const AppState*`.

Alteração experimental:

```cpp
const AppState* app_state = state.get();
res->onData([res, app_state, body = std::string{}](...) { ... });
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Controle estável prévio | 1.59ms | 0 | 0 | 0 | 5799.17 |
| `onData` capturando `const AppState*` | 1.59ms | 0 | 0 | 0 | 5798.06 |

Leitura: a alteração é funcionalmente segura na estrutura atual, mas não apresentou ganho mensurável. O custo do `shared_ptr` não aparece no p99 local ou é pequeno demais para separar do ruído.

Decisão: rejeitado e revertido por KISS. Manter captura de `shared_ptr` como estava.

## Ciclo 02h35: compilar `usockets` com `-march=x86-64-v3`

Hipótese: o alvo principal já usa `-mavx2 -mfma -march=x86-64-v3`, mas a biblioteca vendored `usockets` era compilada apenas com flags Release padrão. Como o caminho de accept/read/write passa por ela, compilar `usockets` para o mesmo baseline de CPU poderia reduzir overhead do servidor.

Alteração experimental:

```cmake
target_compile_options(usockets PRIVATE -march=x86-64-v3)
```

Validação: build completo com `--no-cache` para garantir recompilação real de `usockets`.

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Controle estável recente | 1.59ms | 0 | 0 | 0 | 5799.17 |
| `usockets` com `-march=x86-64-v3` | 1.64ms | 0 | 0 | 0 | 5786.48 |

Leitura: a flag não ajudou. É possível que o gargalo em `usockets` seja syscall/event-loop e não código gerado, ou que o binário maior/mais específico piore cache/branching nessa janela.

Decisão: rejeitado e revertido.

Fechamento operacional: a imagem estável foi reconstruída após a reversão da flag e a pilha foi recriada. O benchmark de limpeza marcou `p99 1.59ms`, `final_score 5798.27`, 0 FP/FN/HTTP errors. Estado local novamente coerente com a configuração publicada.

## Ciclo 02h50: compilar `simdjson_singleheader` com `-march=x86-64-v3`

Hipótese: parse JSON é parte do hot path. Compilar a tradução única do simdjson com o mesmo baseline de CPU do binário principal poderia reduzir custo de parse.

Alteração experimental:

```cmake
target_compile_options(simdjson_singleheader PRIVATE -march=x86-64-v3)
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| Controle estável recente | 1.59ms | 0 | 0 | 0 | 5798.27 |
| `simdjson_singleheader` com `-march=x86-64-v3` | 1.67ms | 0 | 0 | 0 | 5776.37 |

Leitura: piorou. O simdjson já possui dispatch/implementações próprias para SIMD, e forçar a flag no TU da biblioteca não trouxe benefício para o payload do desafio.

Decisão: rejeitado e revertido.

Fechamento operacional: imagem estável reconstruída após a reversão e pilha recriada. Benchmark de limpeza: `p99 1.66ms`, `final_score 5780.59`, 0 FP/FN/HTTP errors. O estado está funcional, mas a janela local segue mais lenta que o melhor histórico/oficial.

## Ciclo 03h05: revalidação de `IVF_FULL_NPROBE=2`

Hipótese: em uma rodada anterior, `IVF_FULL_NPROBE=2` marcou `p99 1.37ms`, 0 erros e `final_score 5863.46`. Embora tenha sido rejeitado por perder para o melhor local histórico (`1.18ms`), esse número seria melhor que a submissão oficial #1314 (`1.43ms`, `5844.41`). Por isso, valia revalidar no Docker Engine correto antes de descartar definitivamente.

Alteração experimental:

```yaml
IVF_FAST_NPROBE: "1"
IVF_FULL_NPROBE: "2"
```

Resultado:

| Variante | p99 | FP | FN | HTTP errors | final_score |
|---|---:|---:|---:|---:|---:|
| `IVF_FULL_NPROBE=2`, melhor run antiga | 1.37ms | 0 | 0 | 0 | 5863.46 |
| `IVF_FULL_NPROBE=2`, revalidação atual | 1.60ms | 0 | 0 | 0 | 5795.49 |

Leitura: a run antiga não reproduziu. A configuração preserva precisão, mas não entrega ganho sustentável de p99 nesta janela. O resultado atual fica abaixo da submissão oficial e abaixo do estado estável recente.

Decisão: rejeitado e revertido para `IVF_FULL_NPROBE=1`.
