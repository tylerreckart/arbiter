# `arbiter --setup-tools`

Interactive OpenTUI wizard for the three optional agent tool surfaces that are
not covered by the first-run API-key flow:

| Surface | What it enables | Where it writes |
|---------|-----------------|-----------------|
| Web search | `/search <query>` | `~/.arbiter/search_api_key` |
| Browse | `/browse <url>` via Playwright | `playwright` entry in `mcp_servers.json` |
| MCP servers | `/mcp tools` / `/mcp call` | `~/.arbiter/mcp_servers.json` |

```
arbiter --setup-tools
```

Requires a TTY. Non-interactive callers get a clear error and exit `1`.

## Menu

The top-level screen shows live status for search, browse, and every registered
MCP server, then offers:

1. **Web search** — paste or replace a Brave Search API key, or clear the saved
   file. Environment variables (`ARBITER_SEARCH_API_KEY`, `BRAVE_SEARCH_API_KEY`)
   still win over the file; the wizard warns if a key is coming from the env.
2. **Browse** — enable Playwright (headless or headed), reinstall, or remove the
   `playwright` registry entry. `/browse` needs that exact server name.
3. **MCP servers** — add Playwright, add a hosted server via `mcp-remote`
   (name + HTTPS URL), add a custom stdio command, or remove an entry.
   Names are stored lowercase; reusing a name prompts before overwrite.
   On open, the wizard normalizes any mixed-case names already on disk.

Arrow keys move, Enter selects, Esc cancels a prompt (keeps changes already
written). Secrets are masked while typing.

## First-run offer

After the provider-key / model / starter-agent wizard finishes, arbiter asks
whether to configure tools now. Choosing **Configure search / browse / MCP**
opens this same wizard. **Skip for now** leaves tools unset; run
`--setup-tools` later.

## When changes apply

Files are written immediately. A running `arbiter` or `arbiter --api` process
does **not** reload them — restart to pick up a new search key or MCP registry.

## See also

- [Web search](../concepts/search.md)
- [MCP servers](../concepts/mcp.md)
- [Environment](environment.md) — env-vs-file precedence for the search key
