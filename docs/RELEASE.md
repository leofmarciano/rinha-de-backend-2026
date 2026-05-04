# Release

## Checklist

1. Rode a suite local:

   ```bash
   make check
   make validate
   ```

2. Atualize `CHANGELOG.md`, movendo itens de `Unreleased` para a versao nova.
3. Gere ou valide o indice esperado:

   ```bash
   make index
   shasum -a 256 build/fraud.ivf16
   ```

4. Crie uma tag anotada:

   ```bash
   git tag -a vX.Y.Z -m "vX.Y.Z"
   git push origin vX.Y.Z
   ```

## Versionamento

Use `vMAJOR.MINOR.PATCH` quando o projeto passar a publicar versoes formais.

- `MAJOR`: mudanca incompativel no contrato HTTP, formato do indice ou requisitos de runtime.
- `MINOR`: melhoria de performance, observabilidade ou operacao sem quebra de contrato.
- `PATCH`: correcao pontual, ajuste de build, docs ou hardening sem mudanca funcional.
