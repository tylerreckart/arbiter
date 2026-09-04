// Remote (--connect) session helpers for ReplSession.
#include "repl/session.h"
#include "repl/session_internal.h"

#include "remote/sse_turn.h"
#include "render_policy.h"
#include "stream_renderer.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <utility>

namespace arbiter {

ReplSession::ReplSession(std::string config_dir,
                         RemoteConnectConfig remote_cfg,
                         bool exec_allowed_flag)
    : dir(std::move(config_dir))
    , orch(std::map<std::string, std::string>{})  // no local provider keys
    , conversation_store(dir)
    , remote(std::make_unique<RemoteApiClient>(std::move(remote_cfg)))
{
    cfg.exec_allowed = exec_allowed_flag;
    // Remote mode: agents live on the API host.  Skip local agent load so
    // missing ~/.arbiter/agents is fine.  Keep a stub "index" model label
    // for chrome via orch defaults (Orchestrator always has index).
    orch.set_exec_disabled(true);

    history_sidebar.set_enabled(
        tui_design().layout.show_history_sidebar, dir);

    interactive_prompts.set_notify([this]() { wake_main_input(); });

    palette_items = {
        {"/ask",          "ask the index master"},
        {"/use",          "switch the focused pane's current agent"},
        {"/agents",       "list remote agents"},
        {"/status",       "connection status"},
        {"/find",         "search the focused pane's scrollback"},
        {"/theme",        "change TUI theme"},
        {"/chat",         "list / new / switch remote conversations"},
        {"/attach",       "attach an image for the next message"},
        {"/quit",         "exit"},
        {"/help",         "help"},
    };

    sidebar.set_remote_info(remote->config().display_host, {});
}

void ReplSession::remote_refresh_conversations() {
    if (!remote) return;
    auto list = remote->list_conversations(100);
    std::lock_guard<std::mutex> lk(remote_conv_mu);
    remote_conversations = std::move(list);
}

std::vector<ConversationEntry> ReplSession::conversation_list() const {
    if (!remote) return conversation_store.list();
    std::lock_guard<std::mutex> lk(remote_conv_mu);
    return remote_conversations;
}

std::string ReplSession::conversation_active_id() const {
    if (!remote) return conversation_store.active_id();
    std::lock_guard<std::mutex> lk(remote_conv_mu);
    return remote_active_id;
}

void ReplSession::conversation_set_active(const std::string& id) {
    if (!remote) {
        conversation_store.set_active(id);
        return;
    }
    std::lock_guard<std::mutex> lk(remote_conv_mu);
    remote_active_id = id;
}

std::string ReplSession::conversation_create(const std::string& /*folder_id*/) {
    if (!remote) {
        return conversation_store.create(
            std::filesystem::current_path().string());
    }
    std::string err;
    const std::string id = remote->create_conversation(&err, "Untitled");
    if (id.empty()) {
        std::cerr << "ERR: " << (err.empty() ? "create failed" : err) << "\n";
        return {};
    }
    ConversationEntry e;
    e.id = id;
    e.title = "Untitled";
    {
        std::lock_guard<std::mutex> lk(remote_conv_mu);
        remote_conversations.insert(remote_conversations.begin(), e);
        remote_active_id = id;
    }
    return id;
}

bool ReplSession::conversation_delete_remote(const std::string& id,
                                              std::string* err) {
    if (!remote) return false;
    if (!remote->delete_conversation(id, err)) return false;
    std::lock_guard<std::mutex> lk(remote_conv_mu);
    remote_conversations.erase(
        std::remove_if(remote_conversations.begin(), remote_conversations.end(),
                       [&](const ConversationEntry& e) { return e.id == id; }),
        remote_conversations.end());
    if (remote_active_id == id) remote_active_id.clear();
    return true;
}

void ReplSession::refresh_history_sidebar_entries() {
    if (!remote) {
        history_sidebar.refresh_entries(conversation_store);
        return;
    }
    // Feed remote list into the sidebar via a temporary shim: refresh_entries
    // reads ConversationStore.  Instead, poke entries through a dedicated
    // path — HistorySidebarState::refresh_entries_list.
    remote_refresh_conversations();
    history_sidebar.refresh_entries_list(conversation_list(),
                                          conversation_active_id());
}

void ReplSession::enter_history_sidebar_focus() {
    if (is_remote()) {
        remote_refresh_conversations();
        history_sidebar.enter_focus_list(conversation_list(),
                                          conversation_active_id());
    } else {
        history_sidebar.enter_focus(conversation_store,
                                    conversation_store.active_id());
    }
}

void ReplSession::cancel_pane_turn(Pane& pane) {
    if (is_remote()) {
        auto gate = std::atomic_load(&pane.remote_turn);
        if (!gate) return;
        gate->stream_cancel.store(true, std::memory_order_release);
        std::string rid;
        {
            std::lock_guard<std::mutex> lk(gate->mu);
            rid = gate->request_id;
        }
        if (!rid.empty() && remote) (void)remote->cancel_request(rid);
        return;
    }
    // Scoped only — never fall back to orch.cancel(). An idle pane (no
    // turn_cancel token) must not abort sibling panes' in-flight turns
    // during switch/delete/close.
    auto token = std::atomic_load(&pane.turn_cancel);
    if (token) orch.cancel_token(token);
}

ApiResponse ReplSession::run_remote_turn(Pane& pane, const std::string& line,
                                         std::vector<PromptAttachment> attachments) {
    ApiResponse fail;
    fail.ok = false;
    if (!remote) {
        fail.error = "not a remote session";
        return fail;
    }
    if (pane.conversation_id.empty()) {
        fail.error = "no conversation bound";
        return fail;
    }

    auto gate = std::make_shared<Pane::RemoteTurnGate>();
    std::atomic_store(&pane.remote_turn, gate);

    StreamRenderer renderer(master_stream_policy(cfg), pane.output_queue);
    RemoteTurnHooks hooks;
    hooks.on_request_id = [gate](const std::string& rid) {
        std::lock_guard<std::mutex> lk(gate->mu);
        gate->request_id = rid;
    };
    hooks.on_tenant = [this](const std::string& name, int64_t /*tid*/) {
        if (!name.empty()) {
            remote_tenant_name = name;
            sidebar.set_remote_info(remote->config().display_host, name);
        }
    };
    hooks.on_tool = [&](const ToolActivityEvent& ev) {
        if (ev.phase == ToolActivityEvent::Phase::Finished) {
            sidebar.record_tool(ev.label, ev.ok, ev.result_preview);
        }
        ToolActivityEvent view = ev;
        if (view.label.rfind("todo:", 0) == 0) {
            view.label = sidebar.friendly_todo_label(ev.label);
        }
        pane.output_queue.push_tool(view);
        if (ev.phase == ToolActivityEvent::Phase::Started) {
            pane.tool_indicator.on_started();
        } else if (ev.phase == ToolActivityEvent::Phase::Finished) {
            pane.tool_indicator.bump(view.label, ev.ok);
        }
    };
    hooks.on_sub_agent = [&](const std::string& agent, const std::string& content) {
        if (pane.last_interim_agent != agent) {
            pane.output_queue.push_prose({styled_interim_header(agent)});
            pane.last_interim_agent = agent;
        }
        StreamRenderer interim(kInterim, pane.output_queue);
        interim.feed(content);
        interim.flush();
    };
    hooks.on_escalation = [&](const std::string& agent, const std::string& reason) {
        pane.output_queue.push_prose(
            {styled_advisor_halt_line(agent, reason)});
        pane.output_queue.end_message();
    };
    hooks.on_intent = [&](const std::string& kind,
                            const std::string& source,
                            const std::string& target,
                            bool applied) {
        auto line = styled_intent_event_line(kind, source, target, applied);
        if (!line) return;
        pane.output_queue.push_prose({std::move(*line)});
        pane.output_queue.end_message();
    };
    hooks.on_advisor = [&](const std::string& kind,
                             const std::string& agent,
                             const std::string& detail) {
        if (kind == "gate_continue" || kind == "gate_halt" || kind == "gate_budget") {
            return;  // quiet success; halt paints via on_escalation
        }
        if (kind == "consult") {
            pane.output_queue.push_prose(
                {styled_advisor_consult_line(agent, detail)});
            pane.output_queue.end_message();
            return;
        }
        if (kind == "gate_redirect") {
            pane.output_queue.push_prose(
                {styled_advisor_redirect_line(agent, detail)});
            pane.output_queue.end_message();
            return;
        }
    };
    hooks.on_presence = [&](const std::string& kind,
                              const std::string& watcher,
                              const std::string& detail) {
        if (kind != "context") return;
        pane.output_queue.push_prose(
            {styled_presence_line(watcher, detail)});
        pane.output_queue.end_message();
    };

    RemoteSseTurnConsumer consumer(renderer, pane.output_queue, std::move(hooks));

    auto http = remote->post_message_stream(
        pane.conversation_id,
        line,
        std::move(attachments),
        [&](const std::string& ev, const std::string& data) {
            consumer.on_event(ev, data);
        },
        gate->stream_cancel);

    const bool cancelled =
        gate->stream_cancel.load(std::memory_order_acquire) ||
        http.error == "canceled";
    auto turn = consumer.finish(cancelled);

    // Clear gate so Esc no longer targets this turn.
    std::atomic_store(&pane.remote_turn, std::shared_ptr<Pane::RemoteTurnGate>{});

    if (!cancelled && http.status_code != 200 && http.status_code != 0 &&
        !turn.ok && turn.error.empty()) {
        turn.error = "HTTP " + std::to_string(http.status_code);
        if (!http.error.empty()) turn.error += ": " + http.error;
    } else if (!cancelled && !http.error.empty() && http.error != "canceled" &&
               !turn.ok && turn.error.empty()) {
        turn.error = http.error;
    }

    ApiResponse resp;
    resp.ok = turn.ok;
    resp.content = turn.content;
    resp.error = turn.error;
    resp.input_tokens = turn.input_tokens;
    resp.output_tokens = turn.output_tokens;
    return resp;
}

} // namespace arbiter
