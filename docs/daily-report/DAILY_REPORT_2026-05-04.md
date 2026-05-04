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

Decisão: aceitar como candidato branch-local. Manter no `perf/noon-tuning` para mais reamostragem; não abrir submissão oficial só por esse ganho marginal sem novo bloco robusto.

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

Decisão: rejeitar escopo parcial e restaurar `seccomp=unconfined` em APIs e nginx no branch de tuning.
