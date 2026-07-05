#pragma once
// Minimal C declarations for OpenTUI's native core (packages/core/src/zig/lib.zig).
// Full ABI lives in the OpenTUI repo; this header covers the native calls used
// by Arbiter's interactive renderer.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t OpenTuiHandle;
typedef void (*OpenTuiLogCallback)(uint8_t level, const char* msg, uint32_t len);

void setLogCallback(OpenTuiLogCallback callback);

OpenTuiHandle createRenderer(uint32_t width,
                             uint32_t height,
                             uint8_t buffered_destination_kind,
                             uint8_t remote_mode_value,
                             void* feed_ptr);

void destroyRenderer(OpenTuiHandle renderer);
void setUseThread(OpenTuiHandle renderer, bool use_thread);
void setClearOnShutdown(OpenTuiHandle renderer, bool clear);
void setBackgroundColor(OpenTuiHandle renderer, const uint16_t* color);
typedef struct OpenTuiCursorStyleOptions {
    uint8_t style;
    bool blinking;
    const uint16_t* color;
    uint8_t cursor;
} OpenTuiCursorStyleOptions;
void setCursorStyleOptions(OpenTuiHandle renderer, const OpenTuiCursorStyleOptions* options);

OpenTuiHandle getNextBuffer(OpenTuiHandle renderer);
uint32_t getBufferWidth(OpenTuiHandle buffer);
uint32_t getBufferHeight(OpenTuiHandle buffer);

void bufferFillRect(OpenTuiHandle buffer,
                    uint32_t x,
                    uint32_t y,
                    uint32_t width,
                    uint32_t height,
                    const uint16_t* bg);

void bufferClear(OpenTuiHandle buffer, const uint16_t* bg);

void bufferDrawText(OpenTuiHandle buffer,
                    const char* text,
                    uint32_t text_len,
                    uint32_t x,
                    uint32_t y,
                    const uint16_t* fg,
                    const uint16_t* bg,
                    uint32_t attributes);

void setupTerminal(OpenTuiHandle renderer, bool use_alternate_screen);
void restoreTerminalModes(OpenTuiHandle renderer);
void processCapabilityResponse(OpenTuiHandle renderer,
                               const char* response,
                               uint32_t response_len);
void resizeRenderer(OpenTuiHandle renderer, uint32_t width, uint32_t height);
void setRenderOffset(OpenTuiHandle renderer, uint32_t offset);
uint8_t render(OpenTuiHandle renderer, bool force);

void bufferDrawTextBufferView(OpenTuiHandle buffer,
                              OpenTuiHandle view,
                              uint32_t x,
                              uint32_t y);

void bufferPushScissorRect(OpenTuiHandle buffer,
                           int32_t x,
                           int32_t y,
                           uint32_t width,
                           uint32_t height);

void bufferPopScissorRect(OpenTuiHandle buffer);

OpenTuiHandle createTextBuffer(uint8_t width_method);
void destroyTextBuffer(OpenTuiHandle buffer);
uint32_t textBufferGetLength(OpenTuiHandle buffer);
uint32_t textBufferGetByteSize(OpenTuiHandle buffer);
void textBufferReset(OpenTuiHandle buffer);
void textBufferClear(OpenTuiHandle buffer);
void textBufferAppend(OpenTuiHandle buffer, const char* data, uint32_t data_len);

OpenTuiHandle createSyntaxStyle();
void destroySyntaxStyle(OpenTuiHandle style);
uint32_t syntaxStyleRegister(OpenTuiHandle style,
                              const char* name,
                              uint32_t name_len,
                              const uint16_t* fg,
                              const uint16_t* bg,
                              uint32_t attributes);
bool textBufferSetSyntaxStyle(OpenTuiHandle buffer, OpenTuiHandle style);

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t style_id;
    uint8_t  priority;
    uint16_t hl_ref;
} OpenTuiHighlight;

void textBufferAddHighlightByCharRange(OpenTuiHandle buffer,
                                       const OpenTuiHighlight* hl);
