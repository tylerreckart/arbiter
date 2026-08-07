# `GET /v1/models`

**Auth:** tenant — _Status:_ stable

List the models arbiter knows how to route, including the context-window sizes used by auto-compaction and the TUI sidebar. Powers the frontend's model picker and the interactive `/model` catalogue. The catalogue changes only when the operator deploys a new build — clients should cache it briefly and re-fetch on a fresh session.

Hosted traffic routes through OpenRouter; catalogue ids are OpenRouter slugs (plus a few short Claude aliases rewritten to dotted Anthropic slugs at request time). Unlisted OpenRouter / `ollama/` ids still route; compaction falls back to a substring heuristic (or a char-budget when the window is unknown).

## Request

No path params, no query params, no body.

```bash
curl -H "Authorization: Bearer atr_…" \
  http://arbiter.example.com/v1/models
```

## Response

### 200 OK

```json
{
  "count": 24,
  "models": [
    { "id": "anthropic/claude-sonnet-5",     "provider": "openrouter", "context_window": 1000000 },
    { "id": "anthropic/claude-opus-5",       "provider": "openrouter", "context_window": 1000000 },
    { "id": "openai/gpt-5.5",                "provider": "openrouter", "context_window": 1000000 },
    { "id": "openai/gpt-5.6-sol",            "provider": "openrouter", "context_window": 1000000 },
    { "id": "google/gemini-3.6-flash",       "provider": "openrouter", "context_window": 1000000 },
    { "id": "x-ai/grok-4.5",                 "provider": "openrouter", "context_window": 500000 },
    { "id": "claude-sonnet-4-6",             "provider": "openrouter", "context_window": 1000000 },
    { "id": "claude-haiku-4-5",              "provider": "openrouter", "context_window": 200000 }
  ]
}
```

| Field            | Type   | Description |
|------------------|--------|-------------|
| `id`             | string | Matches what you pass in `agent_def.model` (or as the model on a stored agent). |
| `provider`       | string | Today: `openrouter` for hosted models (Ollama ids use the `ollama/` prefix and are not listed here). |
| `context_window` | number | Approximate context window in tokens. Same value compaction and the TUI sidebar use for this id. `0` would mean unknown (none of the listed hosted models use that). |

## Failure modes

| Status | When | Body |
|--------|------|------|
| 401    | Missing / invalid bearer; tenant disabled. | `{"error": "..."}` |

## See also

- [`POST /v1/orchestrate`](orchestrate.md) — `agent_def.model` is routed by provider prefix; the catalogue is the known set for pickers, not a hard allowlist.
- TUI [`/model`](../tui/commands.md) — list catalogue or change an agent's model at runtime.
