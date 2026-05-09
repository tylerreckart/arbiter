// arbiter/src/commands.cpp — Agent-invocable command execution
#include "commands.h"
#include "api_client.h"  // ContentPart full type — forward-declared in commands.h.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <filesystem>
#include <curl/curl.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace arbiter {

// ---------------------------------------------------------------------------
// parse_agent_commands
// ---------------------------------------------------------------------------

std::vector<AgentCommand> parse_agent_commands(const std::string& response) {
    std::vector<AgentCommand> result;
    std::istringstream ss(response);
    std::string line;
    bool in_code_block = false;

    while (std::getline(ss, line)) {
        // Track code fences (``` or ~~~)
        if (line.size() >= 3 &&
            (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~")) {
            in_code_block = !in_code_block;
            continue;
        }
        if (in_code_block) continue;

        // Trim trailing whitespace / CR
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            line.pop_back();

        if (line.size() > 7 && line.substr(0, 7) == "/fetch ") {
            AgentCommand cmd;
            cmd.name = "fetch";
            cmd.args = line.substr(7);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 8 && line.substr(0, 8) == "/search ") {
            AgentCommand cmd;
            cmd.name = "search";
            cmd.args = line.substr(8);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 8 && line.substr(0, 8) == "/browse ") {
            AgentCommand cmd;
            cmd.name = "browse";
            cmd.args = line.substr(8);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 6 && line.substr(0, 6) == "/read ") {
            // /read <path> — read this conversation's persisted artifact.
            AgentCommand cmd;
            cmd.name = "read";
            cmd.args = line.substr(6);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line == "/list") {
            // /list — list this conversation's persisted artifacts.
            // No-arg command; the args field stays empty.
            AgentCommand cmd;
            cmd.name = "list";
            result.push_back(std::move(cmd));

        } else if (line.size() > 5 && line.substr(0, 5) == "/mcp ") {
            AgentCommand cmd;
            cmd.name = "mcp";
            cmd.args = line.substr(5);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 5 && line.substr(0, 5) == "/a2a ") {
            // /a2a list                — enumerate configured remote agents
            // /a2a card <name>         — render one agent's card
            // /a2a call <name> <msg>   — synchronous send_message call
            AgentCommand cmd;
            cmd.name = "a2a";
            cmd.args = line.substr(5);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));
        } else if (line == "/a2a list") {
            AgentCommand cmd;
            cmd.name = "a2a";
            cmd.args = "list";
            result.push_back(std::move(cmd));

        } else if (line == "/schedule list") {
            AgentCommand cmd;
            cmd.name = "schedule";
            cmd.args = "list";
            result.push_back(std::move(cmd));
        } else if (line.size() > 10 && line.substr(0, 10) == "/schedule ") {
            // /schedule <phrase>: <message>   — create
            // /schedule list                   — list active schedules
            // /schedule cancel <id>            — delete
            // /schedule pause  <id>            — set status='paused'
            // /schedule resume <id>            — set status='active'
            AgentCommand cmd;
            cmd.name = "schedule";
            cmd.args = line.substr(10);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 14 && line.substr(0, 14) == "/mem add entry") {
            // Block form: /mem add entry <type> <title> [--artifact #<id>]
            //             <content body, multi-line>
            //             /endmem
            // The body is REQUIRED — empty content is rejected by the
            // dispatcher.  The block form ensures agents synthesise
            // retrievable text rather than stubbing entries with a title
            // alone (which makes /mem search useless across sessions).
            AgentCommand cmd;
            cmd.name = "mem";
            cmd.args = line.substr(5);   // "add entry <type> <title> [--artifact #<id>]"

            std::ostringstream body;
            bool closed = false;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "/endmem") { closed = true; break; }
                body << line << "\n";
            }
            cmd.content = body.str();
            if (!cmd.content.empty() && cmd.content.back() == '\n')
                cmd.content.pop_back();
            // Mid-stream cutoff (no /endmem yet): the orchestrator can use
            // this signal to request a continuation before the body is
            // submitted incomplete to the writer.
            cmd.truncated = !closed;
            result.push_back(std::move(cmd));

        } else if (line.size() > 5 && line.substr(0, 5) == "/mem ") {
            AgentCommand cmd;
            cmd.name = "mem";
            cmd.args = line.substr(5);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 7 && line.substr(0, 7) == "/agent ") {
            AgentCommand cmd;
            cmd.name = "agent";
            cmd.args = line.substr(7);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line == "/parallel") {
            // Fan-out block: /parallel\n/agent a msg\n/agent b msg\n/endparallel
            // Body is the literal /agent lines; execute_agent_commands re-parses
            // it and runs each child on its own thread.  We intentionally accept
            // ONLY /agent lines inside the block (ignoring everything else) so
            // the grammar is predictable and safe — no nested /exec or /write
            // fan-out today.
            AgentCommand cmd;
            cmd.name = "parallel";
            std::ostringstream body;
            bool closed = false;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "/endparallel") { closed = true; break; }
                body << line << "\n";
            }
            cmd.content   = body.str();
            cmd.truncated = !closed;
            result.push_back(std::move(cmd));

        } else if (line.size() > 6 && line.substr(0, 6) == "/pane ") {
            AgentCommand cmd;
            cmd.name = "pane";
            cmd.args = line.substr(6);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 8 && line.substr(0, 8) == "/advise ") {
            AgentCommand cmd;
            cmd.name = "advise";
            cmd.args = line.substr(8);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() > 6 && line.substr(0, 6) == "/exec ") {
            AgentCommand cmd;
            cmd.name = "exec";
            cmd.args = line.substr(6);
            if (!cmd.args.empty()) result.push_back(std::move(cmd));

        } else if (line.size() >= 5 &&
                   (line == "/help" || line.substr(0, 6) == "/help ")) {
            // /help            — list available topics
            // /help <topic>    — detailed reference for one slash command
            AgentCommand cmd;
            cmd.name = "help";
            cmd.args = (line.size() > 6) ? line.substr(6) : "";
            result.push_back(std::move(cmd));

        } else if (line.size() > 7 && line.substr(0, 7) == "/write ") {
            // Multiline write block: /write <path>\n<content>\n/endwrite
            AgentCommand cmd;
            cmd.name = "write";
            cmd.args = line.substr(7);
            // Trim trailing whitespace from path
            while (!cmd.args.empty() && (cmd.args.back() == ' ' || cmd.args.back() == '\r'))
                cmd.args.pop_back();
            if (cmd.args.empty()) continue;

            // Accumulate lines until /endwrite
            std::ostringstream body;
            bool closed = false;
            while (std::getline(ss, line)) {
                // Trim CR
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "/endwrite") { closed = true; break; }
                body << line << "\n";
            }
            // Remove trailing newline added by the loop
            cmd.content = body.str();
            if (!cmd.content.empty() && cmd.content.back() == '\n')
                cmd.content.pop_back();
            // Mid-stream cutoff: no /endwrite sentinel ⇒ body is incomplete.
            // Orchestrator uses this to request a continuation before writing.
            cmd.truncated = !closed;
            result.push_back(std::move(cmd));
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// /help corpus — detailed slash-command reference, on-demand
// ---------------------------------------------------------------------------
// The system prompt carries a compressed COMMANDS inventory plus values and
// turn-by-turn behavioral rules.  Verbose how-to-use prose, illustrative
// examples, path safety details, and escalation patterns live here and are
// surfaced only when the agent calls /help <topic>.  Topics are matched on
// the first whitespace-delimited token of the args, lowercased.

static std::string help_index() {
    return
        "Available topics:\n"
        "  /help web        — search / fetch / browse escalation pattern\n"
        "  /help write      — file output: ephemeral vs persist, path safety, examples\n"
        "  /help exec       — shell command usage and safety\n"
        "  /help delegation — /agent vs /parallel vs /pane semantics\n"
        "  /help mem        — structured memory graph + scratchpad reference\n"
        "  /help artifacts  — /read, /list, cross-conversation via=mem:<id>\n"
        "  /help mcp        — MCP tool invocation\n"
        "  /help advise     — when and how to consult the advisor model\n";
}

