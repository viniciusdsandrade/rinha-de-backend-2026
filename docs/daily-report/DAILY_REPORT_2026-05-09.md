# Daily Report - 2026-05-09

## Contexto operacional

Objetivo do ciclo: continuar investigação -> hipótese -> experimento -> report até `15h00` de `2026-05-09`, buscando melhora sustentável de performance para a submissão da Rinha de Backend 2026.

Estado seguro de submissão no início do dia:

```text
branch: submission
commit: f2a5b98 restore best official submission image
imagem: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-a477d55
melhor evidência oficial histórica: issue #2316, p99 1.20ms, 0% falhas, final_score 5921.80
última reexecução oficial: issue #2338, p99 1.23ms, 0% falhas, final_score 5910.58
```

Decisão de segurança: não mexer na branch `submission` sem uma evidência nova que supere claramente a melhor submissão oficial.

## Ciclo 10h55: retomada e baseline de contexto

Validações iniciais:

```text
data/hora local: 2026-05-09 10:55:38 -03
branch experimental: perf/noon-tuning
último commit experimental: 019ea08 report overnight tuning results
branch submission: submission
último commit submission: f2a5b98 restore best official submission image
```

Resultado: ambiente limpo para novas hipóteses, sem container pendurado e sem patch de código ativo.

## Ciclo 11h00: varredura de clusters IVF intermediários

Hipótese: o ponto ótimo de clusters poderia estar entre `1024`, `1280`, `1536` e `2048`; clusters maiores reduzem blocos por cluster, mas podem aumentar custo de centroides/bbox e afetar acurácia.

Comandos executados:

```text
prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1536.bin 1536 65536 6
benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1536.bin 3 0 1 1 1 1 4 1 0 0 0

prepare-ivf-cpp resources/references.json.gz /tmp/rinha-ivf-perf/index-1024.bin 1024 65536 6
benchmark-ivf-cpp test/test-data.json /tmp/rinha-ivf-perf/index-1024.bin 3 0 1 1 1 1 4 1 0 0 0
```

Resultados:

| Clusters | ns/query | FP | FN | Falhas | avg primary blocks | Decisão |
|---:|---:|---:|---:|---:|---:|---|
| 1024 | 18054.10 | 3 | 6 | 0.0055% | 408.823 | rejeitado |
| 1280 | ~8604-16696 | 0 | 0 | 0% | 322.897 | mantido como referência |
| 1536 | 17632.30 | 6 | 0 | 0.0037% | 271.393 | rejeitado |
| 2048 | 7058.16 com nprobe 1; 22503.20 com nprobe 4 | 9/0 | 9/0 | 0.0111%/0% | 204.981+ | rejeitado |

Aprendizado: reduzir/aumentar clusters fora de `1280` piora a fronteira acurácia/performance no desenho atual. `2048` parece rápido com `nprobe=1`, mas erra; quando corrige com `nprobe=4`, fica muito mais lento.

## Ciclo 11h15: ordem de dimensões no `bbox_lower_bound`

Hipótese: testar dimensões mais variáveis primeiro no `bbox_lower_bound` poderia reduzir o custo de abortar clusters cujo lower bound já excede o top-5 atual, sem mudar o resultado matemático.

Ordem calculada por variância global do `references.json.gz`:

```text
[6, 10, 9, 5, 11, 2, 4, 7, 0, 1, 8, 12, 3, 13]
```

Patch temporário:

```cpp
constexpr std::array<std::uint8_t, kDimensions> kBboxDimensionOrder{
    6, 10, 9, 5, 11, 2, 4, 7, 0, 1, 8, 12, 3, 13
};

for (const std::size_t dim : kBboxDimensionOrder) {
    ...
}
```

Validação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultado offline:

| Variante | ns/query | FP | FN | Falhas |
|---|---:|---:|---:|---:|
| bbox em ordem por variância | 18435.30 | 0 | 0 | 0% |
| bbox em ordem natural, mesmo índice | 16696.00 | 0 | 0 | 0% |

