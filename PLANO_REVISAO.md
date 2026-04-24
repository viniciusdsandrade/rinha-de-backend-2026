# Plano de Revisão — Rinha de Backend 2026

**Branch avaliada:** `submission-2` (HEAD `d245a39`)
**Stack:** C++20 — uWebSockets + simdjson + AVX2/FMA; nginx `stream` (L4) via unix socket; duas APIs atrás do LB; pré-processamento do dataset no build (`prepare-refs-cpp`).

Este documento **não executa mudanças**. Serve apenas como blueprint de revisão,
ordenado por retorno esperado.

---

## 1. Veredito geral

O código está alinhado com o regulamento oficial em todas as frentes normativas
(contrato da API, 14 dimensões, constantes, k=5, threshold 0.6, topologia,
limites de recursos, porta 9999, bridge, não-privileged, imagens públicas,
LB sem lógica de negócio). Os ajustes propostos abaixo mitigam riscos laterais
(portabilidade de CPU, margem de recursos, ambiguidade float) e ampliam a
cobertura de testes antes da próxima submissão.

---

## 2. Checklist de conformidade

| Eixo | Status | Evidência |
|---|---|---|
| `GET /ready` responde HTTP 2xx (204) | atende | `cpp/src/main.cpp:162-165` |
| `POST /fraud-score` devolve `{approved, fraud_score}` | atende | `cpp/src/main.cpp:167-199` |
| Porta 9999 no LB | atende | `docker-compose.yml:30`, `nginx.conf:16` |
| Exatamente dois endpoints expostos | atende | outros métodos caem no 404 do uWS |
| 14 dimensões na ordem correta | atende | `cpp/src/vectorize.cpp:181-197` |
| Constantes batem com `resources/normalization.json` | atende | `kMaxAmount=10000`, `kMaxMinutes=1440`, etc. |
| Tabela MCC + default 0.5 | atende | `vectorize.cpp:142-154` |
| k=5 + threshold 0.6 (invertido) | atende | `classifier.cpp:40-41` |
| Sentinela -1 em idx 5/6 quando `last_transaction:null` | atende | preservado |
| Distância euclidiana (quadrado, sem `sqrt`) | atende | `refs.cpp:286-302` — ordenação idêntica |
| Topologia LB + 2 APIs | atende | `docker-compose.yml` |
| Soma ≤ 1 CPU / 350 MB | atende (no teto) | `0.10 + 0.45 + 0.45` / `20 + 165 + 165` |
| Network `bridge` + não-privileged | atende | default do compose |
| LB sem lógica de negócio | atende | nginx em `stream` (TCP L4); não parseia HTTP |
| Imagens públicas / `linux-amd64` | atende | `debian:bookworm-slim`, `nginx:1.27-alpine` |

---

## 3. Ações — ordenadas por retorno esperado

### Bloco A — Bloqueantes

#### A1. Promover `submission-2` para `submission`
**Motivo.** O regulamento (`docs/br/SUBMISSAO.md`) exige branch literal `submission`.
O remote hoje tem três branches candidatas:
- `submission` (`4df089d`)
- `submission-2` (`d245a39`) — versão atual, mais recente
- `submission-c` (`925ff44`)

Se o harness só lê `submission`, a versão com o stack C++ não é avaliada.

**Ação sugerida.** Confirmar com a organização qual branch é lida, ou
renomear/force-pushear `submission-2` sobre `submission` (com backup antes).

#### A2. Isolar AVX2/FMA só em `top5_avx2`
**Motivo.** `cpp/CMakeLists.txt:85,118,155` injeta
`-mavx2 -mfma -march=x86-64-v3` no **binário inteiro**. Em host sem AVX2, o
binário aborta com `SIGILL` antes mesmo do runtime check em
`Classifier::supports_avx2()` (`classifier.cpp:45-55`) rodar.

**Ação sugerida.** Rebaixar baseline para `-march=x86-64-v2` (ou remover `-march`
global) e aplicar `__attribute__((target("avx2,fma")))` apenas em
`classifier.cpp::top5_avx2`:

```cpp
__attribute__((target("avx2,fma")))
Classifier::Top5 Classifier::top5_avx2(const QueryVector& query) const noexcept {
    // ...
}
```

O fallback escalar (`top5_scalar`) permanece como caminho seguro.

---

### Bloco B — Correção fina

#### B1. Threshold por inteiro em vez de comparação float
**Motivo.** `classifier.cpp:41`: `approved = fraud_score < 0.6f`.
Com `fraud_score = fraud_count / 5.0f` e `fraud_count = 3`,
`3.0f/5.0f` e `0.6f` colapsam para o mesmo bit pattern IEEE-754,
então a comparação atualmente dá `false` (negado) — correto pelo spec.
Porém, qualquer contração FMA ou mudança de ABI pode alterar 1 ULP
e inverter o resultado em casos de fronteira.

**Ação sugerida.**

```cpp
classification.approved = fraud_count < 3;
classification.fraud_score = static_cast<float>(fraud_count) * 0.2f;
```

Comportamento observável idêntico ao regulamento, sem ambiguidade.

#### B2. Folga no orçamento de recursos
**Motivo.** `docker-compose.yml` está cravado em 1.00 CPU / 350 MB
(teto exato do regulamento). Sob ramp 350 → 650 RPS no k6, qualquer
burst transitório de heap/stack pode disparar OOM killer, e a soma
de `0.45 + 0.45 + 0.10` sofre throttling quando o scheduler vê spike.