static std::string help_for_topic(const std::string& topic) {
    if (topic == "web" || topic == "search" || topic == "fetch" || topic == "browse") {
        return
            "Web research workflow — escalate in this order:\n"
            "  1. /search <query> [top=N]\n"
            "       Discover ranked URLs.  Don't guess from training memory — that\n"
            "       produces fabricated DOIs and dead links.  Default top=10, max 20.\n"
            "  2. /fetch <url>\n"
            "       Fast static HTTP fetch via libcurl.  Strips HTML to text.  Cheap;\n"
            "       preferred for arxiv abstracts, blog posts, plain HTML.\n"
            "  3. /browse <url>\n"
            "       JS-rendering fetch via playwright MCP.  Use ONLY when /fetch\n"
            "       returned 'Just a moment' (Cloudflare), a paywall login redirect,\n"
            "       or empty content (SPA-only pages).  Slower cold-start but renders\n"
            "       JS and handles modern news/journal sites.\n"
            "       Don't /browse a page that /fetch already retrieved successfully.\n"
            "Do not apologize for lacking web access — use the commands.\n";
    }
    if (topic == "write") {
        return
            "/write <path> ... /endwrite\n"
            "  Ephemeral file write — streamed to the client live as an SSE `file`\n"
            "  event.  The user sees the content inline but the server doesn't keep\n"
            "  it after the request ends.\n"
            "\n"
            "/write --persist <path> ... /endwrite\n"
            "  Same write, AND saves to the conversation's artifact store.  Durable;\n"
            "  readable later via /read <path>, /read #<aid>, or HTTP.  Use whenever\n"
            "  the user will likely refine or revisit the file in a later turn.\n"
            "\n"
            "Path safety (enforced server-side; failure rejects the write):\n"
            "  - Relative paths only.  No '..', no leading '/' or drive letters.\n"
            "  - No hidden (dotfile) names.\n"
            "  - ≤ 256 chars total.\n"
            "\n"
            "ALWAYS use /write to produce files.  NEVER say 'here is the content'\n"
            "without issuing /write — terminal output is not saveable by the user.\n"
            "\n"
            "Example:\n"
            "  /write --persist output/report.md\n"
            "  # Report Title\n"
            "\n"
            "  Body text here.\n"
            "  /endwrite\n";
    }
    if (topic == "exec") {
        return
            "/exec <shell command>\n"
            "  Runs in the current working directory with your user permissions.\n"
            "  Returns stdout+stderr; non-zero exit appended as [exit N].\n"
            "  Examples: /exec ls -la, /exec git status, /exec docker ps.\n"
            "  Use for filesystem inspection, process state, git, or system info.\n";
    }
    if (topic == "delegation" || topic == "agent" ||
        topic == "parallel" || topic == "pane") {
        return
            "/agent <agent_id> <message>\n"
            "  Synchronous: the sub-agent runs inline and its response is folded\n"
            "  into your current turn.\n"
            "\n"
            "/parallel ... /endparallel\n"
            "  Concurrent fan-out for INDEPENDENT subtasks (no child needs another's\n"
            "  output).  All children start immediately on separate threads; results\n"
            "  arrive aggregated as one [TOOL RESULTS] block when every child\n"
            "  finishes.  Reusing the same agent_id is fine — each child gets its\n"
            "  own ephemeral copy with fresh history, no shared state with siblings\n"
            "  or with the canonical agent.\n"
            "  Grammar (block form, no nesting):\n"
            "        /parallel\n"
            "        /agent researcher topic A\n"
            "        /agent researcher topic B\n"
            "        /agent coder write the skeleton\n"
            "        /endparallel\n"
            "\n"
            "/pane <agent_id> <message>\n"
            "  Asynchronous: the sub-agent runs visibly in its own pane.  When the\n"
            "  task finishes you receive the result as a fresh [PANE RESULT from\n"
            "  '<agent>' (task: ...)] message starting a new turn.  Multiple panes\n"
            "  run concurrently; results arrive in completion order.  Use when the\n"
            "  user benefits from watching progress live, or for fan-out where\n"
            "  result timing is unpredictable.\n"
            "  Treat received [PANE RESULT] blocks like /agent replies — fold the\n"
            "  findings into your synthesis.\n"
            "\n"
            "Delegation discipline:\n"
            "  - In a turn that emits ANY /agent or /parallel calls, your text body\n"
            "    is for ROUTING decisions ONLY.  At most one short sentence saying\n"
            "    what you're delegating and why; no synthesis, no preliminary\n"
            "    conclusions.  Synthesis happens in the turn AFTER results arrive.\n";
    }
    if (topic == "mem" || topic == "memory") {
        return
            "Structured memory graph (curated nodes + directed edges):\n"
            "  /mem entries [type=...] [tag=...]\n"
            "       List nodes.  Optional type filter (comma-separated) and/or tag\n"
            "       substring filter.\n"
            "  /mem entry <id>\n"
            "       Fetch one entry plus its in/out edges.  Neighbour titles inline\n"
            "       so you don't need a follow-up /mem entry per neighbour.\n"
            "  /mem search <query> [--rerank]\n"
            "       Relevance-ranked search across title, tags, content, source.\n"
            "       Top 3 hits inline a content excerpt; lower hits are one-liners.\n"
            "       --rerank routes the top-10 candidates through this agent's\n"
            "       advisor_model for a final reorder — costs one LLM call.  Use\n"
            "       on ambiguous queries where BM25 alone scored several\n"
            "       candidates close together; skip on unambiguous lookups.\n"
            "       Falls back to the FTS order if no advisor_model is configured.\n"
            "  /mem expand <id> [depth=N]\n"
            "       Fetch the subgraph around an entry.  Default depth=1, max 2;\n"
            "       capped at 50 nodes.  Replaces N+1 sequential /mem entry calls\n"
            "       when chasing a chain.\n"
            "  /mem density <id>\n"
            "       In/out edge counts, distinct relations, 2-hop reach.  Probe\n"
            "       BEFORE redundant research to skip work the graph already covers.\n"
            "  /mem add entry <type> <title> [--artifact #<id>]\n"
            "      <content body — REQUIRED, synthesised retrievable text>\n"
            "  /endmem\n"
            "       Add a typed node.  Block form: header line, body lines, /endmem\n"
            "       on its own line to close.  Optional --artifact #<id> links\n"
            "       a /write --persist'd file to the node.\n"
            "       The body is REQUIRED and must contain the substance of the\n"
            "       finding (key facts, numbers, sources) — it's the text that\n"
            "       /mem search ranks against and that /mem entry surfaces back\n"
            "       in future sessions.  Title-only entries are rejected.\n"
            "\n"
            "       Type legend — pick the one that matches what the entry IS:\n"
            "         user       durable facts about the human (role, prefs, constraints)\n"
            "         feedback   corrections / 'do this, not that' guidance from the user\n"
            "         project    active deliverables, decisions, in-flight work, briefs\n"
            "         reference  external sources you cited (papers, docs, vendor pages)\n"
            "         learning   synthesised conclusions reached from multiple sources\n"
            "         context    situational state worth retaining (current focus, blockers)\n"
            "       Default-to-`reference` is wrong for most writes.  A research-and-write\n"
            "       turn typically produces 1 project (the deliverable), N reference (the\n"
            "       cited sources), 1 learning (the recommendation/synthesis).  Filing\n"
            "       everything as reference defeats /mem entries [type=...] partitioning.\n"
            "\n"
            "       Examples (one per common type):\n"
            "           /mem add entry project Q3 observability rollout plan\n"
            "           Migrate the 30-service fleet to Honeycomb in two phases: (1) auto-\n"
            "           instrumented services first (Node.js, Python — 18 services, 1 wk),\n"
            "           (2) manual Go/Rust spans second (12 services, 2 wks).  Sampling\n"
            "           policy via Refinery on Pro tier.  Owner: backend infra.\n"
            "           /endmem\n"
            "           /mem add entry reference Honeycomb pricing page (live fetch 2026-04)\n"
            "           Pro tier $130/mo for 100M events flat.  Past 1B spans → Enterprise\n"
            "           (custom).  Refinery is a separate deployment, recommended at scale.\n"
            "           Source: honeycomb.io/pricing.\n"
            "           /endmem\n"
            "           /mem add entry learning Honeycomb wins for trace-first small teams\n"
            "           Linear pricing + OTEL portability beats the metrics/logs gap when\n"
            "           the team is small and tracing is the dominant signal.  Flips to\n"
            "           OTel+Grafana if compliance forces self-host or growth pushes past\n"
            "           1B spans/mo.\n"
            "           /endmem\n"
            "           /mem add entry context Currently picking between three obs backends\n"
            "           User is mid-decision (Datadog / Honeycomb / OTel+Grafana) for a\n"
            "           5-eng team, 30 services, 100M traces/mo.  Brief drafted; awaiting\n"
            "           answers on existing Prometheus footprint and growth curve.\n"
            "           /endmem\n"
            "  /mem add link <src_id> <relation> <dst_id>\n"
            "       Add a directed edge.  Single-line; no body.  Relations:\n"
            "       relates_to, refines, contradicts, supersedes, supports.\n"
            "  /mem invalidate <id>\n"
            "       Mark an entry as no-longer-true at the current moment.\n"
            "       Hides it from default `/mem entries|entry|search` reads;\n"
            "       the row stays in the DB and remains reachable via\n"
            "       historical reads.  Use when a recorded fact has changed\n"
            "       (user pivoted, project shipped, source contradicted).\n"
            "       Distinct from a hard delete — there is no agent surface\n"
            "       for hard delete.\n"
            "\n"
            "Per-agent scratchpad (free-form text, persistent across sessions):\n"
            "  /mem write <text>     — append a note\n"
            "  /mem read             — load scratchpad into context\n"
            "  /mem show             — display raw scratchpad\n"
            "  /mem clear            — delete scratchpad\n"
            "\n"
            "Pipeline-shared scratchpad (visible to all agents in this conversation):\n"
            "  /mem shared write <text>\n"
            "  /mem shared read\n"
            "  /mem shared clear\n";
    }
    if (topic == "artifacts" || topic == "read" || topic == "list") {
        return
            "/read <path>\n"
            "  Read a persisted artifact by path in this conversation.\n"
            "/read #<aid>\n"
            "  Read by artifact id (same conversation).\n"
            "/read #<aid> via=mem:<entry_id>\n"
            "  Read a CROSS-CONVERSATION artifact, using the memory entry that\n"
            "  links it as the access capability.  /mem entry <id> prints the\n"
            "  exact /read line to copy.  Without via=mem, cross-conversation\n"
            "  reads are denied.\n"
            "/list\n"
            "  List this conversation's persisted artifacts (path + size).\n"
            "\n"
            "Pairing pattern — file the user may want to refine later:\n"
            "  1. /write --persist <path> ... /endwrite   (note the artifact id\n"
            "     in the OK line)\n"
            "  2. /mem add entry reference <title> --artifact #<id>   (in the\n"
            "     SAME turn)\n"
            "Future /mem search finds the entry; /mem entry <id> prints the\n"
            "/read line to retrieve the file.\n";
    }
    if (topic == "mcp") {
        return
            "/mcp tools\n"
            "  List MCP tools available on the configured servers.\n"
            "/mcp call <server>.<tool> <json-args>\n"
            "  Invoke a tool.  Args are a JSON object on the same line.\n"
            "MCP servers are spawned per-request as stdio subprocesses; their\n"
            "lifetime matches the orchestrator's.\n";
    }
    if (topic == "advise" || topic == "advisor") {
        return
            "/advise <question>\n"
            "  One-shot consult against a more capable model.  The advisor sees\n"
            "  ONLY the text after /advise — no conversation history, no tool\n"
            "  results, no project context.  Self-contained question or it can't\n"
            "  help.\n"
            "\n"
            "Consult when:\n"
            "  - Sources you've gathered DISAGREE; you need to decide which to weight.\n"
            "  - Two reasonable paths trade off against constraints you can name.\n"
            "  - A multi-step plan is about to be committed; sanity-check the\n"
            "    decomposition.\n"
            "  - A claim is supportable but inference-heavy; calibrate hedge-vs-state.\n"
            "Do NOT consult for: single-fact lookups (use /search or /fetch),\n"
            "formatting/style/phrasing, anything you already know with high\n"
            "confidence, or as a substitute for doing the research yourself.\n"
            "\n"
            "Question quality — pack four things into the body:\n"
            "  1. The decision in one sentence.\n"
            "  2. The 2–3 plausible alternatives + their trade-offs.\n"
            "  3. The constraints (audience, scale, deadline, priors).\n"
            "  4. What would change your mind in either direction.\n"
            "Workflow: /advise AFTER evidence is gathered, BEFORE prose is committed.\n"
            "Budget: 2 consults per turn.  A third wanted consult means the task is\n"
            "under-scoped — deliver what you have and flag the open question instead.\n";
    }

    return std::string{};   // empty ⇒ unknown topic; caller surfaces an error
}

// ---------------------------------------------------------------------------
// html_to_text — strip tags and boilerplate, return readable text
// ---------------------------------------------------------------------------

static std::string html_to_text(const std::string& html) {
    std::string out;
    // Extracted text is typically 30-50% of raw HTML (more for content-heavy
    // pages, less for template-heavy ones).  Reserve generously so the hot
    // append path below never triggers a reallocation mid-run.
    out.reserve(html.size() / 2);

    size_t i = 0;
    const size_t n = html.size();
    const char* const data = html.data();

    // Skip script/style blocks wholesale
    auto skip_block = [&](const char* close_tag) {
        size_t pos = html.find(close_tag, i);
        i = (pos == std::string::npos) ? n : pos + std::strlen(close_tag);
    };

    bool last_was_space = true;

    // Fast path: scan ahead for a run of "plain" bytes (non-special,
    // non-whitespace) and bulk-append them.  For HTML dominated by text
    // content this takes the inner loop from one branch + one append per
    // byte to one scan plus one append per plain-text run.
    auto is_plain = [](unsigned char c) {
        return c > ' ' && c != '<' && c != '&' && c != 127;
    };

    while (i < n) {
        if (is_plain(static_cast<unsigned char>(data[i]))) {
            size_t j = i + 1;
            while (j < n && is_plain(static_cast<unsigned char>(data[j]))) ++j;
            out.append(data + i, j - i);
            last_was_space = false;
            i = j;
            continue;
        }

        if (html[i] == '<') {
            // Peek at the tag name
            size_t j = i + 1;
            while (j < n && html[j] == ' ') ++j;
            // Check for block-level tags we want to map to newlines
            auto tag_is = [&](const char* t) {
                size_t tl = std::strlen(t);
                return n - j >= tl &&
                       ::strncasecmp(html.c_str() + j, t, tl) == 0 &&
                       (j + tl >= n || html[j + tl] == '>' || html[j + tl] == ' ');
            };
            if (tag_is("script") || tag_is("style") || tag_is("noscript")) {
                // Find the matching close tag
                size_t close = html.find('>', i);
                if (close != std::string::npos) i = close + 1;
                // Now skip until </script>, </style>, </noscript>
                if (tag_is("script"))   { skip_block("</script>");   continue; }
                if (tag_is("style"))    { skip_block("</style>");    continue; }
                if (tag_is("noscript")) { skip_block("</noscript>"); continue; }
            }
            bool block = tag_is("p")  || tag_is("/p")  ||
                         tag_is("br") || tag_is("li")  ||
                         tag_is("h1") || tag_is("h2")  || tag_is("h3") ||
                         tag_is("h4") || tag_is("h5")  || tag_is("h6") ||
                         tag_is("div") || tag_is("/div") ||
                         tag_is("tr") || tag_is("td")  || tag_is("th");
            // Skip to end of tag
            while (i < n && html[i] != '>') ++i;
            if (i < n) ++i;
            if (block && !out.empty() && out.back() != '\n') {
                out += '\n';
                last_was_space = true;
            }
        } else if (html[i] == '&') {
            // Basic HTML entity decoding
            if (n - i >= 4 && html.substr(i, 4) == "&lt;")       { out += '<'; i += 4; }
            else if (n - i >= 4 && html.substr(i, 4) == "&gt;")  { out += '>'; i += 4; }
            else if (n - i >= 5 && html.substr(i, 5) == "&amp;") { out += '&'; i += 5; }
            else if (n - i >= 6 && html.substr(i, 6) == "&nbsp;"){ out += ' '; i += 6; last_was_space = true; }
            else { out += html[i++]; last_was_space = false; }
        } else {
            char c = html[i++];
            if (c == '\r') continue;
            bool is_ws = (c == ' ' || c == '\t' || c == '\n');
            if (is_ws) {
                if (!last_was_space && !out.empty()) {
                    out += (c == '\n') ? '\n' : ' ';
                    last_was_space = true;
                }
            } else {
                out += c;
                last_was_space = false;
            }
        }
    }

    // Collapse runs of blank lines to a single blank line
    std::string compressed;
    compressed.reserve(out.size());
    int consecutive_newlines = 0;
    for (char c : out) {
        if (c == '\n') {
            ++consecutive_newlines;
            if (consecutive_newlines <= 2) compressed += c;
        } else {
            consecutive_newlines = 0;
            compressed += c;
        }
    }

    return compressed;
}

