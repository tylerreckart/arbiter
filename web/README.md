# Arbiter Web

Static marketing and documentation site for Arbiter.

The build reads Markdown from `../docs` by default and emits plain HTML to
`dist/`. Override the docs source with `ARBITER_DOCS_PATH=/path/to/docs` when
building from another checkout layout.

```bash
npm install
npm run build
npm run serve
```

## Layout

| Path | Role |
|------|------|
| `scripts/build.mjs` | Build orchestrator |
| `lib/` | Docs discovery, Markdown, navigation models, assets |
| `views/` | Pug templates (`layout`, pages, includes) |
| `src/styles.css` | Visual system |
| `src/site.js` | Install copy button and section reveal |

`install.sh` is copied to `dist/install.sh` during the build so the homepage can
advertise:

```bash
curl -fsSL https://arbiter.run/install.sh | sh
```
