#pragma once
// Minimal C declarations for OpenTUI's native core (packages/core/src/zig/lib.zig).
// Full ABI lives in the OpenTUI repo; this header covers Phase 0 spike surface.

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

OpenTuiHandle getNextBuffer(OpenTuiHandle renderer);
uint32_t getBufferWidth(OpenTuiHandle buffer);
uint32_t getBufferHeight(OpenTuiHandle buffer);

void bufferFillRect(OpenTuiHandle buffer,
                    uint32_t x,
                    uint32_t y,
                    uint32_t width,
                    uint32_t height,
                    const uint16_t* bg);

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
void resizeRenderer(OpenTuiHandle renderer, uint32_t width, uint32_t height);
uint8_t render(OpenTuiHandle renderer, bool force);

#ifdef __cplusplus
}
#endif
