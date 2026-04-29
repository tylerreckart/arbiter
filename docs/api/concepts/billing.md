# Billing

## Money on the wire

All monetary values are **micro-cents** (µ¢), 64-bit integers, where `1 USD = 1_000_000 µ¢`. Integer math keeps fractional-cent LLM costs precise without floating-point drift.

## Per-turn billing

For each LLM turn (master agent + every delegated and parallel sub-agent):

1. Arbiter computes the provider's list-price cost broken down by token type (plain input, output, cache reads, cache writes) using the pricing table for the model.
2. Adds a **20% markup** over that provider cost, rounded up to the nearest µ¢. Formula: `markup = ceil(provider_uc × 0.20)`, implemented as `(provider_uc * 20 + 99) / 100`.
3. The tenant is billed `provider_uc + markup_uc`.

Historical ledger rows capture the cost breakdown **as applied at the time of the call** — when pricing rates update later, past invoices don't shift.

## Caps

If a tenant has a non-zero `monthly_cap_micro_cents` and a turn would push the month-to-date total over it, arbiter cancels the in-flight orchestration and emits a `cap_exceeded` signal in both the live SSE stream and the final `done` event.

Caps are enforced **at turn granularity** — a single turn can push past the cap, but no further turns will run. The tenant may end up slightly over cap (by one turn's worth); this is deliberate to avoid dropping a turn mid-generation.

### Cap-exceeded behavior on the wire

When a turn's `mtd_micro_cents` crosses `monthly_cap_micro_cents`, arbiter:

1. Emits an `error` event with `message: "monthly usage cap exceeded"`, `mtd_micro_cents`, and `cap_micro_cents`.
2. Cancels the orchestrator's in-flight API calls.
3. Lets in-progress turns complete (they're already paid for).
4. Skips all further turns.
5. Emits `done` with `cap_exceeded: true` and whatever the final `content` was.

## Aggregation

Pre-aggregated rollups are available via [`GET /v1/admin/usage/summary`](../admin/usage-summary.md) (group by `model`, `day`, or `tenant`). For chart recipes and dashboards, see that endpoint's docs.

## See also

- [Tenants](tenants.md)
- [SSE event catalog](sse-events.md) — where `token_usage`, `error`, and `done.cap_exceeded` are described.
- [`GET /v1/admin/usage`](../admin/usage.md) — raw ledger rows.
- [`GET /v1/admin/usage/summary`](../admin/usage-summary.md) — bucketed analytics.
- [`PATCH /v1/admin/tenants/:id`](../admin/tenants-patch.md) — change a tenant's cap.
