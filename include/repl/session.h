#pragma once
// ReplSession — shared state + method surface for the interactive TUI/REPL.
//
// cmd_interactive() constructs one session and drives boot → input loop →
// shutdown. Method bodies live in the peeled src/repl/*.cpp translation units.

#include "api_server.h"
#include "config.h"
#include "loop_manager.h"
#include "notification_bus.h"
#include "orchestrator.h"
#include "repl/conversation_store.h"
#include "repl/layout.h"
#include "repl/pane.h"
#include "repl/pane_history.h"
#include "scheduler.h"
#include "tui/history_sidebar.h"
#include "tui/interactive_prompt.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/opentui/pane_input_editor.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/opentui/session.h"
#include "tui/opentui/shared_input_history.h"
#include "tui/palette.h"
#include "tui/sidebar.h"
#include "tui/theme_picker.h"
#include "tui/tty_guard.h"
#include "tui/tui.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace arbiter {

struct ReplSession {
    // ── Nested request / gesture types ─────────────────────────────────────
    struct PendingClose {
        Pane*       pane = nullptr;
        std::string agent_id;
    };

    struct PendingConversationOp {
        bool switch_op = false;
        bool create_new = false;
        std::string target_id;
        bool delete_op = false;
        bool hard_delete = false;
        std::string folder_id;
    };

    struct MouseDragState {
        bool active = false;
        LayoutTree::SeparatorRef sep{};
    };

    struct MouseSelectState {
        bool active = false;
        bool dragged = false;
        Pane* pane = nullptr;
        opentui::ScrollCellPos anchor{};
    };

    struct PendingMouseSwitch {
        bool pending = false;
        bool create_new = false;
        bool toggle_folder = false;
        std::string folder_id;
    };

    struct PendingAfterCancel {
        enum class Kind { None, Switch, Delete } kind = Kind::None;
        bool create_new = false;
        std::string target_id;
        bool hard_delete = false;
        std::string folder_id;
        Pane* pane = nullptr;
        std::string wait_conversation_id;
        const char* abandon_status = "Switch cancelled";
    };

    // ── Lifetime (destruction order matters — reverse of declaration) ──────
    // StdinRawModeGuard must outlive OpenTUI Session so termios restore runs
    // after OpenTUI teardown on exception unwind.
    StdinRawModeGuard stdin_guard;
    opentui::Session  ot_session;
    UiContext         ui_ctx;

    std::string dir;
    Orchestrator orch;
    Config cfg;
    LoopManager loops;

    // Serializes layout tree mutations against the pump thread's iteration.
    // Recursive because dispatch_chord can call back into the tree.
    std::recursive_mutex layout_mu;
    // Owned after boot; methods use layout_ptr (null before construct / after
    // teardown). Lifetime managed in run() via layout_holder.
    std::unique_ptr<LayoutTree> layout_holder;
    LayoutTree* layout_ptr = nullptr;

    InteractivePromptQueue interactive_prompts;
    std::atomic<opentui::PaneInputEditor*> active_readline{nullptr};
    std::atomic<bool> deferred_main_interrupt{false};
    std::atomic<bool> quit_requested{false};
    std::atomic<bool> refresh_focused_input{false};

    ConversationStore conversation_store;
    HistorySidebarState history_sidebar;
    ThemePickerState theme_picker;
    SidebarState sidebar;

    std::mutex                pending_closes_mu;
    std::vector<PendingClose> pending_closes;
    std::mutex                          pending_conv_mu;
    std::vector<PendingConversationOp>  pending_conv_ops;

    MouseDragState mouse_drag;
    MouseSelectState mouse_select;
    PendingMouseSwitch mouse_switch;
    PendingAfterCancel pending_after_cancel;
    std::atomic<bool> pending_cancel_wait{false};

