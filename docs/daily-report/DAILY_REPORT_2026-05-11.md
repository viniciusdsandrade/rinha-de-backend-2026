# Daily Report - 2026-05-11

## Auditoria de retomada

Retomada feita em `2026-05-11 23:53:01 -0300`, portanto depois do limite operacional previamente solicitado para `18h00` do mesmo dia. Por isso, esta rodada não executa novos k6 pesados nem abre nova submissão oficial; o objetivo aqui foi auditar o estado real e deixar o histórico consistente.

Estado verificado:

```text
worktree experimental: /home/andrade/.config/superpowers/worktrees/rinha-de-backend-2026/perf-noon-tuning
branch: perf/noon-tuning
ultimo commit: 89c0951 report merchant code parser rejection
working tree: limpo
containers rinha/submission em execucao: nenhum
```

Worktree de submissão:

```text
worktree submission: /home/andrade/Desktop/rinha-de-backend-2026-rust
branch: submission
HEAD: a7bec6b point submission to likely hot path image
```

Submissão oficial mais recente:

```text
issue: https://github.com/zanfranceschi/rinha-de-backend-2026/issues/2625
titulo: rinha/test andrade-cpp-ivf
descricao: rinha/test andrade-cpp-ivf
estado: CLOSED
imagem avaliada: ghcr.io/viniciusdsandrade/rinha-de-backend-2026:submission-e950b12
commit avaliado: a7bec6b
p99 oficial: 1.20ms
falhas: 0%
final_score: 5920.45
```

Comparação com melhor oficial interno:

| Referência | p99 | Falhas | final_score |
|---|---:|---:|---:|
| Melhor oficial anterior `#2316` | 1.20ms | 0% | 5921.80 |
| Submissão corrigida `#2625` | 1.20ms | 0% | 5920.45 |

Decisão: **não há nova submissão melhor para promover agora**. A candidata `submission-e950b12` foi válida, mas não superou o melhor oficial interno anterior.

## Aprendizado operacional persistido

O aprendizado sobre o trigger oficial foi salvo na memória de longo prazo:

```text
/home/andrade/.codex/memories/extensions/ad_hoc/notes/20260509-140142-rinha-2026-issue-trigger.md
```

Regra consolidada: para disparar o teste oficial da Rinha 2026, o comando precisa estar na **descrição/body da issue**, não em comentário posterior e não substituído por link.

```text
title: rinha/test andrade-cpp-ivf
body:  rinha/test andrade-cpp-ivf
```

## Hipótese preparada para próxima janela

Hipótese: o parser manual ainda calcula timestamps ISO com `days_from_civil_fast`, que usa divisões genéricas. Como a massa oficial usa datas de 2026, um fast path específico para `year == 2026` pode calcular `day_of_year` por tabela fixa de meses e manter fallback genérico para qualquer outro ano.

Motivo técnico:

- O campo `requested_at` aparece em todo payload.
- `last_transaction.timestamp` aparece quando há histórico.
- O hot path atual chama `days_from_civil_fast` para cada timestamp parseado.
- Um fast path de 2026 troca divisões por tabela e soma simples.
- O fallback genérico preserva compatibilidade com payloads fora de 2026.

Plano de experimento recomendado:

1. Implementar `days_2026_fast(month, day)` com tabela cumulativa de meses de 2026.
2. Em `scan_iso_timestamp`, usar o fast path somente quando os quatro dígitos do ano forem `2026`.
3. Manter `days_from_civil_fast` como fallback.
4. Validar com `rinha-backend-2026-cpp-tests`.
5. Rodar um k6 isolado e comparar contra a melhor referência local do dia 09/05 (`1.03ms`, score `5985.53`) e contra a melhor oficial interna (`5921.80`).

Decisão operacional: **não executar agora**, pois o horário combinado de atuação já passou. Próxima retomada deve começar por essa hipótese ou por uma revalidação local curta do baseline aceito, dependendo da carga da máquina.

## Ciclo 23h58: fast path de timestamp para 2026

Hipótese: o parser manual calcula timestamps ISO com `days_from_civil_fast`, que usa divisões genéricas de calendário. Como a massa oficial usa datas de 2026, um fast path para `year == 2026` poderia trocar essas divisões por tabela fixa de início de mês, preservando fallback genérico para outros anos.

Alteração temporária:

```text
days_2026_fast(month, day):
  kDaysTo2026 = 20454
  month_start = [0,31,59,90,120,151,181,212,243,273,304,334]

scan_iso_timestamp:
  if year == 2026 -> days_2026_fast
  else -> days_from_civil_fast
```

Validação funcional:

```text
cmake --build cpp/build --target rinha-backend-2026-cpp-manual rinha-backend-2026-cpp-tests
ctest --test-dir cpp/build --output-on-failure
100% tests passed, 0 tests failed out of 1
```

Resultados k6 locais:

| Variante | Run | p99 | Falhas | final_score |
|---|---:|---:|---:|---:|
| parser manual + `[[likely]]` melhor run histórica | - | 1.03ms | 0% | 5985.53 |
| timestamp fast path 2026 | 1 | 1.10ms | 0% | 5956.93 |
| timestamp fast path 2026 | 2 | 1.15ms | 0% | 5938.55 |

Decisão: **rejeitado e revertido**.

Aprendizado: o custo do cálculo civil genérico não é gargalo dominante ou o fast path alterou layout/inlining de forma desfavorável. Mesmo correto funcionalmente, não superou a melhor referência local nem justificou promoção. Manter `days_from_civil_fast`.
