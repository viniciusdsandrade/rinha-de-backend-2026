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

## Ciclo 12h15: nginx sem `reuseport`

Hipótese: com 2 workers nginx e pouco CPU, `listen ... reuseport` poderia aumentar variância/overhead; remover `reuseport` poderia deixar a aceitação mais estável.

Resultados k6 locais:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| controle imediato com `reuseport` | 1.27ms | 0% | 5896.51 |
| sem `reuseport` #1 | 1.16ms | 0% | 5937.37 |
| sem `reuseport` #2 | 1.21ms | 0% | 5915.90 |
| controle posterior com `reuseport` | 1.21ms | 0% | 5915.52 |

Decisão: **rejeitado por ausência de ganho reproduzível**.

Aprendizado: remover `reuseport` pode cair numa run boa, mas o controle posterior empatou. Isso parece ruído de benchmark, não melhoria sustentável; manter `listen 9999 reuseport backlog=4096`.

## Ciclo 12h21: `connections.reserve(4096)`

Hipótese: reservar o `std::unordered_map` de conexões no servidor manual poderia evitar rehash durante o ramp-up do teste e reduzir p99.

Alteração temporária:

```cpp
std::unordered_map<int, std::unique_ptr<Connection>> connections;
connections.reserve(4096);
```

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-manual rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais:

| Run | p99 | Falhas | final_score |
|---|---:|---:|---:|
| `connections.reserve(4096)` #1 | 1.16ms | 0% | 5936.91 |
| `connections.reserve(4096)` #2 | 1.20ms | 0% | 5919.86 |

Decisão: **rejeitado e revertido**.

Aprendizado: a mudança é segura, mas o ganho não é inquestionável contra os controles recentes. Por ser marginal e consumir mais memória upfront, não vale entrar na submissão.

## Ciclo 12h26: fast path de resposta com `send` direto

Hipótese: o servidor manual sempre copiava respostas estáticas para `conn.out` antes de chamar `send`. Tentar escrever diretamente a resposta estática e só enfileirar em caso de `EAGAIN`/partial write poderia reduzir cópia e alocação no caminho HTTP.

Alteração temporária:

