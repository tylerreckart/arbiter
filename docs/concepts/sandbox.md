# Per-tenant sandbox

Arbiter's HTTP API ships with `/exec` disabled by default — agents can write files, fetch URLs, delegate to sub-agents, but cannot run shell commands. The **sandbox** lifts that restriction safely: one persistent Docker container per tenant, with a bind-mounted workspace directory that `/exec`, `/write`, and `/read` all share.

The sandbox is **off by default** and **opt-in** at server startup. Without it the runtime behaves exactly as before: `/exec` returns a clean `ERR:` block and the agent adapts.

## What you get when it's on

- **`/exec <cmd>`** runs inside the tenant's container, cwd `/workspace`, with `--network=none` (no internet) and configurable CPU/memory/PID caps.
- **`/write <path>`** drops the file into `/workspace/<path>` on the host volume *and* emits the existing SSE `file` event. The next `/exec` sees it on the bind mount.
- **`/read <path>`** falls back to the workspace when no DB artifact matches the path. Cross-conversation reads still flow through the curated memory graph (`/read #<aid> via=mem:<mid>`); workspace fallback applies only to same-conversation path reads.
- **`/list`** appends a `[sandbox workspace]` section listing every file in the tenant's workspace tree.

Containers are **per-tenant**, not per-conversation or per-request. Two simultaneous requests from the same tenant share one container; two tenants always get separate containers. Workspace bytes persist across requests and across server restarts (only the containers themselves are torn down on shutdown).

## Setup

Three required pieces:

1. A working `docker` on `$PATH` (rootful or rootless — arbiter shells out to the CLI).
2. A container image you've built or pulled that has whatever toolbelt your agents need (`bash`, `git`, `python3`, `curl`, build tooling — whatever you want available behind `/exec`).
3. The `ARBITER_SANDBOX_IMAGE` env var pointing at it, plus any caps you want to override.

Setting `ARBITER_SANDBOX_IMAGE` is what flips the feature on. Everything else has defaults.

```bash
ARBITER_SANDBOX_IMAGE=arbiter/sandbox:latest \
ARBITER_SANDBOX_MEMORY_MB=1024 \
ARBITER_SANDBOX_CPUS=2 \
ARBITER_SANDBOX_EXEC_TIMEOUT=60 \
arbiter --api
```

On a clean start with the sandbox enabled and usable, the server logs:

```
[arbiter] sandbox enabled: image=arbiter/sandbox:latest network=none memory=1024m cpus=2.00 pids=256 timeout=60s
```

If the runtime can't be found, the image is missing, or the workspaces root can't be created, the server logs the reason and **continues running with `/exec` disabled**. The alternative — refusing to start — is too sharp a knife for an opt-in feature.

```
[arbiter] sandbox disabled: sandbox runtime 'docker' not found on PATH
```

### Env vars

Setting `ARBITER_SANDBOX_IMAGE` is the only required step. Everything else has a default.

| Variable                          | Purpose                                                                                  | Default     |
|-----------------------------------|------------------------------------------------------------------------------------------|-------------|
| `ARBITER_SANDBOX_IMAGE`           | Container image to run inside. Required — without this, the sandbox stays off.           | unset       |
| `ARBITER_SANDBOX_RUNTIME`         | Runtime binary to shell out to. v1 ships with `docker` support only.                     | `docker`    |
| `ARBITER_SANDBOX_NETWORK`         | Docker `--network` value. `none` keeps `/exec` offline; `bridge` lets it reach the internet. | `none`  |
| `ARBITER_SANDBOX_MEMORY_MB`       | Hard memory cap per container, MB. `0` = no cap.                                          | `512`       |
| `ARBITER_SANDBOX_CPUS`            | CPU shares per container. `0` = no cap.                                                   | `1.0`       |
| `ARBITER_SANDBOX_PIDS_LIMIT`      | Max processes per container. `0` = no cap.                                                | `256`       |
| `ARBITER_SANDBOX_EXEC_TIMEOUT`    | Wall-clock kill, seconds, per `/exec` call. `0` = no parent-side timeout.                 | `30`        |

Workspaces land at `~/.arbiter/workspaces/t<tenant_id>/` (one directory per tenant, mode `0700`). Override by editing `ApiServerOptions::sandbox_workspaces_root`; there's no env var for the path yet — open an issue if you need one.

### Image recommendations

The arbiter binary intentionally ships **no default image**. You bring your own so the toolbelt available behind `/exec` reflects your agents' actual needs.

A minimal starter (build it once, tag it, point `ARBITER_SANDBOX_IMAGE` at it):

```dockerfile
# Dockerfile
FROM alpine:3
RUN apk add --no-cache bash coreutils findutils grep sed gawk \
                       curl git python3 jq
WORKDIR /workspace
```

```bash
docker build -t arbiter/sandbox:latest .
ARBITER_SANDBOX_IMAGE=arbiter/sandbox:latest arbiter --api
```

