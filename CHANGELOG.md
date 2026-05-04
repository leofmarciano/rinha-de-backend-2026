# Changelog

Todas as mudancas relevantes deste projeto devem ser documentadas aqui.

Este historico simula a implementacao incremental do projeto do zero ate a base atual. As versoes
abaixo representam marcos logicos de construcao, mesmo quando varias mudancas tiverem sido feitas
no mesmo ciclo real de desenvolvimento.

## Unreleased

### Added

- Setup de qualidade com `clang-format`, `clang-tidy`, presets CMake e atalhos no `Makefile`.
- CI para build nativo, formatacao e lint.
- Documentacao operacional, release, contribuicao e politica de seguranca.

## 0.9.0 - Production Base

### Added

- `CMakePresets.json` com presets `release`, `debug`, `format`, `format-check`, `lint` e `check`.
- `Makefile` como entrada unica para build, lint, formatacao, geracao de indice e validacao.
- Workflow de CI para configurar, compilar, verificar formatacao e rodar `clang-tidy`.
- Dependabot para GitHub Actions.
- Template de pull request.
- `.gitattributes` para normalizacao de fim de linha e tratamento de artefatos binarios.
- `.env.example` com variaveis principais de runtime.
- `CONTRIBUTING.md`, `SECURITY.md`, `docs/RUNBOOK.md` e `docs/RELEASE.md`.

### Changed

- README passou a documentar o fluxo por `make` e presets CMake.
- `.gitignore` passou a cobrir builds debug, caches e artefatos de cobertura/profiling.

## 0.8.0 - Quality Gates

### Added

- Configuracao de `clang-format` para padronizar estilo C++.
- Configuracao de `clang-tidy` com checks de analyzer, bugprone, performance, portability,
  modernize e alguns readability.
- `.editorconfig` para manter charset, LF, newline final e indentacao consistente.
- Alvos CMake `format`, `format-check`, `lint` e `check`.
- Exportacao de `compile_commands.json` pelo CMake.

### Changed

- Fontes C++ formatados pelo `clang-format`.
- Correcoes acionadas pelo lint em parsing HTTP, calculo de taxa de mismatch e tratamento de
  excecoes no executavel da API.

## 0.7.0 - Local Runtime

### Added

- `Dockerfile` multi-stage para compilar o binario e montar uma imagem runtime enxuta.
- `docker-compose.yml` com HAProxy e duas replicas da API.
- Configuracao HAProxy em `deploy/haproxy.cfg` com health check em `/ready`, round-robin e limites
  de conexao.
- `.dockerignore` para reduzir contexto de build.
- Exposicao padrao da aplicacao em `:9999` via load balancer.

### Changed

- Compose passou a configurar `PORT`, `INDEX_PATH`, parametros de busca e numero de workers por
  ambiente.

## 0.6.0 - Validation Harness

### Added

- Executavel `validate-index` para comparar predicoes contra `test/test-data.json`.
- Suporte a `--base-nprobe`, `--ambig-nprobe`, `--flat-only` e `--limit` na validacao.
- Scripts k6 `test/smoke.js` e `test/test.js`.
- Dataset de validacao em `test/test-data.json`.
- Relatorio de metricas com total validado, mismatches, erros de parse, expansoes e uso de flat.

## 0.5.0 - Fraud API

### Added

- Servidor HTTP nativo em C++ com endpoint `GET /ready`.
- Endpoint `POST /fraud-score` retornando `approved` e `fraud_score`.
- Parser HTTP simples com leitura de headers, `Content-Length` e corpo.
- Pool de workers configuravel por variavel de ambiente.
- Respostas deterministicas para falhas recuperaveis de parsing/vetorizacao.
- Fechamento de conexao por resposta para reduzir retencao de workers em ramping local.

## 0.4.0 - ANN Search

### Added

- Estruturas de indice mmapado em `src/ann/index.h`.
- Busca IVF em `src/ann/ivf_scan.cc`.
- Fallback flat f16 em `src/ann/flat_scan.cc`.
- Utilitario `TopK` para selecao dos vizinhos mais proximos.
- Parametros `base_nprobe`, `ambig_nprobe` e warmup de indice.
- Fast-path para classificar casos obvios antes da busca vetorial.

## 0.3.0 - Index Builder

### Added

- Executavel `build-index` para gerar `build/fraud.ivf16` a partir de `resources/references.json.gz`.
- Formato binario do indice com header, checksum, dimensoes, offsets, centroides, vetores e labels.
- Agrupamento em listas IVF com `16384` listas.
- Persistencia de vetores normalizados em `f16` padded para 16 dimensoes.
- Compressao de labels em `u8`.

## 0.2.0 - Feature Pipeline

### Added

- Parser order-insensitive para payloads JSON de transacao.
- Vetorizacao de requisicoes em `src/vectorize/fraud_vector.*`.
- Parsing de timestamp ISO UTC em `src/util/time_parse.*`.
- Normalizacao baseada em `resources/normalization.json`.
- Lookup de risco MCC baseado em `resources/mcc_risk.json`.
- Tratamento explicito de `last_transaction: null` preservando sentinelas negativas.

## 0.1.0 - Native Scaffold

### Added

- Projeto C++20 com CMake.
- Biblioteca `rinha_core` compartilhando codigo entre executaveis.
- Executaveis `fraud-server`, `build-index` e `validate-index`.
- Estrutura inicial de diretorios `cmd/`, `src/`, `resources/`, `deploy/` e `test/`.
- README inicial com arquitetura, build local, Docker e decisoes tecnicas.
- `info.json` com metadados do projeto.
- `.gitignore` inicial para builds locais, indice gerado, resultados de teste e arquivos de sistema.
