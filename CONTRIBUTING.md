# Contribuindo

Este projeto usa C++20 com CMake. A entrada recomendada para desenvolvimento local e CI e via
presets do CMake ou pelos atalhos do `Makefile`.

## Ambiente

Dependencias locais:

- compilador C++20
- CMake 3.22+
- Zlib
- LLVM com `clang-format` e `clang-tidy`

No macOS:

```bash
brew install cmake llvm zlib
```

No Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake zlib1g-dev clang-format clang-tidy
```

## Fluxo

```bash
make build
make check
```

Ou diretamente com CMake:

```bash
cmake --preset release
cmake --build --preset release
cmake --build --preset check
```

Antes de abrir PR, rode:

```bash
make format-check
make lint
make build
```

Use `make format` para aplicar a formatacao automaticamente.

## Indice e validacao

```bash
make index
make validate
```

Variaveis uteis:

```bash
VALIDATE_LIMIT=10000 make validate
INDEX=build/custom.ivf16 make index
```

## Escopo de mudancas

- Mantenha mudancas pequenas e focadas.
- Evite refatorar caminhos quentes sem medir impacto.
- Preserve os contratos HTTP e o formato do indice.
- Atualize o README ou o runbook quando alterar comandos operacionais.
