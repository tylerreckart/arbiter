// arbiter/src/api/event_logger.cpp

#include "api/event_logger.h"
#include "api/sse_stream.h"

namespace arbiter {

void emit_error(SseStream& sse, const std::string& msg) {
    auto o = jobj();
    o->as_object_mut()["message"] = jstr(msg);
    sse.emit("error", o);
}

} // namespace arbiter
