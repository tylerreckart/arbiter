#pragma once
// In-process text embedder. The real translation unit links ONNX Runtime
// and loads bge-small-en-v1.5 once. The stub translation unit is what
// sanitizer and offline builds compile: prepare() fails and routing falls
// back to the heuristic.

#include <memory>
#include <string>
#include <vector>

namespace arbiter {

class OnnxTextEmbedder {
public:
    static OnnxTextEmbedder& instance();

    // Resolve the pinned model and vocab, download them when that is
    // allowed, and load the session. A second call is a no-op. A failed
    // call stays failed for the process.
    bool prepare();
    bool ready() const;
    // L2-normalized CLS embedding. Empty on failure.
    std::vector<float> embed(const std::string& text);
    const std::string& last_error() const;

    OnnxTextEmbedder(const OnnxTextEmbedder&) = delete;
    OnnxTextEmbedder& operator=(const OnnxTextEmbedder&) = delete;

private:
    OnnxTextEmbedder();
    ~OnnxTextEmbedder();
    static void release_at_exit();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace arbiter
