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

Image:
```text
ghcr.io/leofmarciano/rinha-de-backend-2026:submission-amd64
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

## Benchmark

A pipeline `bench` em [`.github/workflows/bench.yml`](.github/workflows/bench.yml) sobe o stack
docker-compose, executa `k6 run test/smoke.js` e `k6 run test/test.js`, salva
`test/results.json` e atualiza o bloco abaixo a cada push em `main` que toque código ou
infraestrutura. Como o runner do GitHub Actions é compartilhado, o p99 daqui não é diretamente
comparável ao da máquina local — sirva como termo de regressão, não de score absoluto.

<!-- BENCH-RESULTS:START -->
**Última execução:** commit `6e6f6b7` em `2026-05-04 04:33 UTC`. [Workflow run](https://github.com/leofmarciano/rinha-de-backend-2026/actions/runs/25301081000).

| Métrica | Valor |
|---------|-------|
| p99 latência | `0.75ms` |
| Score final | `5540.56` |
| Score p99 | `3000` |
| Score detecção | `2540.56` |
| ε (erro ponderado) | `0.00061` |
| TP / TN | `24029` / `30013` |
| FP / FN | `9` / `8` |
| HTTP errors | `0` |
| Failure rate | `0.03%` |

<details><summary>Resultado bruto (<code>test/results.json</code>)</summary>

```json
{
  "expected": {
    "total": 54100,
    "fraud_count": 24058,
    "legit_count": 30042,
    "fraud_rate": 0.4447,
    "legit_rate": 0.5553,
    "edge_case_count": 797,
    "edge_case_rate": 0.0147
  },
  "p99": "0.75ms",
  "scoring": {
    "breakdown": {
      "false_positive_detections": 9,
      "false_negative_detections": 8,
      "true_positive_detections": 24029,
      "true_negative_detections": 30013,
      "http_errors": 0
    },
    "failure_rate": "0.03%",
    "weighted_errors_E": 33,
    "error_rate_epsilon": 0.00061,
    "p99_score": {
      "value": 3000,
      "cut_triggered": false
    },
    "detection_score": {
      "value": 2540.56,
      "rate_component": 3000,
      "absolute_penalty": -459.44,
      "cut_triggered": false
    },
    "final_score": 5540.56
  }
}
```

</details>
<!-- BENCH-RESULTS:END -->

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
- O compose inicial usa `lb=0.04 CPU/24MB` e `api=0.48 CPU/163MB` por réplica.
- A auditoria clean-room do líder está em `docs/leader-study.md`; o código AGPL não foi copiado.

## Alternativas Rejeitadas

- `Flat f32`: exato, mas memória e p99 ruins.
- `Flat f16`: bom fallback, lento como caminho principal.
- `HNSWFlat M=16`: não cabe em 350MB com duas réplicas.
- `IVF q8` e `IVFPQ`: rápidos, mas mais arriscados para score de detecção.
