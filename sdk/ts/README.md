# `@arbiter/sdk`

TypeScript client for Arbiter's intent surfaces. Maps `apiKey` → `Authorization: Bearer`.

```bash
# from this repo
npm install ./sdk/ts
```

Published name: `@arbiter/sdk` (monorepo package; not yet on npm).

## IntentClient

```ts
import { IntentClient } from '@arbiter/sdk';

const client = new IntentClient({
  baseUrl: process.env.ARBITER_URL, // default http://127.0.0.1:8080
  apiKey: process.env.ARBITER_KEY,
});

// Classify / route (POST /v1/intent) — no dispatch.
const intent = await client.classify({
  message: 'Look up primary sources on the RISC-V memory model',
});

// Desired end state (POST /v1/reconcile) — SSE under the hood.
const result = await client.reconcile({
  targetState: {
    system: 'wire-transfer-portal',
    account: '4820194',
    amountUSD: 12500.0,
    status: 'SETTLED',
  },
  invariants: ['amountUSD <= 15000.00', 'require_two_factor_auth_prompt'],
  workspace: { kind: 'sandbox' },
  verification: { requireTests: true, command: 'auto' },
  rollbackOnFailure: true,
});

// result.status: "satisfied" | "failed" | "rolled_back" | "canceled"
```

`reconcileStream` yields SSE frames and still resolves to the same terminal result:

```ts
for await (const ev of client.reconcileStream(req)) {
  console.log(ev.event, ev.data);
}
```

## Auth

`apiKey` is the tenant bearer (`atr_…`). There is no second auth scheme.

## See also

- [Reconcile](../../docs/concepts/reconcile.md)
- [`POST /v1/reconcile`](../../docs/api/reconcile.md)
- [`POST /v1/intent`](../../docs/api/intent.md)