void textBufferClearAllHighlights(OpenTuiHandle buffer);

OpenTuiHandle createTextBufferView(OpenTuiHandle buffer);
void destroyTextBufferView(OpenTuiHandle view);
void textBufferViewSetWrapWidth(OpenTuiHandle view, uint32_t width);
void textBufferViewSetWrapMode(OpenTuiHandle view, uint8_t mode);
void textBufferViewSetViewport(OpenTuiHandle view,
                               uint32_t x,
                               uint32_t y,
                               uint32_t width,
                               uint32_t height);
void textBufferViewSetFirstLineOffset(OpenTuiHandle view, uint32_t offset);
uint32_t textBufferViewGetVirtualLineCount(OpenTuiHandle view);

OpenTuiHandle createEditBuffer(uint8_t width_method, OpenTuiHandle event_sink);
void destroyEditBuffer(OpenTuiHandle edit);
void editBufferClear(OpenTuiHandle edit);
void editBufferInsertText(OpenTuiHandle edit, const char* text, uint32_t text_len);
void editBufferDeleteCharBackward(OpenTuiHandle edit);
void editBufferDeleteChar(OpenTuiHandle edit);
void editBufferMoveCursorLeft(OpenTuiHandle edit);
void editBufferMoveCursorRight(OpenTuiHandle edit);
void editBufferSetText(OpenTuiHandle edit, const char* text, uint32_t text_len);
void editBufferSetCursorByOffset(OpenTuiHandle edit, uint32_t offset);
void editBufferGetText(OpenTuiHandle edit, uint8_t* out, uint32_t max_len);

OpenTuiHandle createEditorView(OpenTuiHandle edit,
                               uint32_t viewport_width,
                               uint32_t viewport_height);
void destroyEditorView(OpenTuiHandle view);
void editorViewSetViewport(OpenTuiHandle view,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height,
                           bool move_cursor);
void editorViewSetViewportSize(OpenTuiHandle view, uint32_t width, uint32_t height);
void editorViewSetWrapMode(OpenTuiHandle view, uint8_t mode);

void bufferDrawEditorView(OpenTuiHandle buffer, OpenTuiHandle view, int32_t x, int32_t y);

// --- DiffView (native DiffRenderable) ----------------------------------------
// Parses unified diff text and renders with line numbers, +/- signs, and
// added/removed/context backgrounds.  Composes existing TextBuffer draw paths.

typedef struct OpenTuiDiffOptions {
    uint8_t  view_mode;          // 0=unified, 1=split
    uint8_t  wrap_mode;          // 0=none, 1=char, 2=word
    bool     show_line_numbers;
    uint16_t added_bg[4];
    uint16_t removed_bg[4];
    uint16_t context_bg[4];
    uint16_t line_number_fg[4];
    uint16_t added_sign_color[4];
    uint16_t removed_sign_color[4];
} OpenTuiDiffOptions;

OpenTuiHandle createDiffView(const OpenTuiDiffOptions* opts);
void destroyDiffView(OpenTuiHandle diff);

bool diffViewSetPatch(OpenTuiHandle diff, const char* patch, uint32_t patch_len);
bool diffViewSetViewMode(OpenTuiHandle diff, uint8_t mode);
void diffViewSetWrapMode(OpenTuiHandle diff, uint8_t mode);
void diffViewSetWrapWidth(OpenTuiHandle diff, uint32_t content_width);
void diffViewSetScrollY(OpenTuiHandle diff, uint32_t offset);

uint32_t diffViewGetVirtualLineCount(OpenTuiHandle diff);
uint32_t diffViewGetHunkCount(OpenTuiHandle diff);
uint32_t diffViewGetHunkStartLine(OpenTuiHandle diff, uint32_t hunk_index);

void bufferDrawDiffView(OpenTuiHandle buffer,
                        OpenTuiHandle diff,
                        int32_t x,
                        int32_t y,
                        uint32_t width,
                        uint32_t height);

#ifdef __cplusplus
}
#endif