// ---------------------------------------------------------------------------
// cmd_fetch  (libcurl — no shell invocation)
// ---------------------------------------------------------------------------

namespace {
struct FetchBuffer {
    std::string data;
    // Per-instance cap so the same buffer struct serves both cmd_fetch
    // (text-only HTML, 512 KB cap) and cmd_fetch_bytes (raw bytes, larger
    // cap for images / binary).  Default keeps the legacy text behaviour.
    size_t      max_size = 512 * 1024;
};

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<FetchBuffer*>(userdata);
    size_t bytes = size * nmemb;
    if (buf->data.size() + bytes > buf->max_size) return 0;
    buf->data.append(ptr, bytes);
    return bytes;
}

// SSRF guard: reject private, loopback, link-local, CGNAT, multicast,
// reserved, and cloud-metadata-adjacent addresses.  Applied per-connection
// via CURLOPT_OPENSOCKETFUNCTION, so redirects are re-validated too.
static bool is_blocked_v4(uint32_t ip_host_order) {
    const uint8_t a = (ip_host_order >> 24) & 0xff;
    const uint8_t b = (ip_host_order >> 16) & 0xff;
    if (a == 0)   return true;                       // 0.0.0.0/8
    if (a == 10)  return true;                       // 10/8 RFC1918
    if (a == 127) return true;                       // loopback
    if (a == 169 && b == 254) return true;           // link-local + AWS metadata
    if (a == 172 && b >= 16 && b <= 31) return true; // 172.16/12 RFC1918
    if (a == 192 && b == 168) return true;           // 192.168/16 RFC1918
    if (a == 100 && b >= 64 && b <= 127) return true;// 100.64/10 CGNAT
    if (a >= 224) return true;                       // multicast + reserved
    return false;
}

// IPv6 SSRF guard.  IN6_IS_ADDR_* macros from libc cover loopback/link-
// local/v4-mapped/multicast but miss several routable transition prefixes
// that re-encode IPv4 — without explicit checks below, a 6to4 literal like
// http://[2002:7f00:1::]/ or http://[2002:a9fe:a9fe::]/ resolves to v6 at
// connect time but actually reaches 127.0.0.1 / 169.254.169.254 (AWS IMDS)
// via a 6to4 relay if v6 egress is open.  Same for Teredo (2001::/32).
// We do explicit prefix checks instead of relying solely on libc macros.
static bool is_blocked_v6(const struct in6_addr* a6) {
    const uint8_t* b = (const uint8_t*)a6;

    // ::/128 unspecified, ::1/128 loopback
    if (IN6_IS_ADDR_UNSPECIFIED(a6)) return true;
    if (IN6_IS_ADDR_LOOPBACK(a6))    return true;
    // ::ffff:a.b.c.d — IPv4-mapped — recurse on embedded v4
    if (IN6_IS_ADDR_V4MAPPED(a6)) {
        uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
        return is_blocked_v4(ip);
    }
    // ::a.b.c.d — IPv4-compatible (deprecated but routable on some stacks)
    {
        bool zero_high = true;
        for (int i = 0; i < 12; ++i) if (b[i]) { zero_high = false; break; }
        if (zero_high && (b[12] || b[13] || b[14] || b[15])) {
            uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
            return is_blocked_v4(ip);
        }
    }
    // 64:ff9b::/96 NAT64 well-known + 64:ff9b:1::/48 local-use NAT64
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
        if (b[4] == 0x00 && b[5] == 0x00 &&
            b[6] == 0x00 && b[7] == 0x00 &&
            b[8] == 0x00 && b[9] == 0x00 &&
            b[10] == 0x00 && b[11] == 0x00) {
            uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
            return is_blocked_v4(ip);
        }
        // 64:ff9b:1::/48 — extract embedded IPv4 from low 32 bits
        if (b[4] == 0x00 && b[5] == 0x01) {
            uint32_t ip = ntohl(((const uint32_t*)a6)[3]);
            return is_blocked_v4(ip);
        }
    }
    // 100::/64 discard-only address block
    if (b[0] == 0x01 && b[1] == 0x00 &&
        b[2] == 0x00 && b[3] == 0x00 &&
        b[4] == 0x00 && b[5] == 0x00 &&
        b[6] == 0x00 && b[7] == 0x00) return true;
    // 2001::/32 Teredo — embeds an IPv4 in bytes 12..15 (XOR'd with 0xff)
    if (b[0] == 0x20 && b[1] == 0x01 &&
        b[2] == 0x00 && b[3] == 0x00) {
        uint32_t ip = ntohl(((const uint32_t*)a6)[3]) ^ 0xffffffffu;
        if (is_blocked_v4(ip)) return true;
    }
    // 2001:db8::/32 documentation
    if (b[0] == 0x20 && b[1] == 0x01 &&
        b[2] == 0x0d && b[3] == 0xb8) return true;
    // 2002::/16 6to4 — IPv4 lives in bytes 2..5
    if (b[0] == 0x20 && b[1] == 0x02) {
        uint32_t ip = (uint32_t(b[2]) << 24) | (uint32_t(b[3]) << 16) |
                      (uint32_t(b[4]) << 8)  |  uint32_t(b[5]);
        if (is_blocked_v4(ip)) return true;
    }
    if (IN6_IS_ADDR_LINKLOCAL(a6)) return true;
    if (IN6_IS_ADDR_SITELOCAL(a6)) return true;     // fec0::/10 deprecated
    if (IN6_IS_ADDR_MULTICAST(a6)) return true;
    if ((b[0] & 0xfe) == 0xfc)     return true;     // fc00::/7 unique-local
                                                      // (catches fd00:ec2::254 etc.)
    return false;
}

static bool is_blocked_address(const struct sockaddr* sa) {
    if (!sa) return true;
    if (sa->sa_family == AF_INET) {
        uint32_t ip = ntohl(((const struct sockaddr_in*)sa)->sin_addr.s_addr);
        return is_blocked_v4(ip);
    }
    if (sa->sa_family == AF_INET6) {
        return is_blocked_v6(&((const struct sockaddr_in6*)sa)->sin6_addr);
    }
    return true;
}

static curl_socket_t safe_opensocket_cb(void* /*clientp*/, curlsocktype purpose,
                                        struct curl_sockaddr* addr) {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    if (is_blocked_address(&addr->addr)) return CURL_SOCKET_BAD;
    return ::socket(addr->family, addr->socktype, addr->protocol);
}

// Extract the host component from an http(s) URL.  Handles userinfo
// (`http://user@host/`), bracketed IPv6 literals, and ports.  Returns
// empty on shapes that don't look like an absolute http/https URL —
// the caller treats empty as "block by default."  We lowercase and
// strip a trailing dot so "Metadata.Google.Internal." normalises.
static std::string url_host(const std::string& url) {
    auto sep = url.find("://");
    if (sep == std::string::npos) return {};
    size_t start = sep + 3;
    // Path / query / fragment terminate the authority.
    size_t end = url.size();
    for (size_t i = start; i < url.size(); ++i) {
        if (url[i] == '/' || url[i] == '?' || url[i] == '#') { end = i; break; }
    }
    std::string authority = url.substr(start, end - start);
    auto at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);

    std::string host;
    if (!authority.empty() && authority.front() == '[') {
        auto rb = authority.find(']');
        if (rb == std::string::npos) return {};
        host = authority.substr(1, rb - 1);     // bare IPv6, no brackets
    } else {
        auto colon = authority.find(':');
        host = authority.substr(0, colon);
    }
    // Lowercase ASCII + strip trailing dots so "metadata.google.internal."
    // normalises to "metadata.google.internal".
    for (auto& c : host) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    while (!host.empty() && host.back() == '.') host.pop_back();
    return host;
}

// Hostname denylist for cloud-provider metadata services.  IP-layer
// guards catch the canonical 169.254.169.254 / fd00:ec2::254 only
// when DNS actually returns those addresses.  A split-horizon resolver
// or future provider alias can still resolve these names to *some*
// routable IP — refuse the name outright.
static bool is_blocked_metadata_host(const std::string& host) {
    static const char* kBlocked[] = {
        "metadata.google.internal",
        "metadata.goog",
        "metadata",
        "instance-data",
        "instance-data.ec2.internal",
        "kubernetes.default.svc",
        "kubernetes.default.svc.cluster.local",
    };
    for (const auto* h : kBlocked) {
        if (host == h) return true;
    }
    return false;
}

// Pre-flight SSRF check by hostname resolution.  Used by callers that
// hand the URL off to an external resolver (Playwright via /browse,
// MCP children, etc.) where the safe_opensocket_cb hook cannot
// intervene.  This is best-effort: there is an unavoidable TOCTOU
// between our resolution here and the downstream's resolution at
// connect time, but it catches literal-IP attacks and DNS that
// always returns blocked addresses, which closes the obvious holes.
//
// Returns empty string on success; on rejection returns a short
// reason suitable to splice into an "ERR: refused — ..." message.
static std::string preflight_ssrf_check(const std::string& url) {
    const bool is_http  = url.size() >= 7 && url.compare(0, 7, "http://")  == 0;
    const bool is_https = url.size() >= 8 && url.compare(0, 8, "https://") == 0;
    if (!is_http && !is_https) return "URL must start with http:// or https://";

    std::string host = url_host(url);
    if (host.empty()) return "could not parse host from URL";

    if (is_blocked_metadata_host(host))
        return "host on metadata-service denylist";

    // Literal-IP fast paths.  inet_pton catches every numeric form
    // (decimal/hex/octal triggers EINVAL on most libc — those are
    // the curl-canonical-but-not-libc cases we actually want to fail
    // closed on; getaddrinfo below will resolve them as hostnames
    // and likely return the corresponding loopback/private IP).
    struct in_addr v4{};
    struct in6_addr v6{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        if (is_blocked_v4(ntohl(v4.s_addr)))
            return "literal IPv4 address resolves to a blocked range";
    } else if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        if (is_blocked_v6(&v6))
            return "literal IPv6 address resolves to a blocked range";
    }

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        // Fail closed: if we can't resolve, we can't validate.
        if (res) freeaddrinfo(res);
        return std::string("could not resolve host: ") + gai_strerror(rc);
    }
    bool any_blocked = false;
    for (auto* p = res; p; p = p->ai_next) {
        if (is_blocked_address(p->ai_addr)) { any_blocked = true; break; }
    }
    freeaddrinfo(res);
    if (any_blocked)
        return "host resolves to a private, loopback, link-local, or "
               "metadata-adjacent address";
    return {};
}
} // namespace

// Bytes-returning fetch.  Shares all the SSRF / protocol / TLS hardening
// of cmd_fetch but skips the HTML→text transform and surfaces the upstream
// Content-Type so callers can dispatch by media type (text vs image vs …).
FetchedResource cmd_fetch_bytes(const std::string& url, int64_t max_bytes) {
    FetchedResource r;

    const bool is_http  = url.size() >= 7 && url.compare(0, 7, "http://")  == 0;
    const bool is_https = url.size() >= 8 && url.compare(0, 8, "https://") == 0;
    if (!is_http && !is_https) {
        r.error = "URL must start with http:// or https://";
        return r;
    }

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "failed to initialize curl"; return r; }

    // Capture both body bytes and the Content-Type header.  libcurl's
    // header callback fires once per header line; we sniff for the
    // Content-Type prefix and stash the value's first segment (everything
    // before any `;` parameter) lowercased so callers can string-compare.
    auto header_cb = +[](char* buf, size_t sz, size_t n, void* ud) -> size_t {
        size_t total = sz * n;
        auto* out = static_cast<std::string*>(ud);
        if (total >= 14 && (
                strncasecmp(buf, "Content-Type:", 13) == 0)) {
            const char* v = buf + 13;
            size_t vl = total - 13;
            // Trim leading SP/TAB.
            while (vl > 0 && (*v == ' ' || *v == '\t')) { ++v; --vl; }
            // Stop at ';' (parameters) or end-of-line.
            size_t end = 0;
            while (end < vl && v[end] != ';' && v[end] != '\r' &&
                   v[end] != '\n') ++end;
            out->assign(v, end);
            // Lowercase ASCII for stable matching.
            for (auto& c : *out)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        }
        return total;
    };

    FetchBuffer buf;
    buf.max_size = static_cast<size_t>(max_bytes);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r.content_type);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(max_bytes));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, safe_opensocket_cb);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res == CURLE_COULDNT_CONNECT) {
        r.error = "refused — target resolved to a private, loopback, or "
                  "link-local address (SSRF guard)";
        return r;
    }
    if (res == CURLE_FILESIZE_EXCEEDED) {
        r.error = "response exceeds size cap of " +
                  std::to_string(max_bytes) + " bytes";
        return r;
    }
    if (res != CURLE_OK) {
        r.error = curl_easy_strerror(res);
        return r;
    }
    if (http_code < 200 || http_code >= 300) {
        r.error = "HTTP " + std::to_string(http_code);
        return r;
    }

    r.ok         = true;
    r.body       = std::move(buf.data);
    r.byte_count = static_cast<int64_t>(r.body.size());
    return r;
}

