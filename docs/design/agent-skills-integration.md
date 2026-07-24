# Design: Agent Skills in the Arbiter runtime

**Status:** proposal (not implemented)  
**Audience:** maintainers deciding how (and whether) to adopt the [Agent Skills](https://agentskills.io) `SKILL.md` standard inside Arbiter.

This document maps Arbiter's current architecture onto the skills progressive-disclosure model and lists the concrete runtime changes required for a faithful integration.

---

## 1. Verdict

Arbiter can host Agent Skills without changing its core thesis (agents drive a small writ runtime; no JSON tool-calling schema). The natural fit is:

1. **Catalog** in the system prompt (or a depth-0 preamble), analogous to lessons/todos injection.
2. **Activation** via a new `/skill` writ + `SkillInvoker` callback, mirroring MCP/lessons — **not** via `/read`, which today only reads conversation artifacts in HTTP mode.
3. **Resources** (`scripts/`, `references/`, `assets/`) resolved against the skill directory and loaded on demand through the same writ (or a gated `/skill read` subcommand), with `/exec` for runnable scripts when the agent has that capability.
4. **Hard gating** through a new `skills` capability bundle kept in sync between `constitution.cpp` and `commands.cpp`.

There is **no** first-class skills system today. The name `a2a::Skill` already means something else (Agent Card discovery metadata derived from `Constitution.capabilities`). Any user-facing docs must keep those two concepts distinct.

---

## 2. What Arbiter is today (relevant spine)

```
User / HTTP / A2A
        │
        ▼
Orchestrator::send / send_streaming / run_dispatch
        │
        ├─ depth 0: lessons preamble + open todos
        ├─ master: global_status() roster
        ▼
Agent::stream/send  ← system_prompt = Constitution::build_system_prompt()
        │
        ├─ StreamFilter strips /cmd lines from visible text
        ├─ parse_agent_commands(response)
        └─ execute_agent_commands(... capabilities allowlist ...)
                 │
                 └─ invoker callbacks (MCP, A2A, lessons, todos, exec, …)
```

Key properties that constrain a skills design:

| Property | Implication for skills |
|----------|------------------------|
| Writ DSL, not tool schemas | Activation must be a slash command (or prompt-injected content), not a JSON function call |
| Capability allowlists are hard | Skills that imply actions still go through existing writs; a `/skill` writ itself needs a bundle |
| Invokers are `std::function` wiring | Follow MCP/lesson pattern — no plugin ABI |
| Prompt inventory is bundle-driven | Catalog / how-to-activate text must land in `resolve_bundles` + `arbiter_prompt` |
| `/read` is artifact-only (HTTP) | Cannot use file-read activation against host `SKILL.md` paths without a new path |
| API is tenant-isolated | Skill discovery scopes must not leak across tenants; project skills need a defined trust story |
| No hot reload of registries | Same as `mcp_servers.json` / `agents/*.json` — load at process or request start |

Closest existing analogues:

| Concept | Relationship to Agent Skills |
|---------|------------------------------|
| **Lessons** | Pre-turn injection of short learned notes — pattern for catalog/preamble, but not progressive and not filesystem packs |
| **Capability bundles** | Prompt inventory + dispatch gate — the enforcement layer skills must join |
| **MCP** | External tool registry + `/mcp` writ + invoker — best structural template for `/skill` |
| **`a2a::Skill`** | Name collision only — federation card fields, not `SKILL.md` packs |
| **Constitution rules/goal** | Always-on soft guidance — opposite of progressive disclosure |

---

## 3. Target model (Agent Skills progressive disclosure)

Per the open standard, a skill is a directory with at least `SKILL.md` (YAML frontmatter + markdown body), optionally `scripts/`, `references/`, `assets/`.

| Tier | Content | When | Approx. cost |
|------|---------|------|--------------|
| 1. Catalog | `name` + `description` (+ path) | Session / turn start | ~50–100 tokens / skill |
| 2. Instructions | Full `SKILL.md` body | Model (or user) activates | <5k tokens recommended |
| 3. Resources | Bundled files | Explicit follow-up load | Variable |

Arbiter should implement model-driven activation (agent decides from the catalog) plus optional user-explicit activation from the TUI/CLI later.

---

## 4. Required work by subsystem

### 4.1 Discovery & registry (`include/skills/`, new)

Add a small filesystem scanner parallel to `mcp::load_server_registry`:

**Scan roots (recommended):**

| Scope | Path | Precedence |
|-------|------|------------|
| Project (cwd / repo) | `<cwd>/.arbiter/skills/`, `<cwd>/.agents/skills/` | Highest |
| User | `~/.arbiter/skills/`, `~/.agents/skills/` | Lower |
| Bundled (optional later) | in-tree or install prefix | Lowest |

Rules aligned with the client-implementation guide:

- Discover subdirectories containing exactly `SKILL.md`.
- Parse YAML frontmatter leniently; skip skills with empty/missing `description`.
- Project overrides user on `name` collision; warn on shadow.
- Cap scan depth / directory count to avoid runaway trees.
- Trust gate for project-level skills (especially under `--api` / untrusted cwd).

**In-memory record (minimum):**

```cpp
struct SkillRecord {
    std::string name;
    std::string description;
    std::string skill_md_path;   // absolute
    std::string base_dir;        // parent of SKILL.md
    // optional: license, compatibility, allowed_tools, metadata
};
```

Wire load points:

- TUI / `--send`: once at process start (cwd-aware).
- `--api`: per-request or per-tenant — prefer **request-scoped Manager** like MCP so tenants cannot share mutable skill state, with roots from options + optional tenant config.

### 4.2 Catalog disclosure (tier 1)

**Where to inject:** prefer `Constitution::build_system_prompt()` when the agent has the skills bundle, *or* a depth-0 preamble in `Orchestrator::run_dispatch` (same site as lessons/todos).

Recommendation: **system-prompt section** when `/skill` is in `capabilities` (keeps catalog stable across turns inside one conversation), with empty-catalog omission (no empty `<available_skills/>`).

Shape (writ-native, not JSON-tool-native):

```
SKILLS:
When a task matches a skill description, activate it before proceeding:
  /skill <name>
Then follow the returned instructions. Relative paths resolve against the skill directory.

- pdf-processing — Extract PDF text, fill forms, merge files. Use when handling PDFs.
- data-analysis — Analyze datasets and produce summary reports.
```

Implementation touchpoints:

- `src/constitution.cpp` — new `bundle_skills_inventory()` + entry in `resolve_bundles` for `/skill`.
- Optional: inject live catalog from a `SkillCatalogProvider` callback set on `Orchestrator` / `ApiServer` (catalog is dynamic; static prompt strings are not enough alone).

Because constitutions today bake inventory strings at prompt-build time, the cleanest split is:

1. Static how-to lines in the skills bundle.
2. Live catalog list supplied at turn start (preamble) *or* rebuilt into the system prompt if agents already rebuild it every turn (they do — `Agent` calls `build_system_prompt()` per request construction). So a callback into `Constitution` is awkward; better to append catalog in `Agent` / orchestrator after `build_system_prompt()`, or pass an optional catalog string into prompt assembly.

**Practical recommendation:** append the live catalog in `Orchestrator` immediately before the model call (depth-agnostic or depth 0 only — see open questions), similar to lessons, rather than teaching `Constitution` about the filesystem.

### 4.3 Activation writ (tier 2)

Add `/skill` to the writ surface:

| Form | Effect |
|------|--------|
| `/skill` or `/skill list` | Re-list catalog (names + descriptions) |
| `/skill <name>` | Activate: return body (+ resource index), wrapped |
| `/skill read <name> <rel-path>` | Tier-3 resource load, path confined to skill `base_dir` |

Changes:

1. `kWritPrefixes` + parse branch in `src/commands.cpp`.
2. Handler in `execute_agent_commands` with capability gate.
3. `using SkillInvoker = std::function<std::string(const std::string& kind, const std::string& args, const std::string& agent_id)>;` in `commands.h`.
4. `Orchestrator::set_skill_invoker(...)` + pass-through into `execute_agent_commands` (mirror `lesson_invoker`).
5. Wire in `main.cpp` (TUI/CLI) and `api_server.cpp` (`wire_orchestrator_tools` / per-request builders).
6. `/help skill` topic in the help corpus.
7. Bundle sync: `resolve_bundles` **and** the dispatch allowlist block in `execute_agent_commands` (comment in `commands.cpp` already requires these tables to stay aligned).

Activation result shape (structured wrapping for later compaction / dedupe):

```
[/skill activate pdf-processing]
<skill_content name="pdf-processing">
...body (frontmatter stripped or kept — pick one; prefer stripped)...

Skill directory: /home/user/.arbiter/skills/pdf-processing
Relative paths resolve against this directory.

<skill_resources>
  <file>scripts/extract.py</file>
  <file>references/forms.md</file>
</skill_resources>
</skill_content>
[END SKILL]
```

**Deduping:** track activated skill names per `(conversation, agent)` and return a short `OK: already active` on repeat activations within the same history.

### 4.4 Resources & scripts (tier 3)

- `/skill read` must canonicalize paths and reject `..` escape outside `base_dir`.
- Running scripts: do **not** invent a new exec channel. Document that skills use `/exec` when the agent has `/exec` (and sandbox rules in API mode apply unchanged).
- Optional frontmatter `allowed-tools`: map onto Arbiter capabilities as a **soft** prompt note at activation time; hard enforcement remains the agent's constitution allowlist. (Do not silently expand capabilities because a skill asked for `/exec`.)

Sandbox note: under `--api` with Docker sandbox, skill files on the host are **not** inside the tenant workspace. Either:

- copy/mount an allowlisted skill root into the sandbox, or
- keep skill instruction loading in-process (invoker reads host files) and only run scripts that were `/write`n into the workspace.

In-process activation + explicit copy-into-workspace for scripts is the smaller v1.

### 4.5 Capability & constitution surface

| Change | File(s) |
|--------|---------|
| Recognize `/skill` → bundle `"skills"` | `constitution.cpp` `resolve_bundles`, `commands.cpp` gate |
| Inventory + command rules for `/skill` | `constitution.cpp` bundle helpers |
| Starter agents: opt-in `"/skill"` where appropriate | `agents/*.json`, `starters.h` |
| Master (`index`): decide whether skills are master-routable | `master_constitution()` — likely yes for routing, activation on specialists |

Empty `capabilities` today means default bundles **excluding** MCP. Skills should follow MCP: **opt-in**, not in the default empty-capabilities set, so existing agents do not suddenly see a skills catalog.

### 4.6 Orchestration & context hygiene

| Concern | Requirement |
|---------|-------------|
| Sub-agents | Decide: inherit parent's activated skills vs catalog-only. Recommendation: catalog available if capability granted; activations are per-agent history (delegation already creates separate histories). |
| Context compaction | Arbiter does not currently summarize away mid-history the way coding agents do; if that lands later, mark `[/skill …]…[END SKILL]` blocks as protected (lessons are a precedent for durable guidance). |
| Loop detection | Treat repeated failed `/skill` names like other tools. |
| Streaming | Ensure `/skill` is in `StreamFilter` / writ prefixes so activation lines do not leak into visible prose. |

### 4.7 Faces: TUI, CLI, HTTP

| Face | Wiring |
|------|--------|
| TUI / `--send` | Load user + cwd project skills; set invoker on the long-lived `Orchestrator` |
| `--api` | Per-request manager; roots from `ApiServerOptions` + optional tenant overrides; never scan another tenant's home |
| A2A | Do **not** overload `a2a::Skill`. Optionally later advertise a card skill id `agent-skills` if the agent has `/skill` |

### 4.8 Tests

Minimum suite (doctest, alongside `tests/test_a2a.cpp` / MCP tests):

- Frontmatter parse (valid, missing description, colon-in-unquoted description fallback).
- Discovery precedence (project shadows user).
- Path confinement for `/skill read`.
- Capability denial when `/skill` absent.
- Activation body strip + resource listing.
- Catalog omitted when registry empty.
- Deduped second activation.

### 4.9 Docs

- New concept page `docs/concepts/agent-skills.md` (user-facing) once implemented.
- Cross-links from `writ.md`, `environment.md` (`~/.arbiter/skills/`), `philosophy.md` (skills as progressive prompt packs, not a second tool protocol).
- Explicit glossary note distinguishing **Agent Skills** from **A2A Skill**.

---

## 5. Suggested implementation phases

### Phase A — Discovery + `/skill` activate (MVP)

Enough to be skills-compatible for instruction packs without scripts:

1. `skills::Manager` scan + parse.
2. Catalog preamble or system-prompt append.
3. `/skill` / `/skill <name>` writ + invoker + capability bundle.
4. TUI/CLI + API wiring.
5. Unit tests + concept stub.

### Phase B — Resources + trust

1. `/skill read` with path jail.
2. Project-skill trust gate.
3. Sandbox guidance / optional workspace materialization for scripts.
4. Activation dedupe per conversation.

### Phase C — Product polish

1. TUI user-explicit activation (`/skill-name` or picker).
2. `allowed-tools` → soft capability advisory in activation payload.
3. Optional A2A card advertisement.
4. Hot-reload or `--reload-skills` (today's registries do not hot-reload; match that unless product asks otherwise).

---

## 6. Explicit non-goals (v1)

- Replacing constitutions, lessons, or MCP with skills.
- JSON tool-calling / `activate_skill` function schemas.
- Automatically granting writs listed in skill frontmatter.
- Unifying `a2a::Skill` with `SKILL.md` packs.
- Hot-reloading skill directories in a long-lived `--api` process.

---

## 7. Open questions

1. **Depth policy:** inject catalog at every depth, or only depth 0 (lessons/todos style)? Specialists with `/skill` likely need the catalog on delegated turns — lean toward **whenever the agent has the capability**, not depth-gated.
2. **Frontmatter retention:** strip vs keep on activate. Strip matches most dedicated-tool clients; keep helps models see `compatibility`.
3. **Tenant skill store:** filesystem-only vs rows in `TenantStore` for multi-tenant API hosts without shared home directories.
4. **Naming in prompts:** use `SKILLS:` (filesystem packs) vs avoid the word "skill" near A2A roster lines that already print `skills:` from Agent Cards.

---

## 8. File-level checklist (MVP)

| Area | Touch |
|------|-------|
| New | `include/skills/{types,manager,parse}.h`, `src/skills/*` |
| Writ surface | `include/commands.h`, `src/commands.cpp` |
| Orchestrator | `include/orchestrator.h`, `src/orchestrator.cpp` |
| Prompt bundles | `src/constitution.cpp` |
| Process wiring | `src/main.cpp`, `src/api_server.cpp`, `include/api_server.h` |
| Env / layout docs | `docs/cli/environment.md` |
| Tests | `tests/test_skills.cpp` (+ CMake) |
| Starters | selective `agents/*.json` |

Estimated invasiveness: **moderate** — follows an established invoker/registry pattern; no change to the provider client or SSE event model beyond ordinary `tool_call` emissions for the new writ.

---

## 9. One-sentence summary

> Treat Agent Skills as a filesystem registry of progressive prompt packs, disclosed like a capability bundle and activated through a `/skill` writ with MCP-shaped invoker wiring — without conflating them with A2A card skills or bypassing Arbiter's hard capability allowlists.
