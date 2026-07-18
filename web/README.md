# Arbiter Web

Static marketing and documentation site for Arbiter.

The build reads Markdown from `../docs` by default and emits plain HTML to
`dist/`. Override the docs source with `ARBITER_DOCS_PATH=/path/to/docs` when
building from another checkout layout.

```bash
npm run build
npm run serve
```

The site intentionally avoids a client framework. Moving parts:

- `scripts/build.mjs` — discovers docs, derives navigation, renders Markdown
- `src/styles.css` — visual system
- `src/site.js` — install copy button and section reveal

`install.sh` is copied to `dist/install.sh` during the build so the homepage can
advertise:

```bash
curl -fsSL https://arbiter.run/install.sh | sh
```
