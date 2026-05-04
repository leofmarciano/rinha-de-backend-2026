# Rinha Backend 2026 Native IVF

Solução para a Rinha de Backend 2026.

## Arquitetura

- HAProxy em `:9999`, round-robin simples.
- Duas APIs C++20 em `:8080`.
- Índice único `index_k8192.ivfi16` mmapado read-only por réplica.
- Centroides `f32`, vetores `int16` em layout SoA, bbox, labels `u8` e `orig_ids u32`.
- Busca IVF i16 com top-6 determinístico, bbox repair e fallback exato opcional.

## Índice

O arquivo `build/index_k8192.ivfi16` contém:

- header com magic/version/checksum/dimensões/offsets.
- `nlist` centroides `f32`.
- bbox mínima/máxima por lista em `int16`.
- offsets das listas IVF.
- vetores `int16` em SoA, escala `10000`.
- labels `u8`.
- ids originais `u32` para desempate determinístico.

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

Depois de gerar `build/index_k8192.ivfi16`:

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
- Fast-path linear existe apenas como opção (`USE_FAST_PATH=1`), desligado por padrão para priorizar qualidade.
- O perfil padrão é `K=8192`, `BASE_NPROBE=20`, `AMBIG_NPROBE=40`, `BBOX_MODE=ambiguous-only`.
- O compose inicial usa `lb=0.10 CPU/24MB` e `api=0.45 CPU/163MB` por réplica.
- A auditoria clean-room do líder está em `docs/leader-study.md`; o código AGPL não foi copiado.

## Alternativas Rejeitadas

- `Flat f32`: exato, mas memória e p99 ruins.
- `Flat f16`: bom fallback, lento como caminho principal.
- `HNSWFlat M=16`: não cabe em 350MB com duas réplicas.
- `IVF q8` e `IVFPQ`: rápidos, mas mais arriscados para score de detecção.