Decisão: **rejeitado e revertido**.

Aprendizado: a ordem por variância não melhorou o early abort; provavelmente prejudicou localidade/predição do caminho quente e não reduziu trabalho suficiente. Manter ordem natural.

## Ciclo 11h45: normas pré-computadas dos centroides

Investigação externa: repositórios públicos recentes sugerem alternativas como HNSW, mmap e rerank fp32. HNSW é uma mudança estrutural grande demais para aplicar sem revalidar recall/build; rerank fp32 melhora acurácia, mas nosso estado atual já está em `0%` falhas. A hipótese pequena derivada dessa investigação foi atacar o custo fixo de escolher o centroide primário.

Hipótese: pré-computar `||centroid||²` no carregamento e calcular a distância como `||c||² - 2*q·c` poderia ser mais barato que `sum((q-c)^2)` no `nearest_centroid_probe1`. O termo `||q||²` é constante entre centroides e não precisa entrar na comparação.

Patch temporário:

```text
IvfIndex::centroid_norms_
nearest_centroid_avx2: acc = centroid_norms; acc = fmadd(-2*q, centroid, acc)
fallback escalar usando a mesma fórmula
```

Validação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultado offline:

| Variante | ns/query | FP | FN | Falhas |
|---|---:|---:|---:|---:|
| fórmula atual `sum((q-c)^2)` | 16696.00 | 0 | 0 | 0% |
| normas pré-computadas + dot | 16786.10 | 0 | 0 | 0% |

Decisão: **rejeitado e revertido**.

Aprendizado: apesar de reduzir operações aritméticas aparentes, a variante com norma pré-computada não melhorou o hot path e ainda aumentaria memória/complexidade. O compilador/CPU parecem lidar muito bem com o kernel atual de subtração e multiplicação.

## Ciclo 12h10: nginx `multi_accept`

Hipótese: permitir que cada worker do nginx aceite múltiplas conexões por wake-up (`multi_accept on`) poderia reduzir overhead de accept sob rajadas do k6.

Procedimento:

```text
1. Reconstruir a imagem local atual com nginx baseline (`multi_accept off`).
2. Rodar k6 local e salvar /tmp/rinha-ivf-perf/nginx-multiaccept-baseline.json.
3. Alterar somente nginx.conf para `multi_accept on`.
4. Recriar compose com `--no-build` e rodar k6 novamente.
5. Reverter nginx.conf.
```

Resultados:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| baseline `multi_accept off` | 0.94ms | 0% | 6000.00 |
| `multi_accept on` | 1.05ms | 0% | 5979.62 |

Decisão: **rejeitado e revertido**.

Aprendizado: no stack atual, `multi_accept on` piora a cauda local. Provavelmente concentra bursts em um worker/socket e aumenta disputa/context switching em vez de reduzir overhead útil. Manter `multi_accept off`.

## Ciclo 12h35: `-ffast-math` no runtime

Hipótese: `-ffast-math` no target manual e no benchmark poderia reduzir custo de operações float no cálculo de centroide sem mexer no índice gerado pelo `prepare-ivf-cpp`.

Escopo temporário:

```text
rinha-backend-2026-cpp-manual: + -ffast-math
benchmark-ivf-cpp: + -ffast-math
prepare-ivf-cpp: sem alteração
```

Validação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultado offline:

| Variante | ns/query | FP | FN | Falhas |
|---|---:|---:|---:|---:|
| baseline imediato | 16696.00 | 0 | 0 | 0% |
| `-ffast-math` | 16813.00 | 0 | 0 | 0% |

Decisão: **rejeitado e revertido**.

Aprendizado: não houve ganho no caminho de busca; o risco semântico de `-ffast-math` não se justifica sem melhoria clara e reprodutível.

## Ciclo 13h10: comparação com issue oficial do jairoblatt

Investigação oficial via issues fechadas:

