# Hub

API + painel de donos da plataforma. Detalhes: `docs/superpowers/specs/2026-11-09-plataforma-design.md`.

## Rodar local sem Discord

```bash
npm install
HUB_DEV_LOGIN=1 npm start -w @cbmp/hub
```
Abra http://localhost:8787/panel → "Entrar como Dev". (PowerShell: `$env:HUB_DEV_LOGIN='1'; npm start -w @cbmp/hub`)

## Rodar com Discord

1. Crie um app em https://discord.com/developers/applications → OAuth2.
2. Adicione o redirect `<HUB_PUBLIC_URL>/v1/auth/discord/callback`.
3. Copie `.env.example` para `.env`, preencha `DISCORD_CLIENT_ID`/`DISCORD_CLIENT_SECRET` e exporte as variáveis antes de `npm start -w @cbmp/hub`.

## Variáveis de ambiente

Veja `.env.example` para a lista completa. Updates do launcher, mod e downloads (`/v1/launcher/update/...`, `/v1/mod/latest`, `/v1/dl/...`):

| Variável | Uso |
|---|---|
| `HUB_GITHUB_TOKEN` | token do GitHub com leitura de Releases (fine-grained, só *Contents: read* no repo privado). Vazio → as três rotas respondem `503 updates_disabled`. Nunca vai para log nem para o cliente. |
| `HUB_RELEASES_REPO` | `dono/repo` dos Releases; obrigatório quando há token. Tags `launcher-vX.Y.Z` (instalador `*-setup.exe` + `.sig`) e `mod-pN-vX.Y.Z` (`cbmp-mod-pN-vX.Y.Z.zip` + `.sig` + `.sha256`). |

A lista de releases fica 60 s em cache. Drafts e prereleases são ignorados. O Hub não confere assinaturas: o launcher confere.

## Chaves de assinatura

Ficam em `<HUB_DATA_DIR>/keys/<kid>.pem`. A de maior nome é a ativa. Para girar: crie um novo `.pem` com kid maior
(`k` + AAAAMMDDhhmmss) e reinicie; remova a antiga depois de 10 minutos (tickets duram 5 min).

## Testes

```bash
npm test
```
