# Daily Report - 2026-04-23

## Contexto

Rodada focada em deixar a implementação Rust mais competitiva depois de a branch `submission` já ter atingido a faixa
de teto do benchmark oficial local.

Premissas efetivas desta rodada:

- manter a topologia oficial da submissão: `nginx stream` + 2 APIs + rede `bridge`
- manter `100%` de acurácia e `0` `http_errors`
- assumir AVX2/FMA como requisito efetivo
- aceitar somente mudanças pequenas, mensuráveis e sustentáveis
- preservar a comparação por benchmark oficial local em rodadas repetidas

O teto prático de requisições do cenário `test/test.js` é `14355`, conforme `estimated-requests.sh`.

## Descobertas consolidadas do dia

Descobertas técnicas:

- o score local já está limitado pelo teto prático de iterações do cenário oficial local, não por acurácia
- quando `p99 < 10ms`, o multiplicador de latência fica em `1.0`; a disputa passa a ser `raw_score` e consistência de
  completar todas as iterações
- a troca de framework HTTP só valeu quando foi feita como servidor manual simples; a troca anterior para `hyper` direto
  já havia sido rejeitada por piorar o `k6` oficial
- no estado atual da submissão, o caminho oficial usa `nginx stream` com Unix sockets; por isso, hipóteses de TCP como
  `TCP_NODELAY` não atacam o hot path medido
- `target-cpu=x86-64-v3` é preferível a `target-cpu=native` para esta submissão porque assume AVX2/FMA sem acoplar a
  imagem ao processador exato da máquina local
- o ganho mais importante do `x86-64-v3` não foi reduzir `p99`; foi reduzir a mediana e melhorar a consistência do
  `raw_score` no teto `14355`

Descobertas operacionais:

- o benchmark comparável desta rodada foi executado no contexto Docker `desktop-linux`
- o Docker Desktop chegou a estar parado durante a rodada e foi reativado antes dos benchmarks comparativos
- existe também um Docker Engine no contexto `default`, mas ele não deve ser misturado com resultados do `desktop-linux`
  sem declarar explicitamente a troca de daemon
- o Git continuou emitindo aviso de `gc.log` e objetos soltos durante commits; isso é manutenção local e não afetou os
  resultados de execução

## Experimento 1 - substituir `axum` por servidor HTTP manual

Objetivo:

- remover overhead de framework no hot path
- manter o mesmo parser JSON, a mesma classificação e o mesmo contrato HTTP
- responder com buffers HTTP estáticos para `/ready`, erros e os 6 resultados possíveis de score

Arquivos alterados:

- `Cargo.toml`
- `Cargo.lock`
- `src/http.rs`
- `src/main.rs`
- `tests/http_api.rs`

Commit:

- `d3b6f1b` - `replace axum with manual http server`

Validação funcional:

- `cargo test`: passou
- `git diff --check`: passou
- `docker compose config -q`: passou

Benchmark inicial de 3 rodadas:

- diretório: `/tmp/rinha-bench-rust-manual-http-20260423-173600`
- rodada 1:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.16ms`
    - `p90`: `1.45ms`
    - `med`: `1.06ms`
    - `max`: `9.35ms`
- rodada 2:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `1.71ms`
    - `p90`: `0.95ms`
    - `med`: `0.58ms`
    - `max`: `8.08ms`
- rodada 3:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.05ms`
    - `p90`: `1.48ms`
    - `med`: `1.05ms`
    - `max`: `8.02ms`

Benchmark promovido de 5 rodadas:

- diretório: `/tmp/rinha-bench-rust-manual-http-5x-20260423-174030`
- mediana:
    - `final_score`: `14354`
    - `raw_score`: `14354`
    - `p99`: `2.11ms`
    - `p90`: `1.53ms`
    - `med`: `1.11ms`
    - pior `p99`: `2.45ms`
    - pior `max`: `8.56ms`
    - `http_errors`: `0`

Benchmark pós-commit de 3 rodadas:

- diretório: `/tmp/rinha-bench-rust-manual-http-postcommit-20260423-175458`
- rodada 1:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `1.97ms`
    - `p90`: `1.49ms`
    - `med`: `1.12ms`
    - `max`: `4.35ms`
- rodada 2:
    - `final_score`: `14351`
    - `raw_score`: `14351`
    - `p99`: `2.04ms`
    - `p90`: `1.50ms`
    - `med`: `1.12ms`
    - `max`: `14.80ms`
- rodada 3:
    - `final_score`: `14354`
    - `raw_score`: `14354`
    - `p99`: `1.81ms`
    - `p90`: `1.35ms`
    - `med`: `1.01ms`
    - `max`: `6.48ms`

Decisão:

- aceita
- reduziu latência do stack Rust de forma relevante
- manteve acurácia e erros em `0`
- ainda apresentou pequena variação de raw score em rodada isolada, então a próxima hipótese passou a mirar consistência
  no teto `14355`

