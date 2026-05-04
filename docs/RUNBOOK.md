# Runbook

## Build

```bash
make build
```

Equivalente:

```bash
cmake --preset release
cmake --build --preset release
```

## Qualidade

```bash
make check
```

Esse alvo compila o projeto e executa `format-check` e `lint`.

Para aplicar formatacao:

```bash
make format
```

## Indice

Gerar o indice padrao:

```bash
make index
```

O arquivo esperado e `build/index_k8192.ivfi16`.

Validar uma amostra:

```bash
make validate
```

Controlar tamanho da amostra:

```bash
VALIDATE_LIMIT=10000 make validate
```

## Variaveis de runtime

| Variavel | Default local | Uso |
| --- | --- | --- |
| `PORT` | `8080` | Porta HTTP da API |
| `INDEX_PATH` | `build/index_k8192.ivfi16` | Caminho do indice |
| `BASE_NPROBE` | `20` no compose | Busca IVF base |
| `AMBIG_NPROBE` | `40` no compose | Busca IVF para casos ambiguos |
| `BBOX_MODE` | `ambiguous-only` | `off`, `ambiguous-only` ou `always` |
| `USE_EXACT_FALLBACK` | `0` | Ativa flat exato em caso fragil |
| `USE_FAST_PATH` | `0` | Ativa heuristica de casos obvios |
| `WORKERS` | `1` no binario | Workers HTTP |

## Diagnostico rapido

Build falhando por Zlib:

```bash
brew install zlib
# ou
sudo apt-get install zlib1g-dev
```

Lint nao encontrado:

```bash
brew install llvm
```

O CMake procura LLVM tambem em `/opt/homebrew/opt/llvm/bin` e
`/usr/local/opt/llvm/bin`.

Indice ausente:

```bash
make index
```