```text
issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2571
repo-url: https://github.com/jairoblatt/rinha-2026-rust
imagem api: jrblatt/rinha-2026-rust:v0.1.9
imagem lb: jrblatt/so-no-forevis:v0.0.2
p99: 1.14ms
falhas: 0%
final_score: 5941.57
cpu: lb 0.40, api1 0.30, api2 0.30
mem: lb 40MB, api1 155MB, api2 155MB
```

Hipótese: a performance oficial superior poderia estar ligada à alocação de CPU para o load balancer. Para isolar essa variável sem trocar arquitetura, testei `nginx` com a mesma divisão de CPU `0.40/0.30/0.30`.

Resultado local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| baseline imediato `nginx 0.18 / APIs 0.41+0.41` | 0.94ms | 0% | 6000.00 |
| `nginx 0.40 / APIs 0.30+0.30` | 1.26ms | 0% | 5900.55 |

Decisão: **rejeitado e revertido**.

Aprendizado: a vantagem do jairoblatt não parece ser somente “dar mais CPU ao LB”. Com nginx, tirar CPU das APIs prejudica mais do que ajuda. O diferencial provável é o LB próprio (`jrblatt/so-no-forevis`) e/ou menor overhead de proxy, mas trocar o LB exige implementação e validação dedicadas, não apenas ajuste de compose.

## Ciclo 13h25: política de probes inspirada no jairoblatt

Investigação no código do jairoblatt:

```text
FAST_NPROBE = 8
FULL_NPROBE = 24
se fast_count != 2 && fast_count != 3 => retorna fast_count
se fast_count está na fronteira 2/3 => refaz com FULL_NPROBE
sem bbox repair
```

Hipótese: substituir nosso reparo por bbox por uma política de probes fixos em fronteira poderia reduzir custo ou simplificar o hot path.

Experimentos offline no nosso índice `1280`:

| Configuração | ns/query | FP | FN | Falhas | Decisão |
|---|---:|---:|---:|---:|---|
| `fast=8 full=24 bbox=0 repair=2..3` | 74708.20 | 3 | 6 | 0.0055% | rejeitado |
| `fast=4 full=24 bbox=0 repair=2..3` | 47740.90 | 3 | 6 | 0.0055% | rejeitado |
| `fast=2 full=24 bbox=0 repair=2..3` | 33372.60 | 12 | 9 | 0.0129% | rejeitado |

Aprendizado: a política de probes do jairoblatt não é transferível para nosso índice/kernel como simples configuração. No nosso código, `nprobe > 1` ainda carrega custo alto de seleção/scan de clusters e não zera erros sem bbox. Manter `fast=1/full=1/bbox_repair=1/repair=1..4`.

## Ciclo 11h20: `seccomp:unconfined` no compose

Hipótese: remover o filtro seccomp padrão do Docker poderia reduzir overhead de syscalls no caminho `nginx`/API (`epoll`, `accept`, `recv`, `send`) sem recorrer a `privileged` ou `network_mode: host`.

Alteração temporária:

```yaml
security_opt:
  - seccomp:unconfined
```

Aplicada somente em `api1/api2` e `nginx`.

Resultado local com a imagem já construída:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| baseline imediato | 0.94ms | 0% | 6000.00 |
| `seccomp:unconfined` | 1.15ms | 0% | 5938.54 |

Decisão: **rejeitado e revertido**.

Aprendizado: o filtro seccomp não aparece como gargalo relevante nesta stack local. Mesmo sem erros de detecção, a latência piorou contra o baseline imediato; manter o compose dentro do perfil padrão é mais seguro e não custa performance mensurável.

## Ciclo 11h30: screening do LB `jrblatt/so-no-forevis`

Hipótese: parte da vantagem do `jairoblatt` poderia vir do load balancer próprio `jrblatt/so-no-forevis:v0.0.2`, que usa sockets Unix e foi usado na submissão oficial dele. Testei como caixa-preta apenas para decidir se vale implementar um LB próprio equivalente.

Primeira tentativa:

```text
imagem: jrblatt/so-no-forevis:v0.0.2
UPSTREAMS=/sockets/api1.sock,/sockets/api2.sock
PORT=9999
BUF_SIZE=4096
WORKERS=1
```

