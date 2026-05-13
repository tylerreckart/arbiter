# Security policy

Arbiter is experimental software. Several of its built-in capabilities
(unsandboxed shell execution, arbitrary HTTP fetches, MCP subprocesses,
multi-tenant token handling) are deliberately powerful and need to be
deployed with care. This document describes how to report vulnerabilities
and what the project considers in-scope.

## Reporting a vulnerability

**Please do not open public GitHub issues for security reports.**

Use one of the following private channels:

- GitHub's [private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
  on this repository (Security tab → "Report a vulnerability").
- Email: open an issue requesting a private channel and a maintainer
  will share an address.

We aim to acknowledge a report within 5 business days and to ship a fix
or mitigation before publishing details. Coordinated disclosure: we'd
like to credit you in the advisory once a fix is released.

When reporting, please include:

- A clear description of the issue and its impact.
- Steps to reproduce, ideally a minimal script or HTTP transcript.
- The arbiter version (`arbiter --help` shows it on the bottom border;
  also visible in `CMakeLists.txt`'s `project(... VERSION ...)`).
- Operating system and any non-default build flags.

## Supported versions

Arbiter is pre-1.0 and ships frequent breaking changes. We provide
security fixes for:

- The latest tagged release on `main`.
- The current `main` branch.

Older tags do **not** receive backports.

## In-scope

- Memory-safety bugs in the C++ code (use-after-free, OOB reads/writes,
  race conditions producing exploitable state).
- Authentication / authorization bypasses on the HTTP API (`/v1/*`
  routes), including admin-token leaks, tenant-isolation breaks
  (one tenant reading another's conversations / artifacts / scratchpads
  / agents), and bearer-token confusion attacks.
- SQL or path injection through any user-supplied field that lands in a
  query, file path, or shell argument.
- Server-Side Request Forgery against the orchestrator's outbound
  fetchers (`/fetch`, `/search`, MCP HTTP transports).
- Credential exposure: tenant tokens, admin tokens, or provider API keys
  appearing in logs, error messages, SSE events, or persisted artifacts.
- TLS / certificate-validation failures in the LLM provider client.
- Any path that lets an agent escape the documented sandboxing posture
  described below.

## Out-of-scope

Several capabilities are documented as unsandboxed by design. Reports
that simply demonstrate them are not vulnerabilities:

- **`/exec` is unsandboxed in the terminal client** — agents reach
  through the user's shell with the user's permissions. The README and
  agent documentation flag this. The HTTP API (`arbiter --api`)
  disables `/exec` by default (`ApiServerOptions::exec_disabled = true`)
  and an attempt returns an `ERR:` tool result to the agent.
- **Sandboxed `/exec` (opt-in via `ARBITER_SANDBOX_IMAGE`, since
  0.5.0)** — when enabled, `/exec` runs inside a per-tenant Docker
  container with `--network=none --read-only`, a bind-mounted
  `/workspace`, configurable memory / CPU / pids caps, and a per-exec
  wall-clock kill. The sandbox is **not** a container-escape
  mitigation: it inherits the kernel's Docker threat model.
  Container-breakout CVEs in the host kernel or Docker daemon are
  out-of-scope here — track and patch them upstream. In-scope sandbox
  vulnerabilities (and please report them): workspace path traversal
  past the canonicaliser, cross-tenant access to another tenant's
  workspace mount, sandbox-disable bypass that lets `/exec` reach the
  host shell. The sandbox boundary, what it protects against, and what
  it does not are documented in
  [`docs/concepts/sandbox.md#security-boundary`](docs/concepts/sandbox.md#security-boundary).
- **The HTTP server has no built-in TLS, rate limiting, or DDoS
  protection.** Production deployments are expected to put a reverse
  proxy (nginx / caddy / cloudflare) in front. The default bind is
  `127.0.0.1` to make this the path of least resistance.
- **Local file reads through `/read`** are bounded by `arbiter`'s
  process credentials. If an agent on a developer's machine reads a
  file the user already has access to, that's the documented behavior.
- **Provider rate-limit / cost-spike issues**. Configure an external
  billing service (`ARBITER_BILLING_URL`) to enforce per-tenant caps
  if you need cost protection.
- Vulnerabilities in third-party dependencies that have already been
  publicly disclosed and are tracked upstream — please file those with
  the dependency rather than here, but we'd appreciate a heads-up.

## Hardening notes for operators

If you run `arbiter --api` in a multi-tenant context, please:

1. Always front the server with a TLS-terminating reverse proxy.
2. Bind to `127.0.0.1` and let the proxy handle public traffic, or
   restrict the public bind via firewall rules.
3. Set the runtime's admin token (and any tokens your billing service
   needs) via environment variables, never via tracked config files.
4. Rotate tenant tokens promptly when staff turnover happens — the
   `--disable-tenant` CLI flag flips a kill-switch immediately.
5. Run the process under a dedicated unprivileged user. The default
   `/exec` policy on the API path is "disabled," but defense in depth
   matters — a future bug shouldn't compromise the host.
6. Configure an external billing service (`ARBITER_BILLING_URL`) to
   cap spend per tenant. The runtime alone does not enforce caps.
7. Outbound fetches are filtered through an SSRF guard
   (`src/commands.cpp` — `is_blocked_address`) that rejects RFC1918,
   loopback, link-local, CGNAT, and cloud-metadata-adjacent addresses
   on every connect, including after redirects. Don't disable this.
8. If you enable the sandbox (`ARBITER_SANDBOX_IMAGE`), treat the
   image as part of your supply chain — pin tags, scan for CVEs, and
   keep the base layer current. Leave `ARBITER_SANDBOX_NETWORK=none`
   (the default) unless agents genuinely need outbound HTTP from
   inside `/exec`; switching to `bridge` exposes the container to the
   host network and removes the strongest escape mitigation the
   sandbox offers. Keep the resource caps non-zero — `0` disables the
   cgroup limit and lets a runaway `/exec` consume host resources.
   Per-tenant workspace bytes live at `~/.arbiter/workspaces/t<tid>/`
   mode `0700`; if you run as a shared user, additionally restrict
   `~/.arbiter/` itself.

## Known security-relevant guarantees

- Tenant API tokens are stored only as SHA-256 digests; the plaintext
  is shown exactly once at issue.
- Tenant isolation: every conversation, message, artifact, scratchpad,
  memory entry, and stored agent is scoped by `tenant_id`. ID leaks
  across tenants surface as `404`, never as data exposure.
- Admin tokens are constant-time compared and only accepted on
  `/v1/admin/*` routes.
- The HTTP server's `/v1/orchestrate` path intercepts `/write` so
  agent-generated files do not touch the server filesystem; they're
  streamed back as SSE `file` events for the client to handle.
- A per-response file-size cap (default 10 MiB across all files in one
  response, configurable via `ApiServerOptions::file_max_bytes`)
  prevents a runaway agent from OOM'ing the server.