**Ação sugerida.** Deixar ~4% de folga:

```yaml
api1/api2:  cpus: "0.43"  memory: "160MB"
nginx:      cpus: "0.10"  memory: "20MB"
# Total:    0.96 CPU / 340 MB
```

O dataset decodificado ocupa `100k × 14 × 4 bytes = 5.6 MB` por réplica.
Cabe com folga dentro de 160 MB.

---

### Bloco C — Confiança pré-submissão

#### C1. Expandir `core_tests.cpp`
**Motivo.** `cpp/tests/core_tests.cpp` cobre apenas 3 casos
(parse null, vectorize meia-noite, smoke classifier). Casos de borda
conhecidos por enviesarem submissões:

- MCC **não** presente na tabela (default 0.5 — `vectorize.cpp:153`).
- `transaction.amount > 10000` (clamp em 1.0).
- `customer.avg_amount = 0` (divisão defendida em `amount_vs_avg`).
- `transaction.installments > 12` (clamp em 1.0).
- Fevereiro em ano bissexto vs. não-bissexto (`days_in_month`).
- `customer.known_merchants = []` → `unknown_merchant = 1`.
- Duplicatas em `known_merchants` — verificação com `find` cobre, mas testar.
- Duas transações com `last_transaction:null` classificando-se juntas
  (sentinela `-1` aproxima vetores sem histórico — essencial para precisão).

#### C2. Baseline de pontuação com `./run.sh`
**Motivo.** Sem número base, otimização é achismo.

**Ação sugerida.** Rodar `./run.sh` no estado atual, salvar `results.json`
como baseline (p99_score, detection_score, final_score). Repetir após
cada bloco de mudança (A, B, C). Observar especialmente:
- `failure_rate` não pode cruzar 15% (corte rígido `-3000`).
- `p99` abaixo de 1 ms satura `p99_score` em `+3000`.

---

### Bloco D — Opcional (baixo retorno)

- **D1.** `nginx.conf:18` — reduzir `proxy_timeout 30s` para `5s`. Cosmético;
  k6 já tem timeout próprio.
- **D2.** `aligned_alloc` + `_mm256_load_ps` (alinhado) para `dims_`. Ganho
  marginal vs. `_mm256_loadu_ps` atual.
- **D3.** Comentário em `main.cpp:190-192` documentando a estratégia de
  fallback silencioso (`approved:true, fraud_score:0.0`) como decisão
  consciente alinhada ao `AVALIACAO.md` (evitar HTTP 500 peso 5).

---

## 4. O que NÃO tocar — já está bem feito

- **Quantização da resposta em 6 strings constantes** (`main.cpp:106-133`):
  elimina `snprintf`/`std::to_string` do hot path. Válido porque k=5 só
  produz `{0.0, 0.2, 0.4, 0.6, 0.8, 1.0}`.
- **Layout SoA + ordenação por variância decrescente** (`refs.cpp:304-341`):
  combina bem com early-exit acumulativo e prune AVX2.
- **Pré-processamento no build** (`Dockerfile:16-22`): converte
  `references.json.gz` → `references.bin` / `labels.bin`. Startup lê binário
  direto, sem descompactação nem parse JSON.
- **Unix socket nginx <-> API** (`nginx.conf:11-12`, `docker-compose.yml:8`):
  elimina overhead de TCP loopback.
- **Distância ao quadrado** (`refs.cpp:286-302`): ordenação idêntica à
  euclidiana, 1 ns a menos por candidato.
- **Fallback silencioso no classifier** (`main.cpp:190-192`): retornar
  `approved:true, fraud_score:0.0` em erro custa menos pontos (FP peso 1
  ou FN peso 3) do que HTTP 5xx (Err peso 5 + entra na failure_rate).
- **Contrato da API** e **topologia**: atendem literal ao regulamento.

---

## 5. Riscos residuais pós-plano

1. **Stages do `test.js` local != harness oficial.** O regulamento avisa que
   a versão final de avaliação pode diferir. O `run.sh` local é proxy, não
   garantia.
2. **Teto de memória justo mesmo com 4% de folga.** Um pico de heap no
   simdjson sob carga longa pode empurrar OOM. Se observar degradação,
   considerar `MALLOC_TRIM_THRESHOLD_` ou `jemalloc`.
3. **Dataset rotulado com k=5/euclidiana.** Como o código usa exatamente a
   mesma métrica, o teto teórico de acurácia é alto. Qualquer desvio
   significativo indica bug de normalização, não escolha de algoritmo.

---

## 6. Referências cruzadas (arquivos consultados)

- Especificação oficial: `docs/br/API.md`, `docs/br/ARQUITETURA.md`,
  `docs/br/AVALIACAO.md`, `docs/br/REGRAS_DE_DETECCAO.md`,
  `docs/br/BUSCA_VETORIAL.md`, `docs/br/DATASET.md`, `docs/br/SUBMISSAO.md`.
- Implementação: `cpp/CMakeLists.txt`, `cpp/src/{main,request,vectorize,classifier,refs,prepare_refs}.cpp`,
  `cpp/include/rinha/{types,request,vectorize,classifier,refs}.hpp`.
- Testes: `cpp/tests/core_tests.cpp`, `test/test.js`.
- Infra: `Dockerfile`, `docker-compose.yml`, `nginx.conf`, `run.sh`.
- Recursos: `resources/normalization.json`, `resources/mcc_risk.json`,
  `resources/references.json.gz`.