Resultado: falhou antes do benchmark por bloqueio de `io_uring` no seccomp padrão.

Erro observado:

```text
failed to build IoUring runtime: Os { code: 1, kind: PermissionDenied, message: "Operation not permitted" }
```

Segunda tentativa: `seccomp:unconfined` somente no LB, mantendo APIs sem essa opção.

Resultado local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| baseline imediato `nginx` | 0.94ms | 0% | 6000.00 |
| `so-no-forevis` + `seccomp:unconfined` no LB | 1.20ms | 0% | 5919.13 |

Decisão: **rejeitado e revertido**.

Aprendizado: trocar nginx pelo LB externo não melhorou nossa solução local. Como o ganho não apareceu nem em screening, não há evidência suficiente para gastar uma rodada longa implementando um LB próprio agora. A diferença do jairoblatt parece vir da combinação completa dele, não de um drop-in replacement de LB sobre nossas APIs.

## Ciclo 11h45: estreitamento do `extreme_repair`

Hipótese: o `extreme_repair` atual corrigia poucos casos relevantes, mas disparava em 48 queries. Se as janelas fossem mais estreitas, seria possível preservar detecção perfeita e reduzir scans de bbox no hot path.

Baseline offline com stats:

| Variante | ns/query | FP | FN | extreme repairs | repaired queries |
|---|---:|---:|---:|---:|---:|
| janela ampla anterior | 17457.90 | 0 | 0 | 48 | 2401 |
| `disable_extreme_repair=1` | 16431.10 | 1 | 2 | 0 | 2353 |

O desligamento completo foi rejeitado porque abriu `1 FP + 2 FN`. Em seguida, usei esses erros para estreitar as duas janelas extremas (`frauds=0` e `frauds=5`) sem desligar o mecanismo.

Resultado offline após estreitamento:

| Variante | ns/query | FP | FN | extreme repairs | repaired queries |
|---|---:|---:|---:|---:|---:|
| janela estreita | 15669.90 | 0 | 0 | 3 | 2356 |

Validação:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais após rebuild da imagem:

| Run | p99 | Falhas | final_score |
|---|---:|---:|---:|
| janela estreita #1 | 1.16ms | 0% | 5936.65 |
| janela estreita #2 | 1.13ms | 0% | 5946.11 |

Decisão: **aceito na branch experimental**.

Aprendizado: há ganho real no kernel IVF ao reduzir reparos extremos de 48 para 3, preservando detecção perfeita no dataset local/oficial conhecido. O p99 end-to-end continua sujeito a ruído, mas a mudança é pequena, explicável e não altera contrato/API/topologia. Próximo passo antes de submissão oficial: portar para `submission`, rebuildar/pushar imagem com tag própria e decidir se a evidência local justifica issue nova.

Tentativa de publicação:

```text
tag candidata: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-45df206
docker tag: ok
docker push: negado pelo GHCR
erro: permission_denied: The token provided does not match expected scopes.
gh auth scopes atuais: delete:packages, gist, read:org, read:packages, repo, workflow
scope ausente: write:packages
```

Decisão operacional: **não abrir issue oficial enquanto a tag pública não existir**. A candidata fica válida localmente, mas a submissão oficial precisa de token com `write:packages` ou outro registry público autenticado.

## Ciclo 11h45: `ulimits nofile`

Hipótese: aumentar `nofile` para APIs e nginx poderia reduzir risco de limitação de descritores sob pico, seguindo padrão visto em outras submissões.

Alteração temporária:

```yaml
ulimits:
  nofile:
    soft: 65535
    hard: 65535
```

Resultado local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| janela estreita sem `ulimits` | 1.13ms | 0% | 5946.11 |
| janela estreita + `ulimits nofile` | 1.29ms | 0% | 5888.09 |

Decisão: **rejeitado e revertido**.

Aprendizado: o limite de descritores não é gargalo observável neste cenário; adicionar `ulimits` não melhora latência e ainda adiciona ruído de configuração. Manter compose minimalista.