## Experimento 2 - compilar a imagem Rust para `x86-64-v3`

Objetivo:

- permitir que o compilador assuma o nível de CPU compatível com AVX2/FMA
- evitar `target-cpu=native`, que seria menos portátil e mais dependente da máquina exata
- testar melhoria somente via build flag, sem alterar contrato, algoritmo ou topologia

Arquivo alterado:

- `Dockerfile`

Mudança:

- `ENV RUSTFLAGS="-C target-cpu=x86-64-v3"` no estágio builder

Commit:

- `bd17404` - `compile rust image for x86-64-v3`

Validação:

- `rustc --print target-cpus`: confirmou suporte a `x86-64-v3`
- `git diff --check`: passou
- `docker compose config -q`: passou
- benchmark oficial local de 3 rodadas passou com build completo da imagem

Benchmark de 3 rodadas:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-20260423-175940`
- rodada 1:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.07ms`
    - `p90`: `0.88ms`
    - `med`: `0.61ms`
    - `max`: `14.09ms`
- rodada 2:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `1.85ms`
    - `p90`: `1.41ms`
    - `med`: `0.88ms`
    - `max`: `5.95ms`
- rodada 3:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.04ms`
    - `p90`: `1.39ms`
    - `med`: `0.65ms`
    - `max`: `6.63ms`

Mediana:

- `final_score`: `14355`
- `raw_score`: `14355`
- `p99`: `2.04ms`
- `p90`: `1.39ms`
- `med`: `0.65ms`
- pior `p99`: `2.07ms`
- pior `max`: `14.09ms`
- `http_errors`: `0`

Comparação direta contra o baseline pós-commit imediatamente anterior:

- `final_score` mediano: `14354 -> 14355`
- `raw_score` mediano: `14354 -> 14355`
- `p99` mediano: `1.97ms -> 2.04ms`
- `p90` mediano: `1.49ms -> 1.39ms`
- `med` mediano: `1.12ms -> 0.65ms`
- pior queda de raw score observada: `14351 -> 14355`

Decisão:

- aceita
- melhorou a consistência no teto do benchmark oficial local em 3/3 rodadas
- reduziu fortemente a mediana de latência
- o pequeno aumento de `p99` mediano é irrelevante para score enquanto permanece muito abaixo do alvo de `10ms`

## Retomada e descoberta operacional pós-restart

Ao tentar prosseguir para o `5x` adicional do estado `x86-64-v3`, a bateria foi interrompida por instabilidade do
ambiente Docker Desktop.

Evidências observadas:

- o host havia reiniciado poucos minutos antes da retomada:
    - `uptime`: cerca de `7-9min`
- o Docker Desktop mudou de `SessionID` durante a retomada
- os containers do projeto `rinha-bench-rust` ficaram `Exited (255)`
- não houve diretório de resultado confiável preservado para a bateria abortada
- havia containers externos no mesmo Docker Desktop:
    - `ecv-document-portal-mailhog-1`
    - `kind-cloud-provider`
    - `kind-registry-mirror`
    - `desktop-control-plane`
- o container `desktop-control-plane` chegou a aparecer consumindo CPU e memória relevantes dentro do Docker Desktop
- o host estava com swap praticamente cheio:
    - `Swap`: `3.9GiB / 4.0GiB`
- o Docker Desktop reportou memória de VM baixa para o tipo de benchmark:
    - `CPUs=16`
    - `Mem=1910161408`
    - `OS=Docker Desktop`
    - `Server=29.4.0`

Decisão operacional:

- a bateria `5x` iniciada nesse estado foi considerada inválida
- os containers externos foram parados temporariamente para limpar o ambiente de benchmark
- foi rodada uma sanidade oficial `1x` no Docker Desktop limpo

Sanidade `1x` no Docker Desktop após limpeza dos containers externos:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-sanity-20260423-183815`
- `final_score`: `2034.12`
- `raw_score`: `13358`
- `p99`: `65.67ms`
- `p90`: `19.29ms`
- `med`: `2.95ms`
- `max`: `181.36ms`
- `http_errors`: `0`
- acurácia: `100%`

Interpretação:

- a stack Rust não falhou funcionalmente
- a queda veio de latência operacional extrema no Docker Desktop
- por haver `0` erros HTTP e `100%` de acurácia, o resultado não aponta para regressão de parser, classificador ou
  contrato HTTP

Para separar código de ambiente, foi executada a mesma sanidade `1x` no daemon Docker `default`.

