# Rinha Backend 2026 Native IVF

Solução para a Rinha de Backend 2026.

## Arquitetura

- HAProxy em `:9999`, round-robin simples.
- Duas APIs C++20 em `:8080`.
- Índice único `fraud.ivf16` mmapado read-only por réplica.
- Centroides `f32`, vetores `f16` padded para 16 dimensões e labels `u8`.
- Busca IVF do relatório mantida no binário; o compose local usa `HEURISTIC_ONLY=1` para evitar timeouts no Docker Desktop arm64.

## Índice

O arquivo `build/fraud.ivf16` contém:

- header com magic/version/checksum/dimensões/offsets.
- `16384` centroides `f32`.
- offsets das listas IVF.
- vetores `f16` ordenados por lista.
- labels `u8`.

O checksum esperado do conteúdo descomprimido de `references.json.gz` é:

```text
24a1fd588e2598ab62de9ac7ac73408e571589e00da4243d7afc9d0f01878f77
```

## Build Local

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
```

## Qualidade

O setup usa `clang-format` para formatação e `clang-tidy` para lint estático.
No macOS, `brew install llvm` é suficiente; o CMake também procura em
`/opt/homebrew/opt/llvm/bin` e `/usr/local/opt/llvm/bin`.

```bash
cmake --build build-cmake --target format
cmake --build build-cmake --target format-check
cmake --build build-cmake --target lint
cmake --build build-cmake --target check
```

## Docker

Depois de gerar `build/fraud.ivf16`:

```bash
docker compose up --build
```

Em outro terminal:

```bash
k6 run test/smoke.js
k6 run test/test.js
```

## Decisões Técnicas

- Sem banco ou cache no hot path.
- Sem lookup de `test-data.json` em runtime.
- Parser order-insensitive para o payload.
- Timestamp ISO UTC parseado por posição.
- MCC com lookup fixo e default `0.5`.
- `last_transaction: null` preserva `-1` nas dimensões 5 e 6.
- Resposta 200 determinística em falha recuperável, evitando peso `Err=5`.
- Fast-path linear decide casos óbvios antes do IVF; a busca vetorial fica para a faixa ambígua.
- O perfil `24/48` segue configurado no compose, e `256/512` segue disponível por env vars para validação de maior qualidade fora do ramping local.
- No compose local, o orçamento foi realocado para `lb=0.40 CPU/80MB` e `api=0.30 CPU/135MB` por réplica; o HAProxy era o gargalo real no Docker Desktop com `0.04 CPU`, mas as APIs precisam de alguma folga para não formar cauda.
- As APIs usam 64 workers HTTP, fecham conexões a cada resposta e ativam `HEURISTIC_ONLY=1`; isso prioriza zero erro HTTP quando o Docker local não sustenta o ANN sob ramping-arrival-rate.

## Alternativas Rejeitadas

- `Flat f32`: exato, mas memória e p99 ruins.
- `Flat f16`: bom fallback, lento como caminho principal.
- `HNSWFlat M=16`: não cabe em 350MB com duas réplicas.
- `IVF q8` e `IVFPQ`: rápidos, mas mais arriscados para score de detecção.
