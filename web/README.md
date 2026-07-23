# Arbiter Web

Static marketing and documentation site for Arbiter.

The build reads Markdown from `../docs` by default and emits plain HTML to
`dist/`. Override the docs source with `ARBITER_DOCS_PATH=/path/to/docs` when
building from another checkout layout.

```bash
npm install
npm run dev
```

`npm run dev` performs an initial build, serves `dist/` at
`http://localhost:4173`, watches Pug templates, build modules, styles,
client-side scripts, docs, installer, and image assets, then rebuilds and
reloads connected browsers after each successful change. Development builds
are staged separately, so a template or build error leaves the last good site
available while you fix it. Set `PORT=4174` to use another port.

For a production-style static build:

```bash
npm run build
npm run serve
```

## Layout

| Path | Role |
|------|------|
| `scripts/build.mjs` | Build orchestrator |
| `scripts/dev.mjs` | Watch, rebuild, and live-reload server |
| `lib/` | Docs discovery, Markdown, navigation models, assets |
| `views/` | Pug templates (`layout`, pages, includes) |
| `src/styles/` | Visual system (`tokens`, `base`, `chrome`, `marketing`, `docs`, `footer`, `responsive`) |
| `src/site.js` | Install copy button + reveal motion |

`install.sh` is copied to `dist/install.sh` during the build so the homepage can
advertise:

```bash
curl -fsSL https://arbiter.run/install.sh | sh
```