Sanidade `1x` no daemon `default`:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-default-sanity-20260423-184000`
- `final_score`: `14278`
- `raw_score`: `14278`
- `p99`: `8.51ms`
- `p90`: `2.32ms`
- `med`: `1.66ms`
- `max`: `45.00ms`
- `http_errors`: `0`
- acurácia: `100%`

Interpretação da comparação:

- o mesmo código ficou muito melhor no daemon `default` do que no Docker Desktop degradado
- ainda assim, o daemon `default` também não reproduziu o teto `14355`; isso indica que o host ainda estava em período de
  aquecimento/pressão de recursos após restart
- a decisão correta é não aceitar nem rejeitar novas hipóteses de código com base nesses números

Conclusão da retomada:

- o próximo passo não deve ser tuning de código
- o próximo passo deve ser estabilizar o ambiente de benchmark e revalidar o baseline atual

### Sanidade adicional após limpeza do Docker Desktop

Depois de parar os containers externos, foi feita uma nova sanidade no Docker Desktop para verificar se o ambiente havia
voltado ao regime normal.

Resultado:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-desktop-sanity2-20260423-184246`
- o `k6` foi morto pelo sistema operacional antes de gerar `test/results.json`
- código de saída observado no shell: `137`
- log do `k6` antes da morte:
    - `13453 complete`
    - `0 interrupted iterations`
    - execução passou de `1m34s` sem finalizar o processo de forma saudável
- estado do host no momento:
    - `uptime`: cerca de `15min`
    - `load average`: `89.67, 35.97, 14.28`
    - `Swap`: `4.0GiB / 4.0GiB`

Confirmação via kernel log:

- horário: `2026-04-23 18:45:45`
- evento: `Out of memory`
- processo morto: `k6`
- `pid`: `28323`
- `anon-rss`: aproximadamente `3.9GiB`

Interpretação:

- a sanidade adicional confirmou que o ambiente estava sob OOM real
- qualquer benchmark executado nesse estado é inválido para comparar implementações
- a próxima etapa técnica deve ser recuperar memória/ambiente antes de medir performance novamente

### Recuperação parcial usando o daemon Docker `default`

Como o Docker Desktop continuou inválido para benchmark, ele foi parado temporariamente e os stacks residuais da Rinha
foram limpos nos dois daemons.

Estado observado após parar o Docker Desktop:

- memória disponível subiu para cerca de `4.4GiB`
- swap usada caiu de `4.0GiB` para cerca de `2.7GiB`
- reciclar swap exigiria senha `sudo`, então não foi executado
- daemon `default` reportou:
    - `CPUs=16`
    - `Mem=7993290752`
    - `OS=Ubuntu 24.04.4 LTS`
    - `Server=29.4.1`

Sanidade `1x` no daemon `default` após estabilização parcial:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-default-sanity2-20260423-184909`
- `final_score`: `14354`
- `raw_score`: `14354`
- `p99`: `2.00ms`
- `p90`: `1.43ms`
- `med`: `1.03ms`
- `max`: `4.00ms`
- `http_errors`: `0`
- acurácia: `100%`

Baseline `3x` no daemon `default`:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-default-3x-20260423-185048`
- rodadas:
    - `14355`, `p99=2.36ms`, `p90=1.49ms`, `med=1.01ms`, `max=6.77ms`
    - `14355`, `p99=2.21ms`, `p90=1.49ms`, `med=1.07ms`, `max=5.81ms`
    - `14355`, `p99=2.30ms`, `p90=1.50ms`, `med=1.07ms`, `max=4.07ms`
