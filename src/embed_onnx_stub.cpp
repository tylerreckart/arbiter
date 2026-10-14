// arbiter/src/embed_onnx_stub.cpp — compiled when ONNX Runtime is not linked.
// Sanitizer jobs use this so they do not load an unsanitized libonnxruntime.
// Offline configure does too. Routing then keeps the heuristic.

#include "embed_onnx.h"

namespace arbiter {

struct OnnxTextEmbedder::Impl {
    std::string error = "onnx runtime not linked";
};

OnnxTextEmbedder::OnnxTextEmbedder() : impl_(std::make_unique<Impl>()) {}
OnnxTextEmbedder::~OnnxTextEmbedder() = default;

OnnxTextEmbedder& OnnxTextEmbedder::instance() {
    static OnnxTextEmbedder inst;
    return inst;
}

bool OnnxTextEmbedder::prepare() { return false; }

bool OnnxTextEmbedder::ready() const { return false; }

std::vector<float> OnnxTextEmbedder::embed(const std::string&) { return {}; }

const std::string& OnnxTextEmbedder::last_error() const { return impl_->error; }

} // namespace arbiter
