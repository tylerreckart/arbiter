# `arbiter --init`

Initialize the per-user config directory and seed it with example agent constitutions. By default, **existing files are preserved** — re-running `--init` is safe and won't clobber a JSON you've edited.

```
arbiter --init             # write only the JSON files that don't exist yet
arbiter --init --force     # overwrite every starter back to the embedded default
```

## What gets created

```
~/.arbiter/
├── agents/
│   ├── vera.json       Vera — code review (Claude Opus 5, defect-first)
│   ├── scout.json      Scout — research (Gemini 3.6 Flash + Opus advisor)
│   ├── quill.json      Quill — essays, READMEs, docs (Claude Sonnet 5)
│   ├── forge.json      Forge — infra/CI (GPT-5.6 Sol + Opus advisor)
│   ├── compass.json    Compass — task decomposition (GPT-5.5 + Opus advisor)
│   ├── nexus.json      Nexus — APIs/data (Claude Sonnet 5 + Opus advisor)
│   ├── loom.json       Loom — UI/a11y (Claude Sonnet 5 + Opus advisor)
│   ├── beacon.json     Beacon — strategy/campaigns (GPT-5.5)
│   └── echo.json       Echo — platform-native content (Grok 4.5)
```

The master orchestrator (`index`) is not written to disk — it ships as a compiled-in constitution and is loaded automatically. Only sub-agent definitions live in `~/.arbiter/agents/`.

Starter agents use personal callsigns (`scout`, `vera`, `quill`, …) as their ids — related to their function, but meant to feel like a roster of people rather than a list of job titles. Intent *kinds* (`research`, `review`, `write`, …) are unchanged; they map onto these callsigns.

Index speaks in a **conversational** register (complete sentences, collaborative tone) while specialists keep a compressed field-report voice. Dedicated TTS agents use `mode: "spoken"`. Routing, writ inventory, and delegation rules are shared; only the user-facing register differs. See [Voice](../concepts/voice.md).

The starter JSON files are the **single source of truth** for what gets written. They live in `agents/` in the source tree and are embedded into the binary at build time. `--init` writes them verbatim — pretty-printed, in source order, byte-identical to the source tree — so the file you see on disk matches what a maintainer would see in the repo.

Each file is a plain JSON document — a model id, system prompt, tool allowlist, optional advisor block, optional cost-attribution metadata. Edit them in place, or copy one as the basis for your own agent. Drop a new `agents/<id>.json` into the source tree and it'll show up in `--init` automatically on the next build (no code changes required).

## Re-seeding from defaults

To reset *one* starter, delete the file and re-run `--init`:

```
rm ~/.arbiter/agents/scout.json
arbiter --init
```

To reset *every* starter back to the embedded defaults — useful after an upgrade where the maintained rules have improved:

```
arbiter --init --force
```

`--force` overwrites every current starter unconditionally (local edits are
lost; no confirm) and deletes the retired job-title starter files
(`research.json`, `reviewer.json`, `writer.json`, `devops.json`,
`planner.json`, `backend.json`, `frontend.json`, `marketer.json`,
`social.json`) so an upgrade does not keep both rosters loaded. Schedules
and `/agent` commands that still name those old ids need retargeting to the
new callsigns.

## What's NOT in `--init`

- **API keys.** You provide those yourself — see [environment.md](environment.md) for `OPENROUTER_API_KEY` and the `~/.arbiter/openrouter_api_key` file convention.
- **The tenant store.** Created automatically the first time you run `--api` or `--add-tenant`. Empty until you provision a tenant.
- **Sessions, scratchpad memory, artifact stores.** Created on demand by the relevant subsystems.

## When to run it

Once, after first install. Subsequent upgrades don't require it (the binary works against an existing `~/.arbiter/`); re-run with `--force` after an upgrade if you want to pick up improvements to starter rules. Re-running without `--force` is also harmless — it's how you pick up *new* starter agents added in a release without losing your existing edits.

You can also skip `--init` entirely if you'd rather hand-curate `~/.arbiter/agents/`. Arbiter only needs the directory to exist and at least one valid agent file — bootstrapping that by hand is a five-line JSON file.

## Output

Stdout-only. Lists each agent created (or kept), then prints `Edit these or add your own. Then run: arbiter`. No interactivity, no prompts.