// Standard base64 alphabet, RFC 4648 §4.  We hand-roll instead of pulling
// EVP_EncodeBlock because the OpenSSL header pulls in a lot of state and
// the alphabet is six lines.  Pads to a multiple of 4 with '=' as the
// providers' wire shapes all expect.
std::string base64_encode(const std::string& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    if (bytes.empty()) return out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();

    while (i + 3 <= n) {
        uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) |
                     uint32_t(p[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6)  & 0x3F]);
        out.push_back(kAlphabet[ v        & 0x3F]);
        i += 3;
    }
    if (i < n) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        if (i + 1 < n) {
            out.push_back(kAlphabet[(v >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

std::string cmd_fetch(const std::string& url) {
    // Allocation-free prefix check — substr() would build two temporaries
    // per fetch just to throw them away.
    const bool is_http  = url.size() >= 7 && url.compare(0, 7, "http://")  == 0;
    const bool is_https = url.size() >= 8 && url.compare(0, 8, "https://") == 0;
    if (!is_http && !is_https)
        return "ERR: URL must start with http:// or https://";

    CURL* curl = curl_easy_init();
    if (!curl) return "ERR: failed to initialize curl";

    FetchBuffer buf;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, static_cast<long>(512 * 1024));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // SSRF guard: reject private/loopback/metadata addresses on every
    // connection attempt, including after redirects.
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, safe_opensocket_cb);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res == CURLE_COULDNT_CONNECT)
        return "ERR: refused — target resolved to a private, loopback, or "
               "link-local address (SSRF guard)";
    if (res != CURLE_OK)
        return std::string("ERR: ") + curl_easy_strerror(res);

    return html_to_text(buf.data);
}

// ---------------------------------------------------------------------------
// cmd_exec
// ---------------------------------------------------------------------------

std::string cmd_exec(const std::string& command, bool confirmed) {
    static constexpr size_t kMaxOutput = 32768;

    if (command.empty()) return "ERR: empty command";

    if (!confirmed && is_destructive_exec(command))
        return "ERR: destructive command blocked — requires confirmation";

    // Capture stdout and stderr together
    std::string shell_cmd = command + " 2>&1";
    FILE* pipe = popen(shell_cmd.c_str(), "r");
    if (!pipe) return "ERR: popen failed";

    std::string output;
    output.reserve(4096);
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
        if (output.size() > kMaxOutput) {
            output.resize(kMaxOutput);
            output += "\n... [truncated at 32 KB]";
            break;
        }
    }
    int status = pclose(pipe);

    // Trim trailing newlines
    while (!output.empty() && output.back() == '\n')
        output.pop_back();

    if (output.empty()) output = "(no output)";

    if (status != 0) {
        output += "\n[exit " + std::to_string(status) + "]";
    }

    return output;
}

// ---------------------------------------------------------------------------
// cmd_write
// ---------------------------------------------------------------------------

std::string cmd_write(const std::string& path, const std::string& content) {
    if (path.empty()) return "ERR: empty path";

    // Path safety: canonicalize and verify the resolved path stays within cwd.
    // The cwd itself is canonicalised so symlinked ancestors (common on macOS,
    // where /tmp → /private/tmp) don't cause a false mismatch.
    std::error_code path_ec;
    fs::path cwd_raw = fs::current_path(path_ec);
    if (path_ec) return "ERR: cannot determine working directory";
    fs::path cwd = fs::canonical(cwd_raw, path_ec);
    if (path_ec) cwd = cwd_raw;  // fall back if cwd itself fails

    // For the target path we canonicalise the deepest existing ancestor (so
    // symlink tricks like a symlinked parent dir are resolved) and append the
    // remaining tail.  This is the symlink-aware analog of weakly_canonical —
    // which only normalises `.`/`..` and leaves symlinks alone.
    fs::path target(path);
    fs::path abs_target = target.is_absolute() ? target : (cwd / target);
    fs::path existing = abs_target;
    fs::path tail;
    while (!existing.empty()) {
        std::error_code ec;
        if (fs::exists(existing, ec)) break;
        tail = existing.filename() / tail;
        if (!existing.has_parent_path() || existing.parent_path() == existing) {
            existing.clear();
            break;
        }
        existing = existing.parent_path();
    }
    fs::path resolved;
    if (!existing.empty()) {
        std::error_code ec;
        fs::path canon = fs::canonical(existing, ec);
        if (ec) return "ERR: invalid path: " + ec.message();
        resolved = tail.empty() ? canon : (canon / tail);
    } else {
        resolved = fs::weakly_canonical(abs_target, path_ec);
        if (path_ec) return "ERR: invalid path: " + path_ec.message();
    }
    resolved = resolved.lexically_normal();

    // Resolved path must be a prefix of (or equal to) cwd
    auto resolved_str = resolved.string();
    auto cwd_str = cwd.string();
    if (resolved_str.size() < cwd_str.size() ||
        resolved_str.compare(0, cwd_str.size(), cwd_str) != 0 ||
        (resolved_str.size() > cwd_str.size() && resolved_str[cwd_str.size()] != '/'))
        return "ERR: path escapes project directory";

    // Create parent directories
    fs::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (ec) return "ERR: cannot create directories: " + ec.message();
    }

    // Back up existing file before overwriting.
    bool overwrite = false;
    std::string bak_note;
    if (fs::exists(p)) {
        overwrite = true;
        std::string bak = path + ".bak";
        std::error_code ec;
        fs::copy_file(p, bak, fs::copy_options::overwrite_existing, ec);
        if (!ec) bak_note = " (previous saved to " + bak + ")";
    }

    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) {
        int err = errno;
        return std::string("ERR: cannot open for writing: ") + path
             + " (" + std::strerror(err) + ")";
    }

    f << content;
    if (!content.empty() && content.back() != '\n') f << '\n';

    if (f.fail()) {
        int err = errno;
        std::string why = std::strerror(err);
        if (err == ENOSPC) why = "disk full (ENOSPC)";
        else if (err == EDQUOT) why = "disk quota exceeded (EDQUOT)";
        else if (err == EACCES) why = "permission denied (EACCES)";
        return "ERR: write failed: " + path + " — " + why;
    }
    f.close();

    // Verify: read the file back and compare bytes.  Catches silent
    // disk-level truncation (partial fs writes, ENOSPC, etc.) and gives
    // the agent a concrete signal when the file on disk doesn't match
    // what it intended to write.
    size_t expected = content.size();
    if (!content.empty() && content.back() != '\n') ++expected;  // we added one

    std::ifstream v(path, std::ios::binary | std::ios::ate);
    if (!v.is_open())
        return "ERR: wrote " + std::to_string(content.size())
             + " bytes to " + path + " but cannot re-open to verify";

    size_t on_disk = static_cast<size_t>(v.tellg());
    v.close();

    if (on_disk != expected)
        return "ERR: write verification failed for " + path
             + " (expected " + std::to_string(expected)
             + " bytes, found " + std::to_string(on_disk) + ")";

    std::string action = overwrite ? "overwrote" : "wrote";
    return "OK: " + action + " " + std::to_string(content.size())
           + " bytes to " + path + bak_note + " (verified)";
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

namespace {

// Resolve memory_dir through any symlinks and ensure the result is a real
// directory we own.  Protects against symlink traversal: a malicious
// `.arbiter/memory` pointing at `/etc` would otherwise let /mem write happily
// clobber arbitrary files.  Creates the directory if missing, then
// canonicalises, then verifies ownership.  Returns the canonical path or an
// "ERR: ..." string on any failure.
static std::string resolve_memory_dir(const std::string& memory_dir,
                                      std::string* err_out) {
    auto fail = [&](std::string msg) {
        if (err_out) *err_out = std::move(msg);
        return std::string();
    };
    if (memory_dir.empty()) return fail("ERR: empty memory_dir");

    std::error_code ec;
    fs::create_directories(memory_dir, ec);
    if (ec) return fail("ERR: cannot create memory dir " + memory_dir
                        + ": " + ec.message());

    fs::path canon = fs::canonical(memory_dir, ec);
    if (ec) return fail("ERR: cannot resolve memory dir: " + ec.message());

    struct stat st{};
    if (::lstat(canon.c_str(), &st) != 0)
        return fail(std::string("ERR: lstat memory dir: ") + std::strerror(errno));
    if (!S_ISDIR(st.st_mode))
        return fail("ERR: memory dir is not a directory: " + canon.string());
    if (st.st_uid != ::geteuid())
        return fail("ERR: memory dir not owned by current user: " + canon.string());

    return canon.string();
}

// True when `path` exists and is a regular file owned by the current user —
// the only kind of target /mem and /shared-mem append-writes should touch.
// Used to abort before open() rather than append to a symlink or a file
// owned by someone else in a shared-home scenario.
static std::string verify_mem_target(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return "";        // fine — open(O_APPEND) will create
        return std::string("ERR: lstat ") + path + ": " + std::strerror(errno);
    }
    if (S_ISLNK(st.st_mode))
        return "ERR: refusing to write through symlink: " + path;
    if (!S_ISREG(st.st_mode))
        return "ERR: memory target is not a regular file: " + path;
    if (st.st_uid != ::geteuid())
        return "ERR: memory file not owned by current user: " + path;
    return "";
}

} // namespace

