# Web search

Agents can issue `/search <query>` mid-turn to discover sources before fetching them — the missing piece that turned previous research turns into "guess a DOI from training memory and hope it resolves." Configured per-deployment via two `ApiServerOptions` fields:

| Field             | Default | Description |
|-------------------|---------|-------------|
| `search_provider` | `brave` | Search backend. Only `brave` is implemented in v1; `tavily` and `exa` slots reserved. |
| `search_api_key`  | `""`    | Provider API key. Empty ⇒ `/search` returns ERR with a clear "configure ARBITER_SEARCH_API_KEY" message. |

`arbiter --api` reads the key from `ARBITER_SEARCH_API_KEY` (preferred — provider-agnostic name) or `BRAVE_SEARCH_API_KEY` (convenience for Brave-only deployments). The provider can be set via `ARBITER_SEARCH_PROVIDER`. Without a key the slash command degrades cleanly: agents see `ERR: web search unavailable in this context` and adapt by dropping the `/search` step.

## Slash commands

| Command | Effect |
|---------|--------|
| `/search <query>` | Top 10 results for the query. |
| `/search <query> top=N` | Top N results (clamped to 1..20). The `top=N` token is stripped from the query. |

Capped at **4 searches per turn** (vs. /fetch's 3), since result bodies are small and agents typically need a couple of search→fetch round trips per topic.

## Result format

Numbered lines, one per hit, with title, snippet, and URL:

```
[/search planet nine 2024 top=5]
1. Title of the first result — Snippet text from the search engine, lightly trimmed.
   https://example.com/article-1
2. Second result title — More snippet text.
   https://example.com/article-2
...
[END SEARCH]
```

The dispatcher applies a 16 KB body cap (matching /fetch). Brave's `<strong>...</strong>` highlighting is stripped before rendering.

## Provider notes

**Brave** — `https://api.search.brave.com/res/v1/web/search`. The free tier gives 2,000 queries/month; production deployments should set a paid plan and a per-tenant rate limit at the proxy layer. Errors propagate verbatim:

| Surface | Cause |
|---------|-------|
| `ERR: web search unavailable in this context` | No `search_invoker_cb_` wired (no key configured). |
| `ERR: Brave returned 401` | Bad key. |
| `ERR: Brave returned 403` | Plan / IP restriction. |
| `ERR: Brave rate-limited (429)` | Quota exhaustion. |
| `ERR: empty query` | `/search` with no terms. |

## Workflow for agents

The master constitution (and every research-flavoured starter agent) instructs agents to **search → fetch → browse**, in that order of escalation:

```
/search planet nine orbital clustering 2024
                                                       # next turn returns ranked URLs
/fetch https://arxiv.org/abs/2403.05451                # cheap libcurl read — preferred
/browse https://www.nature.com/articles/...            # JS / paywall — playwright
```

Guessing URLs from prior knowledge produces fabricated DOIs and dead links — `/search` discovers them, `/fetch` reads them when the page is static, and `/browse` falls back to a real browser when /fetch hits Cloudflare's "Just a moment", a login wall, or an SPA-only page.

## See also

- [MCP servers](mcp.md) — `/browse` is a convenience layer over the playwright MCP server.
- [`POST /v1/orchestrate`](../orchestrate.md)