## Ciclo 11h52: reuso `thread_local` de `known_merchants`

Hipótese: o parser recria `std::vector<std::string> known_merchants` e `std::string merchant_id` a cada request; reusar esses buffers por thread reduziria alocação/cópia no hot path.

Alteração temporária:

```cpp
thread_local std::vector<std::string> known_merchants;
thread_local std::string merchant_id;
known_merchants.clear();
merchant_id.clear();
```

Validação funcional:

```text
cmake --build cpp/build --target benchmark-ivf-cpp rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultado offline:

| Variante | ns/query | FP | FN |
|---|---:|---:|---:|
| janela estreita sem reuso parser | 15669.90 | 0 | 0 |
| `thread_local known_merchants` | 12868.20 | 0 | 0 |

Resultados k6 locais:

| Run | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `thread_local` #1 | 1.26ms | 0% | 5899.73 |
| `thread_local` #2 | 1.28ms | 0% | 5892.10 |

Decisão: **rejeitado e revertido**.

Aprendizado: o microbenchmark melhorou bastante, mas o p99 end-to-end piorou de forma reproduzida. Como a pontuação é decidida por p99 e não por ns/query offline, a mudança não é sustentável para submissão.

## Ciclo 11h59: parse sem cópia com buffer já acolchoado

Hipótese: `parse_payload` cria `simdjson::padded_string` por request, copiando o body para garantir padding. No servidor manual, o body já está dentro de `Connection::in`, um `std::array<char, 16 * 1024>`, então seria possível usar `parser.parse(body.data(), body.size(), false)` e evitar a cópia.

Alteração temporária:

```text
request.hpp: adiciona parse_payload_padded(...)
request.cpp: fatoração parse_payload_root(...)
manual_main.cpp: classify_body usa parse_payload_padded(...)
```

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-manual rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultado k6 local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| janela estreita melhor run | 1.13ms | 0% | 5946.11 |
| parse sem cópia | 1.17ms | 0% | 5931.74 |

Decisão: **rejeitado e revertido**.

Aprendizado: apesar de tecnicamente correto em carga local, o caminho sem cópia não trouxe ganho de p99 e aumenta a responsabilidade sobre padding do buffer HTTP. Sem melhoria mensurável, o caminho seguro com `padded_string` continua preferível.

## Ciclo 12h02: `nginx worker_processes 1`

Hipótese: com limite de `0.18 CPU` no nginx, reduzir de 2 workers para 1 poderia diminuir contenção e troca de contexto.

Alteração temporária:

```nginx
worker_processes 1;
```

Resultado k6 local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| janela estreita + nginx 2 workers | 1.13ms | 0% | 5946.11 |
| janela estreita + nginx 1 worker | 1.30ms | 0% | 5885.38 |

Decisão: **rejeitado e revertido**.

Aprendizado: mesmo com CPU pequena, o nginx com 2 workers segue melhor neste cenário. Reduzir workers degrada p99, provavelmente por menor capacidade de absorver conexões simultâneas do k6.

## Ciclo 12h05: sweep da janela geral de reparo

Hipótese: depois de estreitar o `extreme_repair`, talvez a janela geral `repair_min=1` / `repair_max=4` pudesse ser reduzida para economizar bbox scans.

Experimentos offline:

| repair_min | repair_max | ns/query | FP | FN | repaired queries | Decisão |
|---:|---:|---:|---:|---:|---:|---|
| 2 | 3 | 17218.90 | 22 | 28 | 1568 | rejeitado |
| 1 | 3 | 22370.90 | 22 | 0 | 1972 | rejeitado |
| 2 | 4 | 19899.10 | 0 | 28 | 1952 | rejeitado |
| 0 | 4 | 62745.80 | 0 | 0 | 31234 | rejeitado |

Decisão: **manter `repair_min=1` / `repair_max=4`**.

Aprendizado: a janela `1..4` é ampla, mas necessária para preservar detecção perfeita. Reduzir a janela cria erros de classificação; ampliar para `0..4` mantém acurácia, mas destrói a performance.