std::string cmd_mem_read(const std::string& agent_id, const std::string& memory_dir) {
    std::string err;
    std::string dir = resolve_memory_dir(memory_dir, &err);
    if (dir.empty()) return "";   // read is non-fatal; silent on access issues

    std::string path = dir + "/" + agent_id + ".md";
    if (!verify_mem_target(path).empty()) return "";
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string cmd_mem_write(const std::string& agent_id, const std::string& text,
                          const std::string& memory_dir) {
    std::string err;
    std::string dir = resolve_memory_dir(memory_dir, &err);
    if (dir.empty()) return err;

    std::string path = dir + "/" + agent_id + ".md";
    std::string verr = verify_mem_target(path);
    if (!verr.empty()) return verr;

    std::ofstream f(path, std::ios::app);
    if (!f.is_open())
        return std::string("ERR: cannot open ") + path + " for writing ("
             + std::strerror(errno) + ")";

    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    f << "\n<!-- " << ts << " -->\n" << text << "\n";
    if (f.fail()) {
        int e = errno;
        std::string why = std::strerror(e);
        if (e == ENOSPC) why = "disk full (ENOSPC)";
        return "ERR: write failed for " + path + " — " + why;
    }
    // Keep per-user memory files unreadable to other users on shared systems.
    ::chmod(path.c_str(), 0600);
    return "OK: memory written to " + path;
}

void cmd_mem_clear(const std::string& agent_id, const std::string& memory_dir) {
    std::string err;
    std::string dir = resolve_memory_dir(memory_dir, &err);
    if (dir.empty()) return;
    std::string path = dir + "/" + agent_id + ".md";
    if (verify_mem_target(path).empty()) fs::remove(path);
}

// ---------------------------------------------------------------------------
// Shared scratchpad — pipeline-scoped, visible to all agents
// ---------------------------------------------------------------------------

static std::string shared_mem_path(const std::string& memory_dir) {
    return memory_dir + "/shared.md";
}

std::string cmd_mem_shared_read(const std::string& memory_dir) {
    std::string err;
    std::string dir = resolve_memory_dir(memory_dir, &err);
    if (dir.empty()) return "";
    std::string path = dir + "/shared.md";
    if (!verify_mem_target(path).empty()) return "";
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string cmd_mem_shared_write(const std::string& text, const std::string& memory_dir) {
    std::string err;
    std::string dir = resolve_memory_dir(memory_dir, &err);
    if (dir.empty()) return err;
    std::string path = dir + "/shared.md";
    std::string verr = verify_mem_target(path);
    if (!verr.empty()) return verr;
    std::ofstream f(path, std::ios::app);
    if (!f.is_open())
        return std::string("ERR: cannot open shared scratchpad: ") + std::strerror(errno);
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    f << "\n<!-- " << ts << " -->\n" << text << "\n";
    if (f.fail()) {
        int e = errno;
        std::string why = std::strerror(e);
        if (e == ENOSPC) why = "disk full (ENOSPC)";
        return "ERR: write failed for shared scratchpad — " + why;
    }
    ::chmod(path.c_str(), 0600);
    return "OK";
}

std::string cmd_mem_shared_clear(const std::string& memory_dir) {
    std::string err;
    std::string dir = resolve_memory_dir(memory_dir, &err);
    if (dir.empty()) return err;
    std::string path = dir + "/shared.md";
    if (!verify_mem_target(path).empty()) return "OK";  // nothing safe to remove
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) return "ERR: cannot clear shared scratchpad: " + ec.message();
    return "OK";
}

// ---------------------------------------------------------------------------
// execute_agent_commands
// ---------------------------------------------------------------------------

// Conservative pattern check — matches commonly-destructive shell forms.
// Expanding this is cheap; the callsite bails to a y/N prompt on a match.
bool is_destructive_exec(const std::string& cmd) {
    // Shell-meta bypass catch-all.  Without this, an agent can sneak a
    // destructive call past the token scan via command substitution
    // (`$(rm -rf ~)`, backticks), chaining (`true; rm -rf ~`), or a literal
    // newline.  Tripping any of these routes the command through the
    // confirm() gate — agents can still use pipes/redirects after the user
    // approves, but the silent bypass vector is closed.
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (c == '`' || c == '\n' || c == '\r') return true;
        if (c == '$' && i + 1 < cmd.size() && cmd[i + 1] == '(') return true;
        if (c == ';') return true;
        if ((c == '&' || c == '|') && i + 1 < cmd.size() && cmd[i + 1] == c)
            return true;  // && or ||
    }

    // Normalise: lowercase + collapse whitespace, preserve literal chars like '>'.
    std::string s;
    s.reserve(cmd.size());
    bool prev_space = true;
    for (char c : cmd) {
        char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        if (lc == ' ' || lc == '\t') {
            if (!prev_space) { s += ' '; prev_space = true; }
        } else {
            s += lc;
            prev_space = false;
        }
    }
    if (!s.empty() && s.back() == ' ') s.pop_back();
    // Pad with spaces so word-boundary checks (" rm ") hit tokens at the ends.
    std::string padded = " " + s + " ";

    auto has = [&](const char* needle) {
        return padded.find(needle) != std::string::npos;
    };

    // Destructive filesystem + process tools.
    if (has(" rm ") || has(" rmdir ") || has(" unlink ") ||
        has(" shred ") || has(" truncate ") ||
        has(" dd ")  || has(" mkfs")      || has(" wipefs") ||
        has(" fdisk ") || has(" parted ") ||
        has(" chmod -r") || has(" chown -r")) return true;

    // Privilege escalation — anything can happen past sudo.
    if (has(" sudo ") || has(" doas ")) return true;

    // Shell redirection overwrites/appends files.  We gate both since an
    // agent-issued `>` is the moral equivalent of /write.
    if (padded.find(" > ") != std::string::npos ||
        padded.find(">>") != std::string::npos) return true;

    // Piped commands reach a second tool whose token isn't at the command
    // head, so the " rm " / " dd " checks above won't see it.  Treat any
    // pipe as confirm-worthy — the user gets one prompt for the whole chain.
    if (padded.find(" | ") != std::string::npos) return true;

    // find -delete / find -exec rm …
    if (has(" find ") && (has(" -delete") ||
                          padded.find(" -exec rm") != std::string::npos ||
                          padded.find(" -execdir rm") != std::string::npos))
        return true;

    // Git operations that rewrite or discard history.
    if (has(" git ")) {
        if (has(" reset --hard") || has(" clean -f") ||
            has(" push --force") || has(" push -f ") ||
            has(" branch -d") || has(" branch -D") ||
            has(" checkout --") || has(" restore .")) return true;
    }

    return false;
}

bool is_tool_result_failure(const std::string& block) {
    // ERR: / UPSTREAM FAILED / SKIPPED: anywhere in the block flags failure.
    // Also catch exec's "[exit N]" trailer for N != 0, which cmd_exec appends
    // after non-zero exit status.
    if (block.find("ERR:") != std::string::npos) return true;
    if (block.find("UPSTREAM FAILED") != std::string::npos) return true;
    if (block.find("SKIPPED:") != std::string::npos) return true;
    if (block.find("TRUNCATED:") != std::string::npos) return true;
    // cmd_exec annotates non-zero exits as "\n[exit N]\n".  Zero exits are
    // silent, so any "[exit <digit>" that isn't "[exit 0" is a failure.
    size_t pos = 0;
    while ((pos = block.find("[exit ", pos)) != std::string::npos) {
        size_t end = pos + 6;
        if (end < block.size() && block[end] != '0') return true;
        pos = end;
    }
    return false;
}

