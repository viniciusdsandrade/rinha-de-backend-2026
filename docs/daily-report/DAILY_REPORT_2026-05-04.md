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
