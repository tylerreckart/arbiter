# Getting started

Two paths to a first agent reply. Pick whichever matches how you want to operate arbiter — the runtime is the same in both.

- **[Hosted preview](hosted.md)** *(recommended)* — managed arbiter behind an HTTP API. We run the server, you authenticate with a bearer token. Currently in **limited preview**; join the waitlist for access. Best when you want to call arbiter from a web app or automation without operating infrastructure yourself.
- **[Local install](local.md)** — run arbiter on your own machine. You bring the provider keys; you control the binary. Free, open-source, fully featured. Best when you want `/exec` (the hosted environment disables it by default; self-hosted deploys can enable it through the [per-tenant Docker sandbox](../concepts/sandbox.md)), filesystem access, or full control over deployment.

Both paths speak the same writ DSL, expose the same SSE protocol, and use the same agent constitutions. Anything you author against one works against the other.

## See also

- [`philosophy.md`](../philosophy.md) — why arbiter is shaped the way it is.
- [`concepts/writ.md`](../concepts/writ.md) — the slash-command DSL agents emit.
- [`api/`](../api/index.md) — HTTP API reference.
- [`cli/`](../cli/index.md) — non-interactive command-line reference.
- [`tui/`](../tui/index.md) — interactive terminal client.