std::string execute_agent_commands(const std::vector<AgentCommand>& cmds,
                                   const std::string& agent_id,
                                   const std::string& memory_dir,
                                   AgentInvoker agent_invoker,
                                   ConfirmFn    confirm,
                                   std::map<std::string, std::string>* dedup_cache,
                                   AdvisorInvoker advisor_invoker,
                                   ToolStatusFn   tool_status,
                                   PaneSpawner    pane_spawner,
                                   WriteInterceptor write_interceptor,
                                   bool           exec_disabled,
                                   ParallelInvoker parallel_invoker,
                                   StructuredMemoryReader structured_memory_reader,
                                   StructuredMemoryWriter structured_memory_writer,
                                   MCPInvoker     mcp_invoker,
                                   MemoryScratchpadInvoker memory_scratchpad,
                                   SearchInvoker  search_invoker,
                                   ArtifactWriter artifact_writer,
                                   ArtifactReader artifact_reader,
                                   ArtifactLister artifact_lister,
                                   A2AInvoker     a2a_invoker,
                                   SchedulerInvoker scheduler_invoker,
                                   const std::vector<std::string>& capabilities,
                                   std::vector<ContentPart>* out_image_parts) {
    std::ostringstream out;
    out << "\n";

    // Caps: 16 KB per fetch (stripped text), max 3 fetches per turn,
    // and a total tool-result budget of 32 KB.  /search shares the
    // body cap but has its own per-turn budget — search results are
    // small and cheap relative to fetch, and agents typically need
    // 1–2 searches before doing 1–3 follow-up fetches.
    static constexpr size_t kPerFetchLimit  = 16384;
    static constexpr size_t kTotalLimit     = 32768;
    static constexpr int    kMaxFetches     = 3;
    static constexpr int    kMaxSearches    = 4;
    // README: "Consults are capped at two per turn — a third desired
    // call means the task is under-scoped, and the executor is told to
    // deliver what it has."  Without this counter the cap was prompt-
    // text only; an adversarial or misbehaving agent could fan out
    // arbitrary advisor calls and run up the cost ledger.
    static constexpr int    kMaxAdvise      = 2;
    int fetch_count  = 0;
    int search_count = 0;
    int advise_count = 0;

    // Resolve the calling agent's capability bundles once.  Empty input
    // means "all bundles" (legacy master-orchestrator behavior).  This
    // mirrors resolve_bundles() in constitution.cpp — the prompt and the
    // dispatcher must agree on the surface, otherwise we'd promise the
    // agent a tool we then refuse to run.
    std::set<std::string> allowed_bundles;
    const bool unrestricted = capabilities.empty();
    if (!unrestricted) {
        for (const auto& cap : capabilities) {
            if      (cap == "/search" || cap == "/fetch" || cap == "/browse")
                allowed_bundles.insert("web");
            else if (cap == "/exec")
                allowed_bundles.insert("exec");
            else if (cap.rfind("/write", 0) == 0)
                allowed_bundles.insert("write");
            else if (cap == "/read" || cap == "/list")
                allowed_bundles.insert("read");
            else if (cap.rfind("/mem", 0) == 0)
                allowed_bundles.insert("mem");
            else if (cap == "/agent" || cap == "/parallel" || cap == "/pane")
                allowed_bundles.insert("delegation");
            else if (cap == "/mcp")
                allowed_bundles.insert("mcp");
            else if (cap == "/a2a")
                allowed_bundles.insert("a2a");
            else if (cap == "/advise")
                allowed_bundles.insert("advise");
        }
    }

    auto bundle_of = [](const std::string& name) -> const char* {
        if (name == "fetch" || name == "search" || name == "browse") return "web";
        if (name == "exec")                                          return "exec";
        if (name == "write")                                         return "write";
        if (name == "read"  || name == "list")                       return "read";
        if (name == "mem")                                           return "mem";
        if (name == "agent" || name == "parallel" || name == "pane") return "delegation";
        if (name == "mcp")                                           return "mcp";
        if (name == "a2a")                                           return "a2a";
        if (name == "advise")                                        return "advise";
        return "";   // "help" and unknowns are always allowed
    };

    for (auto& cmd : cmds) {
        // Enforce total budget
        if (out.tellp() >= static_cast<std::streampos>(kTotalLimit)) {
            out << "[TOOL RESULTS TRUNCATED: budget exhausted]\n\n";
            break;
        }

        // ── Capability gate ─────────────────────────────────────────────
        // If the calling agent declared a non-empty `capabilities` list,
        // reject any slash command whose bundle isn't in that list.  The
        // empty list keeps the master-orchestrator's all-bundles default
        // intact.  Without this check the `capabilities` array is purely
        // a prompt-text hint — a prompt-injected or jailbroken agent can
        // emit /exec / /write / /fetch even if its constitution lists
        // only /search.
        if (!unrestricted) {
            const char* needed = bundle_of(cmd.name);
            if (*needed && !allowed_bundles.count(needed)) {
                out << "[/" << cmd.name << " " << cmd.args << "]\n"
                    << "ERR: capability not granted — this agent's "
                    << "`capabilities` does not include /" << cmd.name
                    << " (bundle '" << needed << "').  Adapt: stop "
                    << "emitting /" << cmd.name
                    << " or delegate via /agent to one that has it.\n"
                    << "[END " << cmd.name << "]\n\n";
                if (tool_status) tool_status(cmd.name, false);
                continue;
            }
        }

        // ── Dedup gate ────────────────────────────────────────────────────
        // If the same (cmd, args[, content]) already ran this turn or by a
        // prior agent in the pipeline, replay the cached result instead of
        // re-executing.  This shares fetch/exec results across delegation
        // boundaries when the shared_cache propagates from the orchestrator.
        std::string dedup_key = cmd.name + "|" + cmd.args;
        if (cmd.name == "write") {
            dedup_key += "|" + std::to_string(std::hash<std::string>{}(cmd.content));
        }
        if (dedup_cache && dedup_cache->find(dedup_key) != dedup_cache->end()) {
            const auto& cached = (*dedup_cache)[dedup_key];
            out << cached;
            // Dedup hits still count toward the turn's tool-call tally; the
            // model emitted a /cmd, so the user's "(N tool calls…)" indicator
            // should reflect that.  ok/fail mirrors the cached block.
            if (tool_status) tool_status(cmd.name, !is_tool_result_failure(cached));
            continue;
        }

        // Capture each command's full block (header + body + footer) in a
        // local stream so we can feed it to the dedup cache after it runs.
        std::ostringstream block;

        // Set to false when the result shouldn't be cached for dedup — e.g.
        // fetch-budget-skip, or user-declined confirms: a later re-issue is
        // a legitimate retry, not a duplicate.
        bool cache_result = true;

        if (cmd.name == "fetch") {
            if (fetch_count >= kMaxFetches) {
                block << "[/fetch " << cmd.args << "]\n"
                      << "SKIPPED: max " << kMaxFetches << " fetches per turn\n"
                      << "[END FETCH]\n\n";
                cache_result = false;
            } else {
                ++fetch_count;
                block << "[/fetch " << cmd.args << "]\n";

                // Image-aware path: when the caller wires `out_image_parts`
                // and the response is image/*, the body lands as an image
                // ContentPart on the next user turn rather than a textified
                // body in this envelope.  Otherwise fall back to the legacy
                // text behaviour (html_to_text, truncated to kPerFetchLimit).
                static constexpr int64_t kFetchImageCap = 8LL * 1024 * 1024;
                FetchedResource fr = cmd_fetch_bytes(cmd.args, kFetchImageCap);
                if (!fr.ok) {
                    block << "ERR: " << fr.error << "\n";
                } else if (out_image_parts != nullptr &&
                           fr.content_type.compare(0, 6, "image/") == 0) {
                    ContentPart p;
                    p.kind       = ContentPart::IMAGE;
                    p.media_type = fr.content_type;
                    p.image_data = base64_encode(fr.body);
                    int idx = static_cast<int>(out_image_parts->size()) + 1;
                    out_image_parts->push_back(std::move(p));
                    block << "[fetched as image #" << idx << " — "
                          << fr.content_type << ", "
                          << fr.byte_count << " bytes; "
                          << "see image content attached to this turn]\n";
                } else {
                    std::string text = html_to_text(fr.body);
                    if (text.size() > kPerFetchLimit) {
                        text.resize(kPerFetchLimit);
                        text += "\n... [truncated]";
                    }
                    block << text << "\n";
                }
                block << "[END FETCH]\n\n";
            }

        } else if (cmd.name == "search") {
            // /search <query>            — top 10 results
            // /search <query> top=N      — top N (capped at 20)
            // Pulled apart here so the SearchInvoker only sees a clean
            // query string + integer N.  The trailing `top=N` token is
            // stripped from the query before dispatch.
            std::string query = cmd.args;
            int top_n = 10;
            {
                auto pos = query.rfind(" top=");
                if (pos != std::string::npos) {
                    try {
                        int parsed = std::stoi(query.substr(pos + 5));
                        if (parsed > 0) {
                            top_n = std::min(parsed, 20);
                            query.resize(pos);
                        }
                    } catch (...) { /* leave query intact, use default */ }
                }
            }
            // Trim trailing whitespace from the query.
            while (!query.empty() && (query.back() == ' ' || query.back() == '\t'))
                query.pop_back();

            if (search_count >= kMaxSearches) {
                block << "[/search " << query << "]\n"
                      << "SKIPPED: max " << kMaxSearches << " searches per turn\n"
                      << "[END SEARCH]\n\n";
                cache_result = false;
            } else if (query.empty()) {
                block << "[/search]\n"
                      << "ERR: empty query — usage: /search <query> [top=N]\n"
                      << "[END SEARCH]\n\n";
                cache_result = false;
            } else if (!search_invoker) {
                block << "[/search " << query << "]\n"
                      << "ERR: web search unavailable in this context — only "
                         "the HTTP API wires a search provider.  Adapt: drop "
                         "the /search step, or fall back to /fetch with a known "
                         "URL.\n"
                      << "[END SEARCH]\n\n";
                cache_result = false;
            } else {
                ++search_count;
                block << "[/search " << query << "]\n";
                std::string body = search_invoker(query, top_n);
                if (body.size() > kPerFetchLimit) {
                    body.resize(kPerFetchLimit);
                    body += "\n... [truncated]";
                }
                block << body;
                if (body.empty() || body.back() != '\n') block << "\n";
                if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                    cache_result = false;
                block << "[END SEARCH]\n\n";
            }

        } else if (cmd.name == "browse") {
            // /browse <url> — JS-rendering fetch via the configured
            // playwright MCP server.  Use this when /fetch fails on
            // Cloudflare / paywalls / SPA-only sites; libcurl can't
            // execute JS and many modern news/journal pages need a
            // real browser to surface their content.
            //
            // Composition: navigate to the URL, then snapshot the
            // accessibility tree.  Both calls go through the existing
            // MCPInvoker so the per-request mcp::Manager (and its one
            // subprocess) handles spawn/lifecycle.  Cold start is
            // multi-second on the first /browse per request; later
            // /browse calls in the same turn share the live browser.
            //
            // Shares the /fetch budget — these are alternatives, and
            // total "URL reads per turn" stays at 3.
            std::string url = cmd.args;
            while (!url.empty() && (url.back() == ' ' || url.back() == '\t'))
                url.pop_back();
            while (!url.empty() && (url.front() == ' ' || url.front() == '\t'))
                url.erase(0, 1);

            if (fetch_count >= kMaxFetches) {
                block << "[/browse " << url << "]\n"
                      << "SKIPPED: max " << kMaxFetches
                      << " URL reads per turn (shared with /fetch)\n"
                      << "[END BROWSE]\n\n";
                cache_result = false;
            } else if (url.empty()) {
                block << "[/browse]\n"
                      << "ERR: empty URL — usage: /browse <url>\n"
                      << "[END BROWSE]\n\n";
                cache_result = false;
            } else if (!mcp_invoker) {
                block << "[/browse " << url << "]\n"
                      << "ERR: /browse requires a playwright MCP server "
                         "configured for this deployment.  Adapt: try "
                         "/fetch <url> instead (works for static pages).\n"
                      << "[END BROWSE]\n\n";
                cache_result = false;
            } else if (auto reject = preflight_ssrf_check(url); !reject.empty()) {
                // Pre-flight the URL against the same SSRF blocklist
                // /fetch enforces.  Unlike libcurl-driven fetches,
                // /browse hands the URL off to Playwright via MCP,
                // which does its own DNS + connect with no hook for
                // us to intervene — without this check the SSRF guard
                // is bypassed entirely.
                block << "[/browse " << url << "]\n"
                      << "ERR: refused — " << reject << " (SSRF guard)\n"
                      << "[END BROWSE]\n\n";
                cache_result = false;
            } else {
                ++fetch_count;
                block << "[/browse " << url << "]\n";

                // JSON-escape the URL for safe interpolation into the
                // {"url":"..."} args object.  URLs rarely contain " or \
                // but defensive escaping costs nothing.
                std::string escaped;
                escaped.reserve(url.size() + 8);
                for (char c : url) {
                    if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
                    else if (c == '\n') escaped += "\\n";
                    else if (c == '\r') escaped += "\\r";
                    else if (c == '\t') escaped += "\\t";
                    else escaped += c;
                }

                const std::string nav_args =
                    "playwright browser_navigate {\"url\":\"" + escaped + "\"}";
                std::string nav = mcp_invoker("call", nav_args);

                // navigate failure (transport ERR or tool-level isError):
                // surface it and skip the snapshot — no point taking a
                // picture of an unloaded page.
                const bool nav_err =
                    (nav.size() >= 4 && nav.compare(0, 4, "ERR:") == 0) ||
                    nav.find("[tool reported isError=true]") != std::string::npos;
                if (nav_err) {
                    block << nav;
                    if (nav.empty() || nav.back() != '\n') block << "\n";
                    cache_result = false;
                } else {
                    // Snapshot returns the accessibility tree as text —
                    // structured but parseable by the agent.  Cap at the
                    // shared per-fetch limit; trees on rich pages can be
                    // tens of KB.
                    std::string snap = mcp_invoker("call",
                                                    "playwright browser_snapshot");
                    if (snap.size() > kPerFetchLimit) {
                        snap.resize(kPerFetchLimit);
                        snap += "\n... [truncated]";
                    }
                    block << snap;
                    if (snap.empty() || snap.back() != '\n') block << "\n";
                    if (snap.size() >= 4 && snap.compare(0, 4, "ERR:") == 0)
                        cache_result = false;
                }
                block << "[END BROWSE]\n\n";
            }

        } else if (cmd.name == "mcp") {
            // /mcp tools [server]                 — list configured tools
            // /mcp call  <server> <tool> [json]   — invoke a tool, stateful
            //                                       within this orchestrator's
            //                                       lifetime (subprocess dies
            //                                       at request end).
            //
            // Capped: same per-turn budget as /fetch since MCP responses can
            // be far chunkier (playwright accessibility snapshots routinely
            // hit tens of KB).  Body cap mirrors kPerFetchLimit so a single
            // /mcp call can't exhaust the turn's tool-result budget.
            std::istringstream iss(cmd.args);
            std::string subcmd;
            iss >> subcmd;
            std::string rest;
            std::getline(iss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);

            std::string callback_kind;
            if      (subcmd == "tools") callback_kind = "tools";
            else if (subcmd == "call")  callback_kind = "call";

            block << "[/mcp " << subcmd
                  << (rest.empty() ? "" : " " + rest) << "]\n";
            if (callback_kind.empty()) {
                block << "ERR: usage: /mcp tools [server]  OR  "
                         "/mcp call <server> <tool> [json_args]\n";
                cache_result = false;
            } else if (!mcp_invoker) {
                block << "ERR: MCP unavailable in this context — only the "
                         "HTTP API spawns the per-request MCP manager.  "
                         "Adapt: drop the /mcp step or run under "
                         "/v1/orchestrate.\n";
                cache_result = false;
            } else {
                std::string body = mcp_invoker(callback_kind, rest);
                if (body.size() > kPerFetchLimit) {
                    body.resize(kPerFetchLimit);
                    body += "\n... [truncated]";
                }
                block << body;
                if (body.empty() || body.back() != '\n') block << "\n";
                if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                    cache_result = false;
            }
            block << "[END MCP]\n\n";

        } else if (cmd.name == "a2a") {
            // /a2a list                — list configured remote agents
            // /a2a card <name>         — render one remote agent's card
            // /a2a call <name> <msg>   — synchronous send to remote agent
            //
            // The body cap mirrors /mcp's because remote agent responses
            // can be similarly chunky (long-form research summaries, tool
            // pipelines).  Errors from the remote come back as ERR: which
            // the dispatcher refuses to cache so a retry can succeed.
            std::istringstream iss(cmd.args);
            std::string subcmd;
            iss >> subcmd;
            std::string rest;
            std::getline(iss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);

            std::string callback_kind;
            if      (subcmd == "list") callback_kind = "list";
            else if (subcmd == "card") callback_kind = "card";
            else if (subcmd == "call") callback_kind = "call";

            block << "[/a2a " << subcmd
                  << (rest.empty() ? "" : " " + rest) << "]\n";
            if (callback_kind.empty()) {
                block << "ERR: usage: /a2a list  OR  /a2a card <name>  OR  "
                         "/a2a call <name> <message>\n";
                cache_result = false;
            } else if (!a2a_invoker) {
                block << "ERR: A2A unavailable in this context — only the "
                         "HTTP API spawns the per-request remote-agent "
                         "manager.  Adapt: drop the /a2a step or run under "
                         "/v1/orchestrate.\n";
                cache_result = false;
            } else {
                std::string body = a2a_invoker(callback_kind, rest);
                if (body.size() > kPerFetchLimit) {
                    body.resize(kPerFetchLimit);
                    body += "\n... [truncated]";
                }
                block << body;
                if (body.empty() || body.back() != '\n') block << "\n";
                if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                    cache_result = false;
            }
            block << "[END A2A]\n\n";

        } else if (cmd.name == "schedule") {
            // /schedule list
            // /schedule cancel <id>
            // /schedule pause  <id>
            // /schedule resume <id>
            // /schedule <phrase>: <message>           ← implicit "create"
            //
            // The callback receives ("list"|"cancel"|"pause"|"resume", args)
            // for the explicit subcommands and ("create", full-line) for the
            // phrase-and-message form, mirroring the /a2a / /mcp shape.
            std::istringstream iss(cmd.args);
            std::string subcmd;
            iss >> subcmd;
            std::string rest;
            std::getline(iss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);

            std::string callback_kind;
            std::string callback_args;
            if (subcmd == "list" || subcmd == "cancel" ||
                subcmd == "pause" || subcmd == "resume") {
                callback_kind = subcmd;
                callback_args = rest;
            } else {
                callback_kind = "create";
                callback_args = cmd.args;
            }

            block << "[/schedule " << callback_kind
                  << (callback_args.empty() ? "" : " " + callback_args)
                  << "]\n";
            if (!scheduler_invoker) {
                block << "ERR: scheduling unavailable in this context — "
                         "the /schedule writ requires the HTTP API's "
                         "scheduler subsystem.  Run under /v1/orchestrate.\n";
                cache_result = false;
            } else {
                std::string body = scheduler_invoker(callback_kind, callback_args, agent_id);
                if (body.size() > kPerFetchLimit) {
                    body.resize(kPerFetchLimit);
                    body += "\n... [truncated]";
                }
                block << body;
                if (body.empty() || body.back() != '\n') block << "\n";
                if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                    cache_result = false;
            }
            block << "[END SCHEDULE]\n\n";

        } else if (cmd.name == "mem") {
            std::istringstream iss(cmd.args);
            std::string subcmd;
            iss >> subcmd;

            // Helper: read/write/clear scratchpad — DB callback when set,
            // filesystem fallback otherwise.  All variants here consume
            // the same callback so a misconfigured deployment can't end
            // up with reads from one source and writes to another.
            auto scratch_read = [&](const std::string& kind /*"read"|"shared-read"*/,
                                     const std::string& aid) -> std::string {
                if (memory_scratchpad) return memory_scratchpad(kind, aid, "");
                if (kind == "shared-read") return cmd_mem_shared_read(memory_dir);
                return cmd_mem_read(aid, memory_dir);
            };
            auto scratch_write = [&](const std::string& kind /*"write"|"shared-write"*/,
                                      const std::string& aid,
                                      const std::string& text) -> std::string {
                if (memory_scratchpad) return memory_scratchpad(kind, aid, text);
                if (kind == "shared-write") return cmd_mem_shared_write(text, memory_dir);
                return cmd_mem_write(aid, text, memory_dir);
            };
            auto scratch_clear = [&](const std::string& kind /*"clear"|"shared-clear"*/,
                                      const std::string& aid) -> std::string {
                if (memory_scratchpad) return memory_scratchpad(kind, aid, "");
                if (kind == "shared-clear") return cmd_mem_shared_clear(memory_dir);
                cmd_mem_clear(aid, memory_dir);
                return "OK: memory cleared";
            };

            if (subcmd == "shared") {
                // /mem shared write <text> | /mem shared read | /mem shared clear
                std::string action;
                iss >> action;
                if (action == "write") {
                    std::string text;
                    std::getline(iss, text);
                    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                    std::string wr = scratch_write("shared-write", agent_id, text);
                    block << "[/mem shared write] " << wr << "\n\n";
                } else if (action == "read" || action == "show") {
                    std::string mem = scratch_read("shared-read", agent_id);
                    block << "[/mem shared read]\n"
                          << (mem.empty() ? "(shared scratchpad empty)" : mem)
                          << "\n[END SHARED MEMORY]\n\n";
                } else if (action == "clear") {
                    std::string cr = scratch_clear("shared-clear", agent_id);
                    block << "[/mem shared clear] " << cr << "\n\n";
                } else {
                    block << "[/mem shared] ERR: unknown action '" << action
                          << "' — use write, read, or clear\n\n";
                    cache_result = false;
                }

            } else if (subcmd == "write") {
                std::string text;
                std::getline(iss, text);
                if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                std::string result = scratch_write("write", agent_id, text);
                block << "[/mem write] " << result << "\n\n";

            } else if (subcmd == "read") {
                std::string mem = scratch_read("read", agent_id);
                block << "[/mem read]\n"
                      << (mem.empty() ? "(no memory)" : mem)
                      << "\n[END MEMORY]\n\n";

            } else if (subcmd == "show") {
                std::string mem = scratch_read("read", agent_id);
                block << "[/mem show]\n"
                      << (mem.empty() ? "(no memory)" : mem)
                      << "\n[END MEMORY]\n\n";

            } else if (subcmd == "clear") {
                std::string cr = scratch_clear("clear", agent_id);
                block << "[/mem clear] " << cr << "\n\n";

            } else if (subcmd == "entries" || subcmd == "entry"  ||
                       subcmd == "search"  || subcmd == "expand" ||
                       subcmd == "density") {
                // Structured-memory read window.  Distinct from the markdown
                // scratchpad above — these surface tenant-scoped graph
                // entries created via the HTTP API.  Available only when the
                // calling context wired a reader (today: api_server when a
                // tenant token is present).  Without one, return ERR so the
                // agent stops trying.
                std::string args;
                std::getline(iss, args);
                if (!args.empty() && args[0] == ' ') args.erase(0, 1);
                block << "[/mem " << subcmd << (args.empty() ? "" : " " + args) << "]\n";
                if (!structured_memory_reader) {
                    block << "ERR: structured memory unavailable in this context — "
                             "this surface is only available when running under "
                             "the HTTP API with a tenant token.  Adapt: drop "
                             "the /mem " << subcmd << " step.\n";
                    cache_result = false;
                } else {
                    std::string body = structured_memory_reader(subcmd, args, agent_id);
                    block << body;
                    if (body.empty() || body.back() != '\n') block << "\n";
                    if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                        cache_result = false;
                }
                block << "[END MEMORY]\n\n";

            } else if (subcmd == "invalidate") {
                // /mem invalidate <id>
                // Soft-delete one entry by id.  The row stays in the DB and
                // remains reachable via `as_of` reads (audit / replay) but
                // disappears from default `/mem entries`, `/mem entry`, and
                // `/mem search` results.  Distinct from a hard delete (no
                // agent-facing surface for that — REST-DELETE is the only
                // path).  Use this when a recorded fact is no longer true:
                // the user changed their mind, a project shipped, etc.
                std::string args;
                std::getline(iss, args);
                if (!args.empty() && args[0] == ' ') args.erase(0, 1);

                block << "[/mem invalidate"
                      << (args.empty() ? "" : " " + args) << "]\n";
                if (!structured_memory_writer) {
                    block << "ERR: structured memory unavailable in this context — "
                             "this surface is only available when running under "
                             "the HTTP API with a tenant token.  Adapt: drop "
                             "the /mem invalidate step.\n";
                    cache_result = false;
                } else if (args.empty()) {
                    block << "ERR: usage: /mem invalidate <id>\n";
                    cache_result = false;
                } else {
                    std::string body = structured_memory_writer(
                        "invalidate", args, /*body=*/"", agent_id);
                    block << body;
                    if (body.empty() || body.back() != '\n') block << "\n";
                    if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                        cache_result = false;
                }
                block << "[END MEMORY]\n\n";

            } else if (subcmd == "add") {
                // Agent direct-write into the structured memory graph.
                // Two shapes:
                //   /mem add entry <type> <title> [--artifact #<id>]
                //       <body — required>
                //   /endmem
                //   /mem add link  <src_id> <relation> <dst_id>
                // Writes land in the curated graph immediately.  Reading
                // the second token consumes it; the rest of the line is
                // handed to the writer callback as `args`, and the
                // optional /endmem-terminated block (parser-collected
                // into cmd.content) is handed in as `body`.
                std::string kind;
                iss >> kind;
                std::string args;
                std::getline(iss, args);
                if (!args.empty() && args[0] == ' ') args.erase(0, 1);

                std::string callback_kind;
                if      (kind == "entry") callback_kind = "add-entry";
                else if (kind == "link")  callback_kind = "add-link";

                block << "[/mem add " << kind
                      << (args.empty() ? "" : " " + args) << "]\n";
                if (callback_kind.empty()) {
                    block << "ERR: usage: /mem add entry <type> <title> "
                             "(then body, then /endmem) OR /mem add link "
                             "<src_id> <relation> <dst_id>\n";
                    cache_result = false;
                } else if (!structured_memory_writer) {
                    block << "ERR: structured memory unavailable in this context — "
                             "this surface is only available when running under "
                             "the HTTP API with a tenant token.  Adapt: drop "
                             "the /mem add step.\n";
                    cache_result = false;
                } else if (kind == "entry" && cmd.truncated) {
                    // Parser hit EOF before /endmem.  Don't commit a
                    // partial entry — let the orchestrator's continuation
                    // path try again with the rest of the body.
                    block << "ERR: /mem add entry block was not closed with "
                             "/endmem; entry not written.  Re-emit the full "
                             "block in the next turn.\n";
                    cache_result = false;
                } else if (kind == "entry" && cmd.content.empty()) {
                    // Empty body — refuse the write.  Forces the agent to
                    // synthesise retrievable text; a title-only entry is
                    // worthless to /mem search across sessions.
                    block << "ERR: /mem add entry requires a content body "
                             "between the header and /endmem.  Synthesise "
                             "the substance of the finding (key facts, "
                             "numbers, sources) so future /mem search and "
                             "/mem entry calls return useful context.\n";
                    cache_result = false;
                } else {
                    std::string body = structured_memory_writer(
                        callback_kind, args, cmd.content, agent_id);
                    block << body;
                    if (body.empty() || body.back() != '\n') block << "\n";
                    if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                        cache_result = false;
                }
                block << "[END MEMORY]\n\n";

            } else {
                block << "[/mem] ERR: unknown subcommand '" << subcmd
                      << "' — use read, write, show, clear, shared, "
                         "entries, entry, search, expand, density, "
                         "add, or invalidate\n\n";
                cache_result = false;
            }

        } else if (cmd.name == "help") {
            // /help [<topic>] — surface detailed slash-command reference on
            // demand.  The system prompt only carries a compressed inventory;
            // full prose lives in help_for_topic() and is loaded only when
            // the agent asks for it, keeping the per-turn prompt small.
            std::string topic = cmd.args;
            // Lowercase + take the first whitespace-delimited token so
            // "/help mem add" still resolves to topic="mem".
            for (auto& ch : topic) ch = static_cast<char>(std::tolower(ch));
            auto sp = topic.find_first_of(" \t");
            if (sp != std::string::npos) topic.resize(sp);

            block << "[/help" << (topic.empty() ? "" : " ") << topic << "]\n";
            if (topic.empty()) {
                block << help_index();
            } else {
                std::string body = help_for_topic(topic);
                if (body.empty()) {
                    block << "ERR: unknown topic '" << topic
                          << "' — call /help with no args to list available topics\n";
                    cache_result = false;
                } else {
                    block << body;
                }
            }
            block << "[END HELP]\n\n";

        } else if (cmd.name == "exec") {
            block << "[/exec " << cmd.args << "]\n";
            if (exec_disabled) {
                // API / sandboxed contexts run with exec turned off so
                // agents can't invoke arbitrary shell commands.  Tell the
                // calling agent explicitly so it adapts its plan instead
                // of retrying the same /exec.
                block << "ERR: /exec is disabled in this execution context — "
                         "shell commands are not permitted.  Adapt: drop the "
                         "/exec step, use /fetch for URL reads, /write for "
                         "files, or /agent to delegate analysis that doesn't "
                         "require the shell.\n";
                cache_result = false;
            } else if (confirm && is_destructive_exec(cmd.args) &&
                       !confirm("exec '" + cmd.args + "'?")) {
                block << "ERR: user declined\n";
                cache_result = false;  // user may want to approve a retry
            } else {
                block << cmd_exec(cmd.args, /*confirmed=*/true) << "\n";
            }
            block << "[END EXEC]\n\n";

        } else if (cmd.name == "advise") {
            // /advise <question>
            // Fires a one-shot, zero-context call against the caller's
            // configured advisor_model.  Replaces Anthropic's beta
            // `advisor_20260301` tool with a provider-portable path, so
            // cheap executors (ollama/*) can pair with smart advisors
            // (claude-opus-*) without relying on Anthropic-only plumbing.
            block << "[/advise]\n";
            if (!advisor_invoker) {
                block << "ERR: advisor not configured — set advisor_model in "
                         "the agent config to enable /advise.\n";
                cache_result = false;
            } else if (advise_count >= kMaxAdvise) {
                block << "SKIPPED: max " << kMaxAdvise
                      << " advisor consults per turn.  A third call means the "
                         "task is under-scoped — deliver what you have and note "
                         "the open question in your output.\n";
                cache_result = false;
            } else {
                ++advise_count;
                std::string answer = advisor_invoker(cmd.args);
                block << answer;
                if (!answer.empty() && answer.back() != '\n') block << "\n";
                // Same upstream-failed framing as /agent — don't let the
                // executor burn retries against a failing advisor.
                if (answer.size() >= 4 && answer.compare(0, 4, "ERR:") == 0) {
                    block << "UPSTREAM FAILED — the advisor returned an error. "
                             "Same question will fail again.  Do NOT re-issue "
                             "the identical /advise.  Proceed with your own "
                             "judgment and note the open question in your "
                             "output.\n";
                }
            }
            block << "[END ADVISE]\n\n";

        } else if (cmd.name == "pane") {
            // /pane <agent_id> <message> — fire-and-forget: spawn a new UI
            // pane running the agent on the given message.  The spawning
            // agent's turn continues with only a short status block folded
            // in; the new pane's output streams into its own view, not
            // back into this agent's context.  Use for work that's truly
            // parallel-independent — /agent is still the right choice when
            // the caller needs to fold the sub-agent's result into its own
            // synthesis.
            std::istringstream iss(cmd.args);
            std::string sub_id;
            iss >> sub_id;
            std::string sub_msg;
            std::getline(iss, sub_msg);
            if (!sub_msg.empty() && sub_msg[0] == ' ') sub_msg.erase(0, 1);

            block << "[/pane " << sub_id << "]\n";
            if (!pane_spawner) {
                block << "ERR: pane spawning unavailable in this context "
                         "(no interactive REPL)\n";
                cache_result = false;
            } else if (sub_id.empty() || sub_msg.empty()) {
                block << "ERR: usage: /pane <agent-id> <message>\n";
                cache_result = false;
            } else {
                std::string status = pane_spawner(sub_id, sub_msg);
                block << status;
                if (status.empty() || status.back() != '\n') block << "\n";
                if (status.size() >= 4 && status.compare(0, 4, "ERR:") == 0)
                    cache_result = false;
            }
            block << "[END PANE]\n\n";

        } else if (cmd.name == "parallel") {
            // Fan-out block.  Re-parse the body as a list of /agent lines,
            // hand them to the orchestrator's ParallelInvoker, and aggregate
            // results in input order.  The orchestrator spawns one thread
            // per child with its own stream_id and dedup cache.
            std::vector<std::pair<std::string, std::string>> children;
            {
                std::istringstream pss(cmd.content);
                std::string pline;
                while (std::getline(pss, pline)) {
                    if (!pline.empty() && pline.back() == '\r') pline.pop_back();
                    if (pline.size() > 7 && pline.substr(0, 7) == "/agent ") {
                        std::istringstream iss(pline.substr(7));
                        std::string sub_id;
                        iss >> sub_id;
                        std::string sub_msg;
                        std::getline(iss, sub_msg);
                        if (!sub_msg.empty() && sub_msg[0] == ' ') sub_msg.erase(0, 1);
                        if (!sub_id.empty())
                            children.emplace_back(sub_id, sub_msg);
                    }
                }
            }

            block << "[/parallel " << children.size() << " children]\n";
            if (cmd.truncated) {
                block << "ERR: /parallel block was cut off (missing "
                         "/endparallel).  Re-emit the block from scratch.\n";
                cache_result = false;
            } else if (children.empty()) {
                block << "ERR: /parallel block had no /agent lines — only "
                         "/agent <id> <msg> is permitted inside "
                         "/parallel.../endparallel\n";
                cache_result = false;
            } else if (!parallel_invoker) {
                block << "ERR: /parallel unavailable in this context\n";
                cache_result = false;
            } else {
                auto results = parallel_invoker(children);
                if (results.size() != children.size()) {
                    block << "ERR: /parallel rejected the batch "
                             "(orchestrator returned " << results.size()
                          << " results for " << children.size()
                          << " children)\n";
                    if (!results.empty()) block << results.front() << "\n";
                    cache_result = false;
                } else {
                    for (size_t i = 0; i < results.size(); ++i) {
                        block << "[child " << (i + 1) << "/" << results.size()
                              << " — /agent " << children[i].first << "]\n"
                              << results[i] << "\n"
                              << "[END CHILD " << (i + 1) << "]\n";
                        if (results[i].size() >= 4 &&
                            results[i].compare(0, 4, "ERR:") == 0) {
                            cache_result = false;
                        }
                    }
                }
            }
            block << "[END PARALLEL]\n\n";

        } else if (cmd.name == "agent") {
            // /agent <sub_agent_id> <message>
            std::istringstream iss(cmd.args);
            std::string sub_id;
            iss >> sub_id;
            std::string sub_msg;
            std::getline(iss, sub_msg);
            if (!sub_msg.empty() && sub_msg[0] == ' ') sub_msg.erase(0, 1);

            block << "[/agent " << sub_id << "]\n";
            if (!agent_invoker) {
                block << "ERR: agent invocation unavailable in this context\n";
            } else {
                std::string sub_resp = agent_invoker(sub_id, sub_msg);
                block << sub_resp << "\n";
                // Clearer failure signal: when the sub-agent returns an upstream
                // error, tell the calling agent NOT to retry the identical brief.
                // The internal ApiClient already burned 4 backoff retries before
                // the sub-agent returned ERR, so a same-turn reinvocation is
                // almost certainly going to fail the same way.  Prescribe the
                // adaptation paths so the master doesn't just repeat itself.
                if (sub_resp.size() >= 4 && sub_resp.compare(0, 4, "ERR:") == 0) {
                    block << "UPSTREAM FAILED — the sub-agent returned an error "
                             "after the API client's internal retry/backoff chain. "
                             "The same brief will fail again.  Do NOT re-invoke "
                             "/agent " << sub_id << " with the same arguments.  "
                             "Options: adapt the brief (narrower scope, different "
                             "angle), try a different agent whose capabilities "
                             "overlap, or proceed with partial results and flag "
                             "the gap in your synthesis.\n";
                }
            }
            block << "[END AGENT]\n\n";

        } else if (cmd.name == "write") {
            // Detect the --persist flag.  Grammar: `/write [--persist] <path>`.
            // The flag is parsed off the front of args; what remains is the
            // user-supplied path.  When --persist is set AND the API has
            // wired an ArtifactWriter, the write goes to BOTH the SSE
            // event (via write_interceptor for live UI) AND the artifact
            // store (durable, addressable via /read).
            bool persist = false;
            std::string path = cmd.args;
            {
                // Trim leading whitespace before flag detection.
                while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
                    path.erase(0, 1);
                const std::string flag = "--persist";
                if (path.size() > flag.size() &&
                    path.compare(0, flag.size(), flag) == 0 &&
                    (path[flag.size()] == ' ' || path[flag.size()] == '\t')) {
                    persist = true;
                    path.erase(0, flag.size());
                    while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
                        path.erase(0, 1);
                }
            }
            const std::string display = persist ? "--persist " + path : path;
            block << "[/write " << display << "]\n";
            // Orchestrator attempted to recover unclosed /write blocks before
            // we got here.  If `truncated` is STILL set, recovery exhausted
            // its retries — refuse to persist the partial and tell the model
            // explicitly so the next turn can retry with a fresh /write.
            if (cmd.truncated) {
                block << "ERR: /write block for " << path
                      << " was truncated mid-generation (missing /endwrite) "
                         "and could not be recovered.  File NOT written.  "
                         "Re-emit the full /write block from scratch.\n";
                block << "[END WRITE]\n\n";
                cache_result = false;   // allow a clean retry
                out << block.str();
                if (tool_status) tool_status(cmd.name, false);
                continue;
            }
            if (!write_interceptor && confirm) {
                // Only prompt when the write will actually touch disk.
                // Intercepted writes go to an in-memory sink that can't
                // clobber the user's filesystem, so no confirm is needed.
                size_t lines = std::count(cmd.content.begin(), cmd.content.end(), '\n');
                if (!cmd.content.empty() && cmd.content.back() != '\n') ++lines;
                std::string p = "write " + path + " (" +
                                std::to_string(lines) + " lines, " +
                                std::to_string(cmd.content.size()) + " bytes)?";
                if (!confirm(p)) {
                    block << "ERR: user declined\n";
                    block << "[END WRITE]\n\n";
                    cache_result = false;
                    out << block.str();
                    if (tool_status) tool_status(cmd.name, false);
                    continue;
                }
            }
            // API / sandboxed callers route the write through their own
            // sink (e.g. an SSE `file` event) instead of touching disk.
            if (write_interceptor) {
                block << write_interceptor(path, cmd.content) << "\n";
            } else {
                block << cmd_write(path, cmd.content) << "\n";
            }
            // Persistent write — appends a second OK/ERR line for the
            // artifact-store outcome.  Falls back to a clear "ephemeral
            // only" notice when the API didn't wire the writer (e.g. a
            // request without conversation_id, or CLI/REPL).
            if (persist) {
                if (artifact_writer) {
                    std::string body = artifact_writer(path, cmd.content);
                    block << body;
                    if (body.empty() || body.back() != '\n') block << "\n";
                    if (body.size() >= 4 && body.compare(0, 4, "ERR:") == 0)
                        cache_result = false;
                } else {
                    block << "WARN: --persist requested but artifact store is "
                             "unavailable in this context (no conversation, or "
                             "CLI/REPL); the write was ephemeral only.\n";
                }
            }
            block << "[END WRITE]\n\n";

        } else if (cmd.name == "read") {
            // Three call shapes:
            //   /read <path>                       — same-conversation, by path
            //   /read #<aid>                       — same-conversation, by id
            //   /read #<aid> via=mem:<mid>         — cross-conversation, with
            //                                         memory citation as the
            //                                         capability that grants
            //                                         access.
            // The dispatcher disambiguates here so the reader callback gets
            // pre-parsed values and the policy check stays in one place
            // (api_server's lambda).
            std::string args = cmd.args;
            while (!args.empty() && (args.back() == ' ' || args.back() == '\t'))
                args.pop_back();
            while (!args.empty() && (args.front() == ' ' || args.front() == '\t'))
                args.erase(0, 1);

            std::string path;
            int64_t artifact_id   = 0;
            int64_t via_memory_id = 0;
            std::string parse_err;

            if (!args.empty() && args.front() == '#') {
                std::string rest = args.substr(1);
                const size_t sp = rest.find_first_of(" \t");
                const std::string aid_token =
                    (sp == std::string::npos) ? rest : rest.substr(0, sp);
                std::string tail = (sp == std::string::npos) ? std::string{}
                                                              : rest.substr(sp);
                while (!tail.empty() && (tail.front() == ' ' || tail.front() == '\t'))
                    tail.erase(0, 1);
                while (!tail.empty() && (tail.back()  == ' ' || tail.back()  == '\t'))
                    tail.pop_back();

                try { artifact_id = std::stoll(aid_token); }
                catch (...) { artifact_id = 0; }
                if (artifact_id <= 0) {
                    parse_err = "artifact id after '#' must be a positive integer";
                } else if (!tail.empty()) {
                    const std::string prefix = "via=mem:";
                    if (tail.compare(0, prefix.size(), prefix) != 0) {
                        parse_err = "trailing token '" + tail +
                                     "' — expected 'via=mem:<entry_id>'";
                    } else {
                        try { via_memory_id = std::stoll(tail.substr(prefix.size())); }
                        catch (...) { via_memory_id = 0; }
                        if (via_memory_id <= 0) {
                            parse_err = "via=mem:<id> requires a positive entry id";
                        }
                    }
                }
            } else {
                path = args;
            }

            block << "[/read " << args << "]\n";
            if (path.empty() && artifact_id == 0 && parse_err.empty()) {
                parse_err = "usage: /read <path>  OR  "
                            "/read #<artifact_id> [via=mem:<entry_id>]";
            }
            if (!parse_err.empty()) {
                block << "ERR: " << parse_err << "\n";
                cache_result = false;
            } else if (!artifact_reader) {
                block << "ERR: artifact store unavailable in this context — "
                         "/read works only inside an HTTP conversation.  "
                         "Adapt: drop the /read step.\n";
                cache_result = false;
            } else {
                ArtifactReadResult ar = artifact_reader(path, artifact_id, via_memory_id);
                const bool is_err = ar.body.size() >= 4 &&
                                    ar.body.compare(0, 4, "ERR:") == 0;
                if (!is_err && out_image_parts != nullptr &&
                    ar.media_type.compare(0, 6, "image/") == 0) {
                    // Image artifact — push as image part, write placeholder
                    // envelope identical in shape to /fetch's image branch
                    // so the model sees a uniform "[fetched|read as image #N
                    // — <mime>, <bytes>]" cue regardless of source.
                    ContentPart p;
                    p.kind       = ContentPart::IMAGE;
                    p.media_type = ar.media_type;
                    p.image_data = base64_encode(ar.body);
                    int idx = static_cast<int>(out_image_parts->size()) + 1;
                    int64_t bytes = static_cast<int64_t>(ar.body.size());
                    out_image_parts->push_back(std::move(p));
                    block << "[read as image #" << idx << " — "
                          << ar.media_type << ", "
                          << bytes << " bytes; "
                          << "see image content attached to this turn]\n";
                } else {
                    std::string body = std::move(ar.body);
                    if (body.size() > kPerFetchLimit) {
                        body.resize(kPerFetchLimit);
                        body += "\n... [truncated]";
                    }
                    block << body;
                    if (body.empty() || body.back() != '\n') block << "\n";
                    if (is_err) cache_result = false;
                }
            }
            block << "[END READ]\n\n";

        } else if (cmd.name == "list") {
            block << "[/list]\n";
            if (!artifact_lister) {
                block << "ERR: artifact store unavailable in this context — "
                         "/list works only inside an HTTP conversation.\n";
                cache_result = false;
            } else {
                std::string body = artifact_lister();
                if (body.empty()) body = "(no persisted artifacts in this conversation)\n";
                block << body;
                if (body.back() != '\n') block << "\n";
            }
            block << "[END LIST]\n\n";
        }

        // Flush per-command block into the aggregate and record it in the
        // dedup cache so a same-turn repeat short-circuits to the quote path.
        std::string block_str = block.str();
        out << block_str;
        if (dedup_cache && cache_result && !block_str.empty()) {
            (*dedup_cache)[dedup_key] = block_str;
        }
        if (tool_status && !block_str.empty()) {
            tool_status(cmd.name, !is_tool_result_failure(block_str));
        }
    }

    out << "[END TOOL RESULTS]";
    return out.str();
}

} // namespace arbiter