- mediana:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.30ms`
    - pior `p99`: `2.36ms`
    - `http_errors`: `0`

Baseline `5x` no daemon `default`:

- diretório: `/tmp/rinha-bench-rust-x86-64-v3-default-5x-20260423-185508`
- rodadas:
    - `14354`, `p99=2.40ms`, `p90=1.52ms`, `med=1.06ms`, `max=4.95ms`
    - `14354`, `p99=2.26ms`, `p90=1.51ms`, `med=1.07ms`, `max=4.54ms`
    - `14355`, `p99=2.31ms`, `p90=1.58ms`, `med=1.09ms`, `max=5.49ms`
    - `14355`, `p99=2.37ms`, `p90=1.58ms`, `med=1.09ms`, `max=7.76ms`
    - `14355`, `p99=2.13ms`, `p90=1.50ms`, `med=1.12ms`, `max=4.34ms`
- mediana:
    - `final_score`: `14355`
    - `raw_score`: `14355`
    - `p99`: `2.31ms`
    - pior `p99`: `2.40ms`
    - `p90`: `1.52ms`
    - `med`: `1.09ms`
    - pior `max`: `7.76ms`
    - `http_errors`: `0`

Conclusão:

- a implementação atual está saudável quando o benchmark roda fora do Docker Desktop degradado
- o daemon `default` está apto para experimentos locais controlados nesta sessão
- os resultados do daemon `default` não devem ser misturados com o histórico `desktop-linux` sem declarar a mudança

### Correção dos testes para o critério oficial atual

Durante a revisão contra a documentação oficial atual, foi identificada uma inconsistência crítica nos três testes locais
usados para comparar Rust, C++ e C:

- o `test/test.js` ainda usava a fórmula legada:
    - `raw_score = TP + TN - FP - 3*FN - 5*Err`
    - `latency_multiplier = 10 / max(p99, 10)`
    - `final_score = max(0, raw_score) * latency_multiplier`
- a documentação oficial atual usa outra escala:
    - `score_p99` logarítmico, com teto em `p99 <= 1ms` e corte em `p99 > 2000ms`
    - `score_det` logarítmico com `E = FP + 3*FN + 5*Err`, `epsilon = E/N` e corte se `failure_rate > 15%`
    - `final_score = score_p99 + score_det`
    - faixa total: `-6000` a `6000`

Ações aplicadas nos três worktrees:

- Rust: `/home/andrade/Desktop/rinha-de-backend-2026-rust/test/test.js`
- C++: `/home/andrade/Desktop/rinha-de-backend-2026/test/test.js`
- C: `/home/andrade/Desktop/rinha-de-backend-2026-c/test/test.js`

Mudanças de teste:

- substituída a fórmula legada pelo scoring oficial atual
- adicionadas no `setup()` verificações de contrato:
    - `GET /ready` precisa responder `2xx`
    - `POST /fraud-score` precisa responder `200`
    - a resposta precisa conter `approved` booleano e `fraud_score` numérico entre `0` e `1`
    - rotas/métodos fora dos dois endpoints oficiais não podem responder com sucesso
- erros de JSON inválido ou schema inválido no `POST /fraud-score` agora entram como `http_errors`
- `run.sh` nos três worktrees passou a:
    - remover `test/results.json` antes da execução
    - falhar se o k6 falhar
    - imprimir o log do k6 somente em caso de erro
    - evitar leitura de resultado stale

Validações estáticas:

- Rust: `git diff --check`, `bash -n run.sh`, `k6 inspect test/test.js`
- C++: `git diff --check`, `bash -n run.sh`, `k6 inspect test/test.js`
- C: `git diff --check`, `bash -n run.sh`, `k6 inspect test/test.js`

Validações runtime no daemon Docker `default`, uma execução por stack:

Rust:

- `p99`: `2.53ms`
- `p99_score`: `2597.53`
- `detection_score`: `3000`
- `final_score`: `5597.53`
- `http_errors`: `0`
- `failure_rate`: `0.00%`

C++:

- `p99`: `2.49ms`
- `p99_score`: `2604.56`
- `detection_score`: `3000`
- `final_score`: `5604.56`
- `http_errors`: `0`
- `failure_rate`: `0.00%`

C:

- `p99`: `2.49ms`
- `p99_score`: `2603.57`
- `detection_score`: `3000`
- `final_score`: `5603.57`
- `http_errors`: `0`
- `failure_rate`: `0.00%`

Interpretação:

- os resultados antigos `raw_score/final_score` na faixa de `14355` pertencem ao script legado e não são comparáveis com o
  critério oficial atual
- as três implementações mantiveram detecção perfeita na amostra executada localmente
- com `E=0`, o `detection_score` já está no teto de `3000`
- o espaço real remanescente está no `p99_score`: aproximar `p99` de `1ms` é o caminho para chegar perto de `6000`
- abaixo de `1ms`, o score de latência satura e melhorias adicionais deixam de pontuar

### Comparativo oficial atualizado `10x` entre Rust, C++ e C

Foi executada uma bateria comparativa `10x` com o `test/test.js` já corrigido para o critério oficial atual.

Condições:

- daemon Docker: `default`
- diretório dos resultados: `/tmp/rinha-official-10x-20260423-200522`
- uma stack por vez, com `docker compose up -d --build`
- `GET /ready` validado antes do k6
- resultados persistidos em JSON por rodada:
    - Rust: `/tmp/rinha-official-10x-20260423-200522/rust/run-*.json`
    - C++: `/tmp/rinha-official-10x-20260423-200522/cpp/run-*.json`
    - C: `/tmp/rinha-official-10x-20260423-200522/c/run-*.json`

Branches avaliadas:

- Rust: `submission`, commit `363437f`
- C++: `submission-2`, commit `d245a39`
- C: `submission-c`, commit `925ff44`

Resultado agregado:

| Stack | Runs | Mediana `final_score` | Média `final_score` | Pior `final_score` | Melhor `final_score` | Mediana `p99` | Pior `p99` | Melhor `p99` | Erros / FP / FN |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| C | 10 | `5657.21` | `5661.35` | `5624.56` | `5719.17` | `2.20ms` | `2.37ms` | `1.91ms` | `0 / 0 / 0` |
| C++ | 10 | `5652.21` | `5653.63` | `5630.53` | `5684.05` | `2.225ms` | `2.34ms` | `2.07ms` | `0 / 0 / 0` |
| Rust | 10 | `5632.75` | `5639.48` | `5597.11` | `5701.20` | `2.325ms` | `2.53ms` | `1.99ms` | `0 / 0 / 0` |

Detalhes por stack:

Rust:

- `runs`: `10`
- mediana `final_score`: `5632.75`
- média `final_score`: `5639.48`
- pior `final_score`: `5597.11`
- melhor `final_score`: `5701.20`
- mediana `p99`: `2.325ms`
- pior `p99`: `2.53ms`
- melhor `p99`: `1.99ms`
- mediana `p90`: `1.535ms`
- mediana da latência mediana: `1.09ms`
- pior `max`: `9.98ms`
- `detection_score`: `3000` em todas as rodadas
- `http_errors`: `0`
- `false_positive_detections`: `0`
- `false_negative_detections`: `0`
- `failure_rate`: `0.00%` em todas as rodadas

C++:

- `runs`: `10`
- mediana `final_score`: `5652.21`
- média `final_score`: `5653.63`
- pior `final_score`: `5630.53`
- melhor `final_score`: `5684.05`
- mediana `p99`: `2.225ms`
- pior `p99`: `2.34ms`
- melhor `p99`: `2.07ms`
- mediana `p90`: `1.445ms`
- mediana da latência mediana: `1.02ms`
- pior `max`: `13.37ms`
- `detection_score`: `3000` em todas as rodadas
- `http_errors`: `0`
- `false_positive_detections`: `0`
- `false_negative_detections`: `0`
- `failure_rate`: `0.00%` em todas as rodadas

C:

- `runs`: `10`
- mediana `final_score`: `5657.21`
- média `final_score`: `5661.35`
- pior `final_score`: `5624.56`
- melhor `final_score`: `5719.17`
- mediana `p99`: `2.20ms`
- pior `p99`: `2.37ms`
- melhor `p99`: `1.91ms`
- mediana `p90`: `1.395ms`
- mediana da latência mediana: `0.97ms`
- pior `max`: `14.35ms`
- `detection_score`: `3000` em todas as rodadas
- `http_errors`: `0`
- `false_positive_detections`: `0`
- `false_negative_detections`: `0`
- `failure_rate`: `0.00%` em todas as rodadas

Interpretação:

- todas as stacks atingiram detecção perfeita na bateria local
- como `detection_score = 3000` em todos os casos, a diferença de score veio exclusivamente do `p99_score`
- C venceu por mediana de `final_score`, média de `final_score`, mediana de `p99`, mediana de `p90` e latência mediana
- C++ teve melhor pior caso de `final_score` e menor pior `p99`, portanto ainda é a alternativa mais estável no pior caso
- Rust ficou tecnicamente correto, mas abaixo de C e C++ nesta bateria
- a vantagem de C sobre C++ foi pequena (`+5.00` pontos de mediana), mas a vantagem de C sobre Rust foi mais clara (`+24.46` pontos de mediana)

Decisão operacional:

- a próxima rodada de otimização agressiva deve focar em C na branch `submission-c`
- qualquer mudança deve preservar `E=0`, `http_errors=0` e `failure_rate=0.00%`
- o objetivo técnico passa a ser reduzir `p99` de aproximadamente `2.20ms` para mais perto de `1ms`, pois abaixo de `1ms`
  o score de latência satura

## Rodada agressiva em C

Branch:

- `submission-c`

Commit publicado:

- `5d3168e` - `optimize c parser and avx2 search`

Estado base usado para comparação:

- commit anterior da branch C: `925ff44` - `align benchmark with official scoring`
- benchmark comparativo C anterior: mediana `final_score 5657.21`, média `5661.35`, melhor `5719.17`, pior `5624.56`,
  mediana `p99 2.20ms`, zero FP/FN/HTTP errors

Alterações mantidas:

- parser numérico manual para `float` e `uint32_t`, removendo `strtof`/`strtoul` do caminho quente
- parse de timestamp antecipado durante o parse do payload, evitando recalcular `requested_at` e `last_transaction.timestamp`
  durante a vetorização
- cálculo antecipado de `merchant_mcc_risk` no parse do merchant
- lookup de MCC por `switch` numérico em vez de cadeia de `strcmp`
- `ReferenceSet.ordered_dims` com ponteiros das dimensões já materializados na ordem de poda
- kernel AVX2 de 14 dimensões desdobrado explicitamente, preservando kNN exato, distância euclidiana e threshold `0.6`
- split de CPU final preservado em `api=0.45 + api=0.45 + nginx=0.10`

Hipóteses descartadas:

- fast-path JSON dependente do formato serializado pelo `k6`
  - resultado 3x: `5644.37`, `5642.35`, `5676.41`
  - mediana: `5644.37`
  - p99: `2.27ms`, `2.28ms`, `2.11ms`
  - veredito: sem ganho sustentável contra o `fastparse` simples; removido antes do commit
  - evidência: `/tmp/rinha-c-experiment-20260423-205642/c-fastpath-json`
- split `api=0.46 + api=0.46 + nginx=0.08`
  - resultado 3x: `5751.35`, `3631.17`, `5737.05`
  - p99: `1.77ms`, `233.79ms`, `1.83ms`
  - veredito: rejeitado por instabilidade severa no segundo run, apesar de runs bons
  - evidência: `/tmp/rinha-c-experiment-20260423-210714/c-unrolled-api046-nginx008`
- split `api=0.455 + api=0.455 + nginx=0.09`
  - resultado 3x: `5662.16`, `5663.85`, `5699.90`
  - p99: `2.18ms`, `2.17ms`, `2.00ms`
  - veredito: estável, mas pior que o split base com o kernel novo
  - evidência: `/tmp/rinha-c-experiment-20260423-211105/c-unrolled-api0455-nginx009`
- flags `-fomit-frame-pointer -funroll-loops`
  - resultado 3x: `5728.10`, `5663.26`, `5696.22`
  - mediana: `5696.22`
  - p99: `1.87ms`, `2.17ms`, `2.01ms`
  - veredito: correto, mas sem ganho claro contra a validação 3x do kernel mantido (`5719.87`); revertido antes do commit
  - evidência: `/tmp/rinha-c-experiment-20260423-212807/c-unrolled-fomit-funroll`
- PGO no Dockerfile com trainer sobre `test/test-data.json`
  - trainer local validado com `14500` requests e checksum `337898`
  - resultado 3x: `5691.15`, `5698.34`, `5653.32`
  - mediana: `5691.15`
  - p99: `2.04ms`, `2.00ms`, `2.22ms`
  - veredito: estável e correto, mas não compensou a complexidade extra de build; removido antes do commit
  - evidência: `/tmp/rinha-c-experiment-20260423-213313/c-pgo-dockerfile`
- fast-path HTTP com `memmem` para fim de cabeçalho e `Content-Length`
  - resultado 3x: `5654.50`, `5657.33`, `5649.49`
  - mediana: `5654.50`
  - p99: `2.22ms`, `2.20ms`, `2.24ms`
  - veredito: regressão clara; o loop manual anterior é melhor para cabeçalhos pequenos
  - evidência: `/tmp/rinha-c-experiment-20260423-213851/c-http-memmem-content-length`

Validação 3x do kernel mantido:

- experimento: `fastparse + ordered_dims + AVX2 unrolled`, split base `0.45/0.45/0.10`
- resultados: `5675.50`, `5722.39`, `5719.87`
- mediana: `5719.87`
- p99: `2.11ms`, `1.90ms`, `1.91ms`
- FP/FN/HTTP errors: `0/0/0`
- evidência: `/tmp/rinha-c-experiment-20260423-210301/c-fastparse-avx2-unrolled`

Validação 10x oficial local do estado commitado:

- experimento: `fastparse + ordered_dims + AVX2 unrolled`, split base `0.45/0.45/0.10`
- evidência: `/tmp/rinha-c-official-10x-20260423-211500/c-fastparse-avx2-unrolled-base-split`
- resultados por run:
  - run 1: `5668.38`, `p99 2.15ms`, mediana HTTP `0.98ms`, p90 `1.38ms`
  - run 2: `5704.85`, `p99 1.97ms`, mediana HTTP `0.98ms`, p90 `1.37ms`
  - run 3: `5710.25`, `p99 1.95ms`, mediana HTTP `0.94ms`, p90 `1.37ms`
  - run 4: `5690.48`, `p99 2.04ms`, mediana HTTP `0.94ms`, p90 `1.42ms`
  - run 5: `5806.97`, `p99 1.56ms`, mediana HTTP `0.85ms`, p90 `1.15ms`
  - run 6: `5679.31`, `p99 2.09ms`, mediana HTTP `1.02ms`, p90 `1.44ms`
  - run 7: `5698.47`, `p99 2.00ms`, mediana HTTP `1.01ms`, p90 `1.43ms`
  - run 8: `5667.32`, `p99 2.15ms`, mediana HTTP `1.03ms`, p90 `1.47ms`
  - run 9: `5654.42`, `p99 2.22ms`, mediana HTTP `1.03ms`, p90 `1.48ms`
  - run 10: `5666.94`, `p99 2.15ms`, mediana HTTP `0.95ms`, p90 `1.47ms`

Resumo 10x:

- média `final_score`: `5694.739`
- mediana superior `final_score`: `5690.48`
- mediana estatística `final_score`: `5684.895`
- pior `final_score`: `5654.42`
- melhor `final_score`: `5806.97`
- mediana superior `p99`: `2.09ms`
- mediana estatística `p99`: `2.065ms`
- pior `p99`: `2.22ms`
- melhor `p99`: `1.56ms`
- `detection_score`: `3000` em todas as rodadas
- `http_errors`: `0`
- `false_positive_detections`: `0`
- `false_negative_detections`: `0`
- `failure_rate`: `0.00%` em todas as rodadas

Comparação contra o C anterior:

- média `final_score`: `5694.739` contra `5661.35` (`+33.389`)
- mediana superior `final_score`: `5690.48` contra `5657.21` (`+33.27`)
- pior `final_score`: `5654.42` contra `5624.56` (`+29.86`)
- melhor `final_score`: `5806.97` contra `5719.17` (`+87.80`)
- mediana superior `p99`: `2.09ms` contra `2.20ms`
- pior `p99`: `2.22ms` contra `2.37ms`
- melhor `p99`: `1.56ms` contra `1.91ms`
- detecção permaneceu perfeita: `0` FP, `0` FN e `0` HTTP errors

Conclusão:

- a rodada trouxe ganho material e sustentável no C sem alterar a semântica da API, da vetorização ou do kNN exato
- o ganho veio de reduzir overhead no parser e no loop AVX2, não de aceitar aproximação ou erro de detecção
- o split de CPU deve permanecer conservador em `nginx=0.10`; reduzir o nginx abaixo disso pode produzir p99 catastrófico
- PGO foi testado e não trouxe ganho suficiente nesta implementação
- fast-path HTTP com `memmem` também foi testado e regrediu
- a próxima hipótese de alto impacto deve mirar uma estratégia de kernel comprovadamente mais barata ou um ajuste de nginx
  que não reduza CPU abaixo do limite seguro, sempre mantendo a bateria 10x como critério de aceite

## Rodada C com busca exata agrupada

Branch:

- `submission-c`

Commit publicado:

- `3f49e35` - `add exact grouped c search`

Estado base usado para comparação:

- commit anterior da branch C: `5d3168e` - `optimize c parser and avx2 search`
- benchmark 10x anterior: média `5694.739`, mediana superior `5690.48`, pior `5654.42`, melhor `5806.97`,
  mediana superior `p99 2.09ms`, zero FP/FN/HTTP errors

Alteração mantida:

- `ReferenceSet` passou a construir uma visão agrupada em memória no startup
- os grupos são definidos por dimensões discretas de alta poda:
  - `last_transaction:null` (`dim5 == -1` e `dim6 == -1`)
  - `is_online` (`dim9`)
  - `card_present` (`dim10`)
  - `unknown_merchant` (`dim11`)
- cada referência agrupada preserva o índice original para manter o desempate equivalente à varredura original
- a busca AVX2 visita grupos por lower-bound mínimo e interrompe quando o lower-bound do grupo é estritamente maior
  que o quinto melhor candidato atual
- a busca continua exata: não há ANN, aproximação, alteração de `k`, alteração de distância ou alteração de threshold
- os vetores originais continuam intactos; a visão agrupada é apenas uma cópia em memória usada no hot path de classificação

Validação funcional:

- `make -C c test`: passou
- `make -C c all`: passou
- `docker --context default compose config -q`: passou
- `git diff --check`: passou

Validação 3x oficial local:

- experimento: `exact grouped lower-bound`, split base `0.45/0.45/0.10`
- resultados: `5814.39`, `5849.17`, `5858.84`
- mediana: `5849.17`
- p99: `1.53ms`, `1.42ms`, `1.38ms`
- FP/FN/HTTP errors: `0/0/0`
- evidência: `/tmp/rinha-c-experiment-20260423-215852/c-exact-grouped-lower-bound`

Validação 10x oficial local do estado commitado:

- experimento: `exact grouped lower-bound`, split base `0.45/0.45/0.10`
- evidência: `/tmp/rinha-c-official-10x-20260423-220252/c-exact-grouped-lower-bound`
- resultados por run:
  - run 1: `5839.79`, `p99 1.45ms`, mediana HTTP `0.59ms`, p90 `0.94ms`
  - run 2: `5882.52`, `p99 1.31ms`, mediana HTTP `0.64ms`, p90 `0.97ms`
  - run 3: `5886.33`, `p99 1.30ms`, mediana HTTP `0.64ms`, p90 `0.96ms`
  - run 4: `5813.90`, `p99 1.53ms`, mediana HTTP `0.65ms`, p90 `1.05ms`
  - run 5: `5818.06`, `p99 1.52ms`, mediana HTTP `0.62ms`, p90 `1.04ms`
  - run 6: `5832.20`, `p99 1.47ms`, mediana HTTP `0.57ms`, p90 `0.91ms`
  - run 7: `5840.23`, `p99 1.44ms`, mediana HTTP `0.56ms`, p90 `0.98ms`
  - run 8: `5812.62`, `p99 1.54ms`, mediana HTTP `0.54ms`, p90 `1.02ms`
  - run 9: `5839.25`, `p99 1.45ms`, mediana HTTP `0.60ms`, p90 `0.95ms`
  - run 10: `5826.56`, `p99 1.49ms`, mediana HTTP `0.64ms`, p90 `1.01ms`

Resumo 10x:

- média `final_score`: `5839.146`
- mediana superior `final_score`: `5839.25`
- mediana estatística `final_score`: `5835.725`
- pior `final_score`: `5812.62`
- melhor `final_score`: `5886.33`
- mediana superior `p99`: `1.47ms`
- mediana estatística `p99`: `1.46ms`
- pior `p99`: `1.54ms`
- melhor `p99`: `1.30ms`
- `detection_score`: `3000` em todas as rodadas
- `http_errors`: `0`
- `false_positive_detections`: `0`
- `false_negative_detections`: `0`
- `failure_rate`: `0.00%` em todas as rodadas

Comparação contra `5d3168e`:

- média `final_score`: `5839.146` contra `5694.739` (`+144.407`)
- mediana superior `final_score`: `5839.25` contra `5690.48` (`+148.77`)
- pior `final_score`: `5812.62` contra `5654.42` (`+158.20`)
- melhor `final_score`: `5886.33` contra `5806.97` (`+79.36`)
- mediana superior `p99`: `1.47ms` contra `2.09ms`
- pior `p99`: `1.54ms` contra `2.22ms`
- melhor `p99`: `1.30ms` contra `1.56ms`
- detecção permaneceu perfeita: `0` FP, `0` FN e `0` HTTP errors

Conclusão:

- esta foi a melhor melhoria sustentável do dia em C
- o ganho veio de reduzir o número de candidatos avaliados no hot path sem perder exatidão
- o p99 ainda não saturou em `1ms`, mas agora está consistentemente entre `1.30ms` e `1.54ms`
- o próximo ganho provável precisa atacar a cauda restante do p99, não a acurácia
- novas mudanças devem ser comparadas contra `3f49e35`, não mais contra `5d3168e`

## Estado final

Branch:

- `submission`

Commits publicados em `origin/submission` antes deste registro:

- `d3b6f1b` - `replace axum with manual http server`
- `bd17404` - `compile rust image for x86-64-v3`
- `14a7f04` - `add daily report for rust tuning`
- `cecde2b` - `update daily report with benchmark environment findings`
- `f0b28ed` - `document benchmark oom in daily report`
- `e8a899b` - `tune cpu split for capped runs`
- `363437f` - `align benchmark with official scoring`
- `4df089d` - `document language benchmark comparison`
- `dc1bd5d` - `document aggressive c optimization`
- `32e126d` - `document rejected c pgo experiments`
- `5a327af` - `document rejected c http experiment`

Commits publicados nas branches auxiliares:

- `submission-2`: `d245a39` - `add cpp stack and align benchmark`
- `submission-2`: `06535d7` - `harden cpp threshold and edge tests`
- `submission-c`: `925ff44` - `align benchmark with official scoring`
- `submission-c`: `5d3168e` - `optimize c parser and avx2 search`
- `submission-c`: `3f49e35` - `add exact grouped c search`

Alteração registrada nesta atualização:

- `DAILY_REPORT_2026-04-23.md`: registro da rodada C com busca exata agrupada

Estado frente ao remoto:

- branch deve ser publicada após este registro para manter o daily report sincronizado com o experimento C agrupado

Observação:

- o Git reportou aviso de `gc.log` e objetos soltos durante commits; isso é manutenção local do repositório e não altera a
  implementação nem os resultados de benchmark

## Próximo passo sugerido

Com o critério oficial atual, o próximo ganho sustentável deve mirar `p99` próximo de `1ms`, mantendo `E=0`. Depois de
`3f49e35`, o baseline competitivo passa a ser a busca exata agrupada em C, com média `5839.146` e p99 entre `1.30ms` e
`1.54ms` na bateria 10x local.

Ordem recomendada a partir daqui:

1. Estabilizar o ambiente local:
    - manter parados containers externos ao benchmark
    - aguardar o host sair do período pós-restart
    - evitar rodar com swap praticamente cheio
    - não misturar resultados de `desktop-linux` e `default` no mesmo A/B
2. Revalidar o baseline `3f49e35` antes de qualquer nova hipótese:
    - rodar `3x` no mesmo daemon escolhido
    - comparar contra mediana 3x `5849.17` e 10x médio `5839.146`
    - só aceitar conclusões se `0` `http_errors`, `failure_rate 0.00%` e estabilidade operacional forem observados
3. Se o baseline oficial atual ficar estável:
    - investigar apenas hipóteses que ataquem a cauda do p99
    - priorizar instrumentação leve de quantos grupos/candidatos são avaliados por request
    - manter cada ajuste isolado e reversível
    - comparar por A/B no mesmo daemon
4. Evitar repetir hipóteses já rejeitadas:
    - CPU split com nginx abaixo de `0.10`
    - PGO no Dockerfile
    - flags `-fomit-frame-pointer -funroll-loops`
    - fast-path HTTP com `memmem`
    - fast-path JSON dependente do formato serializado
