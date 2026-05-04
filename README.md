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

## Decisões Técnicas

- Sem banco ou cache no hot path.
- Sem lookup de `test-data.json` em runtime.
- Parser order-insensitive para o payload.
- Timestamp ISO UTC parseado por posição.
- MCC com lookup fixo e default `0.5`.
- `last_transaction: null` preserva `-1` nas dimensões 5 e 6.
- Resposta 200 determinística em falha recuperável, evitando peso `Err=5`.
- Fast-path linear decide casos óbvios antes do IVF; a busca vetorial fica para a faixa ambígua.

## Alternativas Rejeitadas

- `Flat f32`: exato, mas memória e p99 ruins.
- `Flat f16`: bom fallback, lento como caminho principal.
- `HNSWFlat M=16`: não cabe em 350MB com duas réplicas.
- `IVF q8` e `IVFPQ`: rápidos, mas mais arriscados para score de detecção.