`--network=none` means anything inside `/exec` cannot reach the internet — `pip install`, `npm install`, `curl https://…` will fail. Either pre-install everything in the image, switch to `ARBITER_SANDBOX_NETWORK=bridge` (loosens the boundary), or have the agent use `/fetch` (runs in the arbiter process, unaffected by `--network=none`).

## Inside the container

The runtime starts each per-tenant container with:

```
docker run -d \
  --name arbiter-sandbox-t<tid> \
  --network=none \
  --memory=<mb>m \
  --cpus=<cpus> \
  --pids-limit=<pids> \
  -v <workspaces_root>/t<tid>:/workspace \
  -w /workspace \
  --read-only \
  --tmpfs /tmp:rw,size=64m \
  --restart=no \
  --user <uid>:<gid>            # Linux only; macOS Docker Desktop maps automatically
  <image> sleep infinity
```

Key properties:

- **Read-only root.** Only `/workspace` (bind mount) and `/tmp` (tmpfs, 64 MB) are writable. Agents can't durably modify the image's filesystem.
- **No network.** `/exec` can't talk to anything off the host. `/fetch` and `/search` still work because those run in the arbiter process itself.
- **Per-tenant uid mapping (Linux).** The container runs as the arbiter process's host uid/gid so files written by `/exec` are owned by arbiter and readable back through the bind mount. On macOS, Docker Desktop already does this mapping; the `--user` flag is skipped.
- **Sleep-infinity keep-alive.** Containers stay warm across requests. `docker exec` against the running container is the per-`/exec` path; cold-starts only happen once per tenant per server lifetime.

`docker exec` is invoked with `-i --workdir /workspace <name> sh -c "<command>"`. Output is captured to combined stdout+stderr, capped at 32 KB, with `[exit N]` appended on non-zero exits and `[timed out after Ns]` appended when the wall-clock kill fires.

## Lifecycle

| Event                              | What happens                                                                          |
|------------------------------------|---------------------------------------------------------------------------------------|
| First `/exec` for a tenant         | Container is started lazily (`docker run -d … sleep infinity`).                       |
| Subsequent `/exec` (any tenant)    | Reuses the warm container; `docker exec` only.                                        |
| Tenant container already running on startup | The manager re-attaches (deterministic name `arbiter-sandbox-t<tid>`).         |
| Container dies between requests    | Next `/exec` notices via `docker inspect`, removes the stale row, restarts.            |
| `arbiter --api` receives SIGTERM   | `stop_all()` removes every managed container. Workspace bytes remain on disk.          |
| Operator runs `docker rm -f arbiter-sandbox-t<tid>` out of band | Stale row is reaped on next `/exec`; workspace is untouched.  |

There is **no idle reaping** in v1. A tenant that ran one `/exec` six hours ago still owns a `sleep infinity` container. Operators wanting tighter resource hygiene should `docker container prune` on a cron or restart the server periodically; a built-in idle-stop is a planned follow-up.

There is also **no per-tenant disk quota** in v1. The workspace dir grows as `/write` and `/exec` produce files. Cap it with a filesystem-level quota (XFS project quotas, ZFS dataset reservations, a dedicated mount) or restart-clean if you can tolerate that.

## Resource caps

The defaults are conservative for general-purpose `/exec`:

| Cap                 | Default | What it limits                                                                     |
|---------------------|---------|------------------------------------------------------------------------------------|
| `--memory`          | 512m    | Hard cgroup memory ceiling. Container OOM-kills past this; agent sees `[exit 137]`. |
| `--cpus`            | 1.0     | CPU shares. One full core, fractional values OK (`0.5`, `2.5`).                    |
| `--pids-limit`      | 256     | Max processes inside the container. Catches fork bombs.                             |
| Wall-clock timeout  | 30s     | Per-`/exec`. Parent-side SIGKILL; surfaces as `[timed out after 30s]`.              |
| Output cap          | 32 KB   | Combined stdout+stderr per `/exec`. Trailing bytes truncated.                       |
| Workspace tmpfs     | 64 MB   | `/tmp` inside the container. Persists only for the container's lifetime.            |

Tune memory/cpus/pids/timeout via env vars; output cap and tmpfs size are hard-coded in v1.

## Workspace layout

Each tenant gets one host directory:

```
~/.arbiter/workspaces/
└── t1/                    # tenant_id = 1
    ├── notes.md           # written by /write notes.md
    ├── build/
    │   └── output.bin     # produced by /exec from inside the container
    └── src/
        └── main.py
```

The directory is created on first `/exec`, `/write`, or `/read` for that tenant (whichever lands first) with mode `0700`. The bind mount makes `/workspace/notes.md` inside the container equivalent to `~/.arbiter/workspaces/t1/notes.md` on the host.

Path safety: `/write` and the `/read` fallback reject absolute paths, traversal segments (`..`), and null/control bytes. Same canonicaliser the persistent artifact store uses.

## Slash command behaviour in sandbox mode

`/exec ls -la` — runs inside the container, sees `/workspace` and `/tmp`. Won't see `/usr/local/bin/arbiter`, `~/.arbiter/`, the host's filesystem, or anything on the public internet.

