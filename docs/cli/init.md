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
│   ├── reviewer.json       code review — terse, defect-focused
│   ├── research.json       research analyst — haiku + opus advisor combo
│   ├── writer.json         essays, READMEs, docs, creative writing
│   ├── devops.json         infrastructure engineer — shell, git, CI/CD
│   ├── planner.json        task decomposition into phased execution plans
│   ├── backend.json        APIs, data modeling, distributed systems
│   ├── frontend.json       components, state, accessibility, performance
│   ├── marketer.json       strategy, positioning, campaign concepts
│   └── social.json         platform-native content, growth, engagement
```

The master orchestrator (`index`) is not written to disk — it ships as a compiled-in constitution and is loaded automatically. Only sub-agent definitions live in `~/.arbiter/agents/`.

The starter JSON files are the **single source of truth** for what gets written. They live in `agents/` in the source tree and are embedded into the binary at build time. `--init` writes them verbatim — pretty-printed, in source order, byte-identical to the source tree — so the file you see on disk matches what a maintainer would see in the repo.

Each file is a plain JSON document — a model id, system prompt, tool allowlist, optional advisor block, optional cost-attribution metadata. Edit them in place, or copy one as the basis for your own agent. Drop a new `agents/<id>.json` into the source tree and it'll show up in `--init` automatically on the next build (no code changes required).

## Re-seeding from defaults

To reset *one* starter, delete the file and re-run `--init`:

```
rm ~/.arbiter/agents/research.json
arbiter --init
```

To reset *every* starter back to the embedded defaults — useful after an upgrade where the maintained rules have improved:

```
arbiter --init --force
```

`--force` overwrites unconditionally; any local edits are lost. There's no interactive confirm.

## What's NOT in `--init`

- **API keys.** You provide those yourself — see [environment.md](environment.md) for `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, etc. and the `~/.arbiter/api_key` file convention.
- **The tenant store.** Created automatically the first time you run `--api` or `--add-tenant`. Empty until you provision a tenant.
- **Sessions, scratchpad memory, artifact stores.** Created on demand by the relevant subsystems.

## When to run it

Once, after first install. Subsequent upgrades don't require it (the binary works against an existing `~/.arbiter/`); re-run with `--force` after an upgrade if you want to pick up improvements to starter rules. Re-running without `--force` is also harmless — it's how you pick up *new* starter agents added in a release without losing your existing edits.

You can also skip `--init` entirely if you'd rather hand-curate `~/.arbiter/agents/`. Arbiter only needs the directory to exist and at least one valid agent file — bootstrapping that by hand is a five-line JSON file.

## Output

Stdout-only. Lists each agent created (or kept), then prints `Edit these or add your own. Then run: arbiter`. No interactivity, no prompts.
