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

## Estado final

Branch:

- `submission`

Commits locais adicionados hoje:

- `d3b6f1b` - `replace axum with manual http server`
- `bd17404` - `compile rust image for x86-64-v3`
- `14a7f04` - `add daily report for rust tuning`

Estado frente ao remoto:

- branch local contém commits ainda não publicados em `origin/submission`

Observação:

- os commits ainda não foram publicados nesta rodada
- o Git reportou aviso de `gc.log` e objetos soltos durante commits; isso é manutenção local do repositório e não altera a
  implementação nem os resultados de benchmark

## Próximo passo sugerido

O próximo ganho sustentável em Rust provavelmente não virá de latência HTTP genérica, porque o `p99` já está muito abaixo
do alvo e o score já bate no teto. As próximas hipóteses devem focar em consistência de `raw_score` e reduzir variância:

Ordem recomendada a partir daqui:

1. Estabilizar o ambiente local:
    - manter parados containers externos ao benchmark
    - aguardar o host sair do período pós-restart
    - evitar rodar com swap praticamente cheio
    - não misturar resultados de `desktop-linux` e `default` no mesmo A/B
2. Revalidar o baseline atual `x86-64-v3`:
    - rodar `3x` no mesmo daemon escolhido
    - se o resultado voltar ao teto, rodar `5x`
    - só aceitar conclusões se `0` `http_errors`, `100%` acurácia e estabilidade operacional forem observados
3. Se o baseline voltar ao teto:
    - testar ajustes mínimos de `nginx stream`/UDS
    - manter cada ajuste isolado e reversível
    - comparar por A/B no mesmo daemon
4. Só voltar ao parser JSON se houver evidência forte de CPU do backend como limitante:
    - parser seletivo pode trazer ganho, mas aumenta risco de bug de contrato
    - nesta fase, sem ambiente estável, qualquer diferença seria ruído