```
[/exec ls -la]
total 8
drwx------ 2 1000 1000 4096 May 11 22:01 .
drwx------ 3 1000 1000 4096 May 11 22:01 ..
-rw------- 1 1000 1000   31 May 11 22:01 notes.md
[END EXEC]
```

`/write src/main.py` followed by `/exec python3 src/main.py` — `/write` lands the file in the workspace; the next `/exec` sees it. Cross-turn workflows are the primary value of the sandbox.

`/read src/main.py` — same-conversation path read. If `/write --persist src/main.py` ran in this conversation, the DB artifact store wins (richer metadata, mime types, sha). Otherwise the workspace fallback serves the bytes.

`/list` — emits both: persistent artifacts under the default heading, then a `[sandbox workspace]` section enumerating the workspace tree.

## Failure modes

| Symptom                                                        | Cause                                                                                 | Surface                                                                                       |
|----------------------------------------------------------------|----------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| `[arbiter] sandbox disabled: …` at startup                     | Missing image var, docker not on PATH, can't create workspaces root.                  | Server starts with `/exec` disabled. Fix the cause and restart.                                |
| `ERR: docker run failed: …` in a `/exec` block                 | First-time container start failed — bad image tag, registry auth, kernel feature.     | Agent sees the docker stderr verbatim; the next `/exec` retries the start.                     |
| `ERR: /exec is disabled in this execution context — …`        | Sandbox isn't wired (config never set, or unusable at startup).                       | Agent's standard "adapt your plan" surface. Identical to the SaaS-default behavior.            |
| `[timed out after 30s]` appended to `/exec` output             | Wall-clock kill fired. Bump `ARBITER_SANDBOX_EXEC_TIMEOUT` or shorten the command.    | Exit code 124; tool-result block marked as failed.                                             |
| `[exit 137]` on an `/exec` that allocated a lot                | OOM-killed by the memory cap. Bump `ARBITER_SANDBOX_MEMORY_MB` or shrink the workload.| Tool-result block marked as failed.                                                            |
| `ERR: invalid path: …` from `/write` or `/read`                | Absolute path, traversal, or control byte in the request.                             | Agent's standard sanitiser error.                                                              |
| Container vanished out of band (`docker rm -f`)                | Operator force-removed the container.                                                 | Next `/exec` notices the stale row, restarts cleanly.                                          |

## Failure-mode philosophy: degrade, don't crash

A misconfigured or unreachable sandbox should never take the API server down. The full degradation ladder:

1. Sandbox not configured (no `ARBITER_SANDBOX_IMAGE`) → `/exec` returns ERR with the standard "adapt" message. Server runs.
2. Sandbox configured but unusable at startup (docker missing, workspaces root unwritable, image string empty) → server logs the reason at startup, keeps running, `/exec` still returns ERR.
3. Sandbox usable at startup but container fails to start for tenant T → tenant T's `/exec` returns ERR with the docker output; the next call retries; other tenants are unaffected.
4. Container running but `/exec` times out / OOMs → standard exit-code framing in the tool result; container stays warm for subsequent calls.

In every case the agent sees an actionable ERR block and adapts. There is no scenario where a broken sandbox propagates out as a 5xx to the API caller.

## Security boundary

What the sandbox protects against:

- **Arbitrary host shell.** Agents can't escape `/workspace` to read other tenants' data, the SQLite DB, provider API keys, or admin secrets.
- **Resource abuse.** Memory, CPU, pids, wall-clock, and output caps bound the blast radius of a runaway or adversarial agent.
- **Network exfiltration.** `--network=none` means no DNS, no outbound TCP — even if the agent gets a shell inside the container.

What it does not protect against:

- **Container escapes.** The standard Docker threat model. Run arbiter on a kernel you trust, behind a daemon you trust. CVE-tracking is your responsibility.
- **Cross-conversation leakage *within* a tenant.** The workspace is tenant-scoped, not conversation-scoped. An agent that writes a secret in conversation A can read it back in conversation B as the same tenant. Use the persistent artifact store for conversation-scoped storage.
- **Image content.** If the image contains a backdoor or vulnerable binaries, the sandbox happily runs them. Pin tags, scan images, treat the registry as part of your supply chain.
- **Cost from `/exec`.** Sandbox CPU/memory consumption isn't currently surfaced to the billing service. Agent token cost is billed as normal; container compute is operator infrastructure.

## See also

- [`docs/concepts/writ.md`](writ.md) — the slash-command DSL `/exec`, `/write`, `/read` belong to.
- [`docs/concepts/artifacts.md`](artifacts.md) — the persistent, conversation-scoped artifact store. Complementary to the workspace.
- [`docs/concepts/mcp.md`](mcp.md) — MCP servers run in the arbiter process, not in the sandbox. `/mcp call` is unaffected by `--network=none`.
- [`docs/concepts/operations.md`](operations.md) — broader operational notes.
- [`docs/cli/environment.md`](../cli/environment.md) — every env var arbiter reads, including the `ARBITER_SANDBOX_*` set.
