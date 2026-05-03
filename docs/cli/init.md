# `arbiter --init`

Initialize the per-user config directory and seed it with example agent constitutions. Idempotent — safe to re-run; existing files are not overwritten.

```
arbiter --init
```

## What gets created

```
~/.arbiter/
├── agents/
│   ├── index.json         master agent — orchestrates the rest
│   ├── research.json      web-research agent (uses /search, /fetch)
│   ├── reviewer.json      code-review agent
│   └── …                  one JSON per starter agent
```

The starter set varies as new examples are added; `--init` always writes whatever the current build ships. Each file is a plain JSON document — a model id, system prompt, tool allowlist, optional advisor model, optional cost-attribution metadata. Edit them in place, or copy one as the basis for your own agent.

If a file already exists, `--init` leaves it alone and prints a brief note. To re-seed a starter from scratch, delete or rename your edited copy and re-run.

## What's NOT in `--init`

- **API keys.** You provide those yourself — see [environment.md](environment.md) for `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, etc. and the `~/.arbiter/api_key` file convention.
- **The tenant store.** Created automatically the first time you run `--api` or `--add-tenant`. Empty until you provision a tenant.
- **Sessions, scratchpad memory, artifact stores.** Created on demand by the relevant subsystems.

## When to run it

Once, after first install. Subsequent upgrades don't require it (the binary works against an existing `~/.arbiter/`), but running it again to pick up newly-added starter agents is harmless.

You can also skip `--init` entirely if you'd rather hand-curate `~/.arbiter/agents/`. Arbiter only needs the directory to exist and at least one valid agent file — bootstrapping that by hand is a five-line JSON file.

## Output

Stdout-only. Lists each starter agent with a one-line description of its role, then prints `Edit these or add your own. Then run: arbiter`. No interactivity, no prompts.