    std::shared_ptr<std::atomic<int64_t>> tool_conversation_id;
    ApiServerOptions api_opts{};
    NotificationBus notifications;
    std::unique_ptr<Scheduler> scheduler;  // constructed during boot

    std::shared_ptr<opentui::SharedInputHistory> shared_history;
    std::function<void()> pump_notify;
    std::vector<PaletteItem> palette_items;
    PaneFrameHooks pane_hooks;

    std::mutex              pump_cv_mu;
    std::condition_variable pump_cv;
    bool                    pump_notified = false;
    std::atomic<bool>       pump_stop{false};
    std::thread             output_pump;

    // Constructed after load_tui_design / get_api_keys in cmd_interactive so
    // StdinRawModeGuard arms only once the theme path has run.
    ReplSession(std::string config_dir,
                std::map<std::string, std::string> api_keys,
                bool exec_allowed_flag);
    ~ReplSession();

    ReplSession(const ReplSession&) = delete;
    ReplSession& operator=(const ReplSession&) = delete;

    void run();

    // ── Shared helpers (interactive.cpp) ───────────────────────────────────
    void wake_main_input();
    void fail_pending_prompts();
    Rect layout_bounds();
    static int64_t parse_conversation_db_id(const std::string& id);
    void bind_tools_conversation(const std::string& conv_id);
    void persist_layout();
    bool sync_layout_to_terminal();
    void reveal_sidebar();
    void refresh_chrome();
    int  outer_bottom_input_rows();
    void present_unlocked();
    void present_holding_lock();
    void setup_pane_hooks();
    void boot_layout_and_transcripts(bool restored);
    void start_output_pump();
    void shutdown();

    // ── session_callbacks.cpp ──────────────────────────────────────────────
    std::unique_ptr<Pane> make_pane();
    void install_orch_callbacks();
    std::string spawn_pane(const std::string& req_agent, const std::string& message);

    // ── slash_commands.cpp ─────────────────────────────────────────────────
    void handle_line(Pane& pane, const std::string& line);

    // ── pane_runtime.cpp ───────────────────────────────────────────────────
    void start_pane_thread(Pane& p_ref);

    // ── interactive_prompts.cpp ────────────────────────────────────────────
    static std::string apply_diff_proposal(Pane& pane, int id);
    static std::vector<std::string> patch_preview_lines(const std::string& patch);
    void handle_diff_decision(Pane& pane, int patch_id, InteractiveDecision d);
    bool service_interactive();

    // ── chords.cpp ─────────────────────────────────────────────────────────
    void clear_spawn_parent_refs(Pane* parent);
    bool service_pending_closes();
    void dispatch_chord(char cmd);

    // ── conversations_ui.cpp ───────────────────────────────────────────────
    bool focused_turn_in_flight();
    bool conversation_turn_in_flight(const std::string& id);
    void apply_conversation_to_pane(Pane& pane, const std::string& id, bool replay);
    void finish_switch_conversation(bool create_new, std::string explicit_id,
                                    std::string folder_id = {});
    void finish_delete_conversation(const std::string& id, bool hard, bool any_showing);
    void begin_pending_after_cancel(PendingAfterCancel pending);
    bool service_pending_after_cancel();
    void switch_conversation(bool create_new, std::string explicit_id = {},
                             std::string folder_id = {});
    void delete_conversation(const std::string& id, bool hard);
    bool service_pending_conv_ops();

    // ── mouse_input.cpp ────────────────────────────────────────────────────
    void clear_mouse_select();
    void clear_mouse_select_and_highlight();
    void clear_mouse_drag();
    void clear_all_selections();
    void scroll_pane(Pane& pane, int direction, int step);
    Rect right_sidebar_rect();
    int  history_visible_rows(const Rect& hb);
    bool route_mouse(const opentui::MouseEvent& ev);
    bool service_mouse_switch();

    // ── input_loop.cpp ─────────────────────────────────────────────────────
    void run_input_loop();
};

}  // namespace arbiter
