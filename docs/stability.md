# Stability contract

This is the surface a 1.0 release will freeze. 1.0 has not shipped.
Until that tag, a breaking change still needs a changelog note, and the
experimental list below can move without a major bump.

A patch release after 1.0 will not change the stable lists. A minor
release may add flags, fields, files, and environment variables. It
will not remove or rename the ones marked stable here, and it will not
change their meaning. The experimental list is outside that promise.

## Stable CLI

These invocations keep their names and the jobs they do today:

- Interactive TUI (no subcommand), including `--theme PRESET`
- `--no-exec`
- `--init` and `--init --force`
- `--setup-tools`
- `--send <agent> <message>`
- `--connect [URL] [--token TOKEN]`
- `--export-theme PRESET`
- `--help` / `-h`
- Tenant admin: `--add-tenant`, `--list-tenants`, `--rotate-tenant-token`, `--disable-tenant`, `--enable-tenant`

`--api` is the flag that starts the experimental HTTP server. The flag
name stays. The routes behind it do not.

## Stable agent files

`~/.arbiter/agents/<id>.json` is the constitution file. The field
catalog is [Agent (catalog row)](concepts/data-model.md#agent-catalog-row).
1.0 will keep these fields and their current meanings:

- `version` — optional integer. Absent means `1`. A value of `1` or
  greater is stored and written back as that integer.
- `model`
- `advisor` and the legacy `advisor_model` shorthand
- `presence`
- `intent`
- `capabilities`
- `routing_examples`
- `max_turns`

Unknown fields are left alone by readers that do not understand them.
A file with `version` greater than this binary knows is still loaded;
the number is preserved on save.

## Stable `~/.arbiter` layout

The directory is created mode `0700`. `tenants.db`, its `-wal` and
`-shm` sidecars, and `tui.log` are mode `0600`.

| Path | Role |
|------|------|
| `openrouter_api_key` | OpenRouter key, one line |
| `master_model` | Wizard override for the index model |
| `search_api_key` | Brave Search key |
| `admin_token` | Admin token for `/v1/admin/*` |
| `tui.json` | Theme preset, theme file, or overrides |
| `mcp_servers.json` | MCP server registry |
| `a2a_agents.json` | Remote A2A registry (the protocol is experimental) |
| `agents/*.json` | Constitutions |
| `themes/*.json` | Custom themes |
| `tenants.db` | SQLite store used by the TUI and by `--api` |
| `tui.log` | Interactive TUI stderr capture |
| `tui-fatal.log` | Crash note from the TUI fatal handlers |
| `history` | Merged editor history |
| `conversations/` | Legacy TUI archive and `layout.json` mirror |
| `sessions/*.json` | Legacy per-cwd snapshots |

`XDG_CONFIG_HOME` is not consulted. `HOME` selects the directory.

The database schema version is SQLite `PRAGMA user_version`. It is `1`.
Opening the store runs the migration hook: a database at `0` (fresh, or
from before the memory full-text index) rebuilds that index and moves
to `1`. A database already at `1` is left alone. A later migration
raises the number in that hook; it does not bump the application
version by itself.

## Stable environment variables

The catalog in [Environment](cli/environment.md) is the stable set:
every variable in that page keeps its name and its effect.

Three variables are read by the binary and are **not** in that catalog.
They are debug switches and are not stable: `ARBITER_MAX_CONNECTIONS`,
`ARBITER_TUI_LAYOUT_TRACE`, `ARBITER_TUI_LOG_PROBE`.

## Experimental

These can change without a major version bump, including before and
after the 1.0 tag, until a later note moves them into the stable lists:

- `arbiter --api` and the HTTP and SSE shapes under `/v1/*`
- Tenants: tokens, isolation, and the admin routes
- A2A
- Reconcile
- The scheduler
- Voice (`channel: voice`, spoken mode, and any later audio path)

The tenant **flags** listed above are stable names for provisioning a
token. The tenant data model and the HTTP API that consumes the token
are experimental.
