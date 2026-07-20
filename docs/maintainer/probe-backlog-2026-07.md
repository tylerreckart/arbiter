# Probe backlog (2026-07) — public issues only

Runtime audit of Arbiter `main` @ `265d679` against `SECURITY.md` /
concept docs. Build: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake
--build build -j && ctest --test-dir build` — **42/42 passed**.

**Private security findings are not in this file.** They were delivered
to the maintainer via the probe run transcript and should be filed only
through [SECURITY.md](../../SECURITY.md) private reporting. Do not paste
exploit detail into public GitHub issues.

Surfaces explicitly considered (even when no issue filed): tenant auth
path, SSRF/`is_blocked_*`, sandbox path sanitiser, capability gating,
MCP env/spawn, A2A HTTP client, orchestrator cancel/`InFlightRegistry`,
`file_max_bytes`, scheduler vs request threads, idempotency cache,
JSON depth limit, TUI prior crash classes (CHANGELOG).

---

## Public issue drafts

Paste each block into GitHub. Local IDs match the probe triage table.

### Align auth docs with single-tenant no-bearer API
- **Type:** docs
- **Severity:** high
- **Disclosure:** public
- **Component:** docs
- **Modes:** api
- **Status:** confirmed
#### Summary
`include/api_server.h` and the `--api` startup banner document
single-tenant mode with no tenant bearer required. `SECURITY.md`
"Known security-relevant guarantees", `docs/concepts/authentication.md`,
`docs/concepts/tenants.md`, `docs/philosophy.md` §4, `docs/cli/api.md`,
and nearly every `docs/api/**` page still claim multi-tenant Bearer
`atr_…` auth. Operators following SECURITY.md will mis-model the
trust boundary (especially with `Access-Control-Allow-Origin: *`).
#### Evidence
- `include/api_server.h` — "Runtime routes — single-tenant mode; no tenant bearer required."
- `src/api_server.cpp` `ApiServer::handle_client` — resolves primary tenant via `resolve_primary_tenant`; no `find_by_token` / `extract_bearer` on non-admin routes.
- `src/cli.cpp` startup banner — `"POST /v1/orchestrate (single-tenant; no bearer required)"`.
- `SECURITY.md` — still lists tenant token isolation and Bearer auth as guarantees.
#### Reproduction
1. Read `SECURITY.md` "Known security-relevant guarantees".
2. Start `arbiter --api` and observe the banner.
3. `curl http://127.0.0.1:<port>/v1/conversations` with no `Authorization` header → `200`.
#### Expected
Docs and SECURITY.md describe the actual trust boundary.
#### Actual
Docs claim Bearer auth; runtime serves the data plane without it.
#### Impact
Operators deploy expecting authz that is not present; proxy configs
built around "token required" leave the data plane open inside the
trust network.
#### Fix direction
Either (a) restore optional/required tenant bearer validation and update
the CLI banner, or (b) rewrite SECURITY.md + concept/API docs for the
single-tenant local-trust model and call out that the reverse proxy
(or network policy) **is** the authn layer. Prefer (b) if single-tenant
is intentional; keep admin Bearer as-is.
#### Test plan
- [ ] Doc review checklist: every `Authorization: Bearer atr_` example
      either removed or marked "optional / proxy-layer".
- [ ] SECURITY.md "Known guarantees" matches `handle_client` behaviour.
#### Notes
Not a claim that localhost bind is wrong — the gap is the false
guarantee. Related private findings (if any) go through SECURITY.md.

---

### Honour --disable-tenant as an API kill-switch
- **Type:** bug
- **Severity:** high
- **Disclosure:** public
- **Component:** api-server
- **Modes:** api, scheduler
- **Status:** confirmed
#### Summary
`SECURITY.md` and `docs/concepts/tenants.md` say `disabled=true` /
`--disable-tenant` immediately rejects API traffic. The request path
never checks `Tenant::disabled`, and API startup forcibly re-enables
the primary tenant if it was disabled.
#### Evidence
- `src/api_server.cpp` `resolve_primary_tenant` — picks lowest id; ignores `disabled`.
- `src/cli.cpp` API startup — `if (primary && primary->disabled) set_disabled(..., false)`.
- Contrast: `TenantStore::find_by_token` still returns nullopt when disabled (dead path for runtime routes); `Scheduler::fire_task` still skips disabled tenants.
#### Reproduction
1. `arbiter --api` running; `arbiter --disable-tenant 1`.
2. `arbiter --list-tenants` shows `disabled`.
3. `curl /v1/conversations` still returns `200` with data.
4. Restart `--api`; tenant is active again.
#### Expected
Disabled primary tenant → `401`/`403` on data-plane routes (or hard
refuse to serve), and restart must not silently clear the flag.
#### Actual
Disable is ignored while the process runs; restart clears it.
#### Impact
Operators lose the documented emergency kill-switch (compromised agent,
runaway spend, hostile client on the bind address).
#### Fix direction
Check `disabled` in `resolve_primary_tenant` (or once per request after
resolve); remove the auto-re-enable on startup (or gate it behind an
explicit `--force-enable-tenant`); add a metric/log when disabled
traffic is rejected.
#### Test plan
- [ ] `unit` or scripted API test: disable → data-plane returns 401;
      restart without re-enable still refuses.
#### Notes
Scheduler already respects disabled — keep that path consistent.

---

### Gate /todo /schedule /lesson through capability allowlists
- **Type:** bug
- **Severity:** high
- **Disclosure:** public
- **Component:** commands
- **Modes:** tui, cli-send, api, scheduler
- **Status:** confirmed
#### Summary
`docs/concepts/writ.md` claims `capabilities` is a hard runtime
allowlist. `dispatch_agent_commands`'s `bundle_of` returns `""` for
`todo` / `schedule` / `lesson`, so those writs are always allowed even
when the constitution omits them. Constitution `resolve_bundles` maps
`/todo` → `todos` for the **prompt** inventory only — dispatcher never
checks the `todos` bundle. `/schedule` and `/lesson` are not mapped in
either layer.
#### Evidence
- `src/commands.cpp` `bundle_of` + capability gate (~1972–2041) — empty bundle ⇒ skip gate.
- `src/constitution.cpp` `resolve_bundles` — `/todo` → `todos`; no schedule/lesson; unknown caps dropped.
- Starter `agents/research.json` capabilities omit `/todo`/`/schedule`/`/lesson`.
- Existing test only covers `/exec` denial (`tests/test_commands.cpp`).
#### Reproduction
1. Agent constitution: `"capabilities": ["/search"]`.
2. Model emits `/todo add x`, `/schedule …`, `/lesson …`.
3. Observe handlers run instead of `ERR: capability not granted`.
#### Expected
Writs outside the allowlist are rejected at dispatch (same as `/exec`).
#### Actual
`/todo`, `/schedule`, `/lesson` always dispatch.
#### Impact
Jailbroken / prompt-injected agents can mutate todos, create recurring
scheduler work, and poison lessons despite a narrow warrant — breaks
the isolation model for constrained agents (e.g. research-only).
#### Fix direction
Add `todos`, `schedule`, `lessons` (names TBD) to both
`resolve_bundles` and `bundle_of` / `allowed_bundles`; decide whether
empty capabilities still imply defaults that include them (today's
default set includes `todos` in the prompt path only).
#### Test plan
- [ ] Extend `unit_commands`: capabilities=`{"/search"}` → `/todo`,
      `/schedule`, `/lesson` denied; with explicit caps → allowed.
#### Notes
`/advise` is already gated in the dispatcher; keep that as the pattern.

---

### Make file_max_bytes enforcement atomic under parallel /write
- **Type:** bug
- **Severity:** medium
- **Disclosure:** public
- **Component:** api-server
- **Modes:** api
- **Status:** likely
#### Summary
SECURITY.md promises a per-response file-size cap to prevent SSE/OOM
blowups. The interceptor does `load` then `fetch_add` without a CAS
loop, so concurrent `/write` from `/parallel` children can all pass the
check and exceed `file_max_bytes`.
#### Evidence
- `src/api_server.cpp` write interceptor in `handle_orchestrate` (~8907–8922) and A2A stream path (~7775–7788).
#### Reproduction
1. Set a small `file_max_bytes`.
2. Agent `/parallel` with N children each `/write` near `cap/N + 1`.
3. Observe total captured bytes > cap (hypothesis; needs stress harness).
#### Expected
Sum of accepted file event bodies ≤ `file_max_bytes`.
#### Actual
Check/add is racy; overshoot is possible under parallel writers.
#### Impact
Runaway or adversarial parallel writers can still buffer large SSE
payloads — weakens the documented OOM guard.
#### Fix direction
`compare_exchange_weak` loop reserving `size`, or a mutex around the
cap accounting; reject losers with the existing ERR string.
#### Test plan
- [ ] Unit/stress: N threads calling the interceptor with sizes that
      individually fit but jointly exceed the cap.
#### Notes
Related: A2A sync `message/send` and scheduler paths may omit the
interceptor entirely — see private disclosure track.

---

### Add SSRF regression tests for is_blocked_address helpers
- **Type:** test
- **Severity:** medium
- **Disclosure:** public
- **Component:** tests
- **Modes:** api, tui, cli-send
- **Status:** confirmed
#### Summary
`commands.cpp` has a carefully expanded IPv4/IPv6 SSRF denylist
(mapped, 6to4, Teredo, NAT64, CGNAT, metadata hosts) applied via
`CURLOPT_OPENSOCKETFUNCTION`, but there is no `unit_*` coverage that
would fail if a prefix check regressed. MCP/A2A outbound paths are
also unguarded by shared helpers (hardening tracked separately).
#### Evidence
- `src/commands.cpp` `is_blocked_v4` / `is_blocked_v6` / `preflight_ssrf_check` / `safe_opensocket_cb`.
- `tests/` — no SSRF suite (grep for `is_blocked` / `ssrf` empty aside from incidental strings).
#### Reproduction
N/A — coverage gap.
#### Expected
Table-driven tests for literals and representative hostname denylist
entries.
#### Actual
Only manual/code-review assurance.
#### Impact
Future refactors can re-open known bypass classes without CI signal.
#### Fix direction
Export test hooks or move helpers to a small internal header; add
`unit_ssrf` with v4/v6 vectors from the comments in `commands.cpp`.
#### Test plan
- [ ] New `unit_ssrf` ctest target.
#### Notes
Do not weaken production fail-closed behaviour for the suite.

---

### Document CORS + single-tenant trust model for operators
- **Type:** docs
- **Severity:** medium
- **Disclosure:** public
- **Component:** docs
- **Modes:** api
- **Status:** confirmed
#### Summary
`docs/concepts/operations.md` CORS section still says Bearer auth
carries credentials so `Allow-Origin: *` is safe. Under single-tenant
no-bearer mode, any origin that can reach the bind address can call
the data plane from a browser. Operators need an explicit warning.
#### Evidence
- `docs/concepts/operations.md` § CORS.
- `src/api_server.cpp` `kCorsHeaders` — `Access-Control-Allow-Origin: *`.
#### Reproduction
Read operations.md; compare to `--api` banner / `handle_client`.
#### Expected
Ops doc states: with no tenant bearer, CORS `*` means browser-reachable
API ≡ fully open to that origin; restrict via proxy origin allowlist
or disable CORS in production.
#### Actual
Doc assumes Bearer auth mitigates CSRF-ish browser access.
#### Impact
SPA-on-another-origin + reachable API = unintended cross-site use of
the orchestration/data plane.
#### Fix direction
Update operations.md; optionally add `ARBITER_CORS_ORIGINS` (already
mentioned as future work in the comment).
#### Test plan
- [ ] Doc-only; optional integration note in `docs/cli/api.md`.
#### Notes
Pairs with the auth-docs alignment issue; can be one PR.

---

### Hold sandbox ensure_container lock off the docker CLI path
- **Type:** enhancement
- **Severity:** low
- **Disclosure:** public
- **Component:** sandbox
- **Modes:** api, scheduler
- **Status:** confirmed
#### Summary
`SandboxManager::ensure_container` holds `mu_` across
`container_is_running`, optional responsive probe, and
`start_container` (up to ~30s `docker run`). Concurrent `/exec` for
**any** tenant serialises behind that lock.
#### Evidence
- `src/sandbox.cpp` `ensure_container` (~474–513).
#### Reproduction
Two tenants first-use `/exec` concurrently; observe second waits on
docker start of the first.
#### Expected
Per-tenant startup critical sections; cross-tenant cold-starts overlap.
#### Actual
Process-wide mutex covers slow docker CLI.
#### Impact
Tail latency / apparent hangs under multi-tenant legacy DBs or bursty
first-use; not a correctness bug by itself.
#### Fix direction
Narrow `mu_` to map mutations; use per-tenant once-flags or a separate
start mutex map; keep inspect/run outside the global lock.
#### Test plan
- [ ] Optional stress under `examples/sandbox/setup.sh --check`.
#### Notes
Out of scope as a vulnerability; operability only.

---

## Surfaces reviewed — no public issue

| Surface | Notes |
|---------|-------|
| `InFlightRegistry` cancel | Holds `reg.mu` through `orch->cancel()` — UAF class from comments already fixed. |
| Admin token compare | XOR loop; early size mismatch is a minor timing oracle — not filed (admin-only, shared secret). |
| JSON parser depth | `kMaxDepth = 256` present. |
| Artifact path sanitiser | Strong component walk; no symlink FS escape (DB blobs, not paths). |
| Theme pointer races | CHANGELOG / `theme.cpp` thread_local fix already landed. |
| TUI degenerate geometry | CHANGELOG 0.x fix for `bufferDrawTextBufferView`; no new generalisation found this pass. |
| Idempotency cache | Keyed by tenant_id+key; races documented and handled. |
| Provider cost / TLS-on-listen / unsandboxed TUI `/exec` | Out of scope per SECURITY.md. |