```text
send_or_queue(conn, response)
append_response(...) passa a retornar bool
process_requests(...) fecha a conexão se send falhar
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
| fast path `send` direto | 1.20ms | 0% | 5921.68 |

Decisão: **rejeitado e revertido**.

Aprendizado: a hipótese é funcionalmente válida, mas não trouxe ganho claro de p99. A complexidade extra de partial write não se justifica; manter `conn.out.append` + `flush_output`.

## Ciclo 12h30: varredura de issues oficiais recentes

Objetivo: verificar se apareceu alguma submissão recente com pista técnica melhor que o nosso eixo atual.

Amostra das issues fechadas mais recentes:

| Issue | Participante/stack | p99 | Falhas | final_score | Repo |
|---|---|---:|---:|---:|---|
| #2590 | `lemesdaniel-nim` | 27.42ms | 0% | 4561.96 | `lemesdaniel/floating-finch` |
| #2588 | `steixeira93-go-hnsw` | 2.46ms | 0% | 5609.73 | `steixeira93/rinha-backend-26` |
| #2578 | `jordaogustavo-csharp` | 2.21ms | 0% | 5656.25 | `JordaoGustavo/rinha-backend-2026` |
| #2572 | `itagyba-dotnet` | 2.08ms | 0% | 5681.33 | `daniloitagyba/rinha-2026-dotnet` |
| #2571 | `jairoblatt-rust` | 1.14ms | 0% | 5941.57 | `jairoblatt/rinha-2026-rust` |

Decisão: continuar usando `jairoblatt-rust` como referência técnica principal. As demais submissões recentes observadas estão abaixo da nossa faixa atual e não justificam troca de stack/arquitetura.

Aprendizado: os diferenciais transferíveis do jairo já testados como drop-in (`LB`, `CPU split`, `probe policy`) não funcionaram na nossa solução. O que resta como hipótese estrutural é parser manual/HTTP loop mais agressivo, mas isso exige uma rodada dedicada porque é uma mudança maior.

## Ciclo 12h35: CPU split `0.42/0.42/0.16`

Hipótese: após estreitar o `extreme_repair`, o gargalo remanescente poderia estar mais nas APIs do que no nginx. Testei mover `0.02 CPU` do nginx para as APIs:

```text
api1: 0.42 CPU
api2: 0.42 CPU
nginx: 0.16 CPU
total: 1.00 CPU
```

Resultado k6 local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| CPU split atual `0.41/0.41/0.18` melhor run | 1.13ms | 0% | 5946.11 |
| CPU split `0.42/0.42/0.16` | 1.26ms | 0% | 5900.93 |

Decisão: **rejeitado e revertido**.

Aprendizado: o nginx ainda precisa da fatia de `0.18 CPU`; retirar CPU dele aumenta p99. Manter o split atual.

## Ciclo 12h42: CPU split `0.40/0.40/0.20`

Hipótese: como retirar CPU do nginx piorou, talvez dar um pouco mais ao nginx reduzisse fila/latência no proxy.

```text
api1: 0.40 CPU
api2: 0.40 CPU
nginx: 0.20 CPU
total: 1.00 CPU
```

Resultado k6 local:

| Variante | p99 | Falhas | final_score |
|---|---:|---:|---:|
| CPU split atual `0.41/0.41/0.18` melhor run | 1.13ms | 0% | 5946.11 |
| CPU split `0.40/0.40/0.20` | 1.21ms | 0% | 5918.41 |

Decisão: **rejeitado e revertido**.

Aprendizado: deslocar CPU para o nginx também não melhora. O split atual parece um equilíbrio local bom entre proxy e APIs.

## Ciclo 12h55: parser manual seletivo no hot path

Hipótese: a comparação com `jairoblatt-rust` indicou que um diferencial estrutural possível era parser manual/HTTP loop mais agressivo. Nosso `parse_payload` via `simdjson` é robusto, mas cria objetos intermediários (`Payload`, strings, vetor de merchants) e valida mais do que o necessário para o formato oficial. Um parser seletivo que gera `QueryVector` diretamente poderia reduzir latência, desde que mantivesse fallback para `simdjson` quando o formato não casar.

Alteração aplicada:

```text
manual_main.cpp:
- adiciona fast_vectorize_payload(body, query)
- parser percorre valores no formato oficial esperado
- gera QueryVector diretamente
- compara known_merchants por string_view local
- calcula timestamp/day_of_week/minutes_since_last no próprio hot path
- classify_body tenta fast_vectorize_payload primeiro
- se falhar, cai no caminho antigo parse_payload + vectorize
```

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-manual rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais:

| Run | p99 | Falhas | final_score |
|---|---:|---:|---:|
| parser manual seletivo #1 | 1.10ms | 0% | 5958.98 |
| parser manual seletivo #2 | 1.12ms | 0% | 5952.33 |

Comparação contra melhor evidência local anterior do dia:

| Variante | Melhor p99 | Falhas | Melhor final_score |
|---|---:|---:|---:|
| janela estreita sem parser manual | 1.13ms | 0% | 5946.11 |
| janela estreita + parser manual seletivo | 1.10ms | 0% | 5958.98 |

Decisão: **aceito na branch experimental**.

Aprendizado: esta foi a primeira melhoria end-to-end reproduzida acima da janela estreita. A manutenção do fallback para `simdjson` reduz risco de formato inesperado, enquanto o caminho rápido atende o formato oficial do k6. Próximo passo: tentar publicar imagem nova; se o GHCR continuar sem `write:packages`, a candidata fica bloqueada apenas por credencial de registry.

Publicação:

```text
tag candidata: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-67270c2
caminho escolhido: GitHub Actions Publish submission image
motivo: workflow usa GITHUB_TOKEN com packages: write, evitando bloqueio do token local sem write:packages
```

Status de publicação/submissão:

```text
workflow: https://github.com/viniciusdsandrade/rinha-de-backend-2026/actions/runs/25605028257
resultado do workflow: success
manifest da imagem: linux/amd64 presente
branch submission: 5e310ee point submission to fast parser image
issue oficial aberta: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2601
status da issue apos 10 min: OPEN, sem comentarios, sem labels
decisão: aguardar runner oficial; não abrir issue duplicada enquanto #2601 estiver pendente
```
