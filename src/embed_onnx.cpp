// arbiter/src/embed_onnx.cpp — ONNX Runtime session for bge-small-en-v1.5.
//
// Why this stack: the model is a BERT WordPiece graph. ONNX Runtime's C API
// is the small CPU runtime that executes the published int8 file. The
// tokenizer is in-tree (src/embed_route.cpp) so sanitizer builds can omit
// this translation unit entirely instead of linking an unsanitized library
// plus a second tokenizer package.
//
// Pooling matches BGE: the first token of last_hidden_state, then L2 norm.
// Queries and agent texts use the same path (no instruction prefix).

#include "embed_onnx.h"

#include "embed_route.h"

#include <onnxruntime_c_api.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace arbiter {

namespace {

bool ort_ok(const OrtApi* api, OrtStatus* status, std::string& error) {
    if (!status) return true;
    const char* message = api->GetErrorMessage(status);
    error = message ? message : "onnx runtime error";
    api->ReleaseStatus(status);
    return false;
}

} // namespace

struct OnnxTextEmbedder::Impl {
    std::mutex mu;
    std::atomic<bool> ready{false};
    bool gave_up = false;
    std::string error = "embedding model not loaded";
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* options = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::vector<std::string> input_names;
    WordPieceVocab vocab;

    ~Impl() { release(); }

    // Drop the session while ONNX Runtime's own mutexes are still alive.
    // Releasing from a static destructor on macOS throws system_error
    // ("mutex lock failed: Invalid argument") and aborts the process.
    void release() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu);
            ready.store(false, std::memory_order_release);
            if (!api) return;
            const OrtApi* ort = api;
            if (session) ort->ReleaseSession(session);
            if (memory) ort->ReleaseMemoryInfo(memory);
            if (options) ort->ReleaseSessionOptions(options);
            if (env) ort->ReleaseEnv(env);
            session = nullptr;
            memory = nullptr;
            options = nullptr;
            env = nullptr;
            api = nullptr;
        } catch (...) {}
    }
};

void OnnxTextEmbedder::release_at_exit() {
    instance().impl_->release();
}

OnnxTextEmbedder::OnnxTextEmbedder() : impl_(std::make_unique<Impl>()) {}
OnnxTextEmbedder::~OnnxTextEmbedder() = default;

OnnxTextEmbedder& OnnxTextEmbedder::instance() {
    static OnnxTextEmbedder inst;
    return inst;
}

bool OnnxTextEmbedder::prepare() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->ready.load(std::memory_order_acquire)) return true;
    if (impl_->gave_up) return false;

    auto fail = [&](const std::string& message) {
        impl_->error = message;
        impl_->gave_up = true;
        return false;
    };

    EmbedAssetPaths assets = resolve_embed_assets();
    if (assets.model.empty() || assets.vocab.empty()) {
        return fail(assets.error.empty() ? "embedding model unavailable" : assets.error);
    }
    if (!wordpiece_load_file(impl_->vocab, assets.vocab)) {
        return fail("cannot load WordPiece vocab");
    }

    const OrtApiBase* base = OrtGetApiBase();
    if (!base) return fail("OrtGetApiBase returned null");
    impl_->api = base->GetApi(ORT_API_VERSION);
    if (!impl_->api) return fail("ONNX Runtime API version mismatch");
    const OrtApi* api = impl_->api;

    if (!ort_ok(api, api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "arbiter-embed", &impl_->env),
                impl_->error)) {
        impl_->gave_up = true;
        return false;
    }
    if (!ort_ok(api, api->CreateSessionOptions(&impl_->options), impl_->error)) {
        impl_->gave_up = true;
        return false;
    }
    // One intra-op thread: these sequences are short, and a thread pool
    // costs more than it saves on a single routing query.
    if (OrtStatus* status = api->SetIntraOpNumThreads(impl_->options, 1))
        api->ReleaseStatus(status);
    if (OrtStatus* status = api->SetInterOpNumThreads(impl_->options, 1))
        api->ReleaseStatus(status);
    if (OrtStatus* status = api->SetSessionGraphOptimizationLevel(impl_->options, ORT_ENABLE_ALL))
        api->ReleaseStatus(status);
    if (!ort_ok(api, api->CreateSession(impl_->env, assets.model.c_str(), impl_->options,
                                        &impl_->session),
                impl_->error)) {
        impl_->gave_up = true;
        return false;
    }
    if (!ort_ok(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &impl_->memory),
                impl_->error)) {
        impl_->gave_up = true;
        return false;
    }

    OrtAllocator* allocator = nullptr;
    if (!ort_ok(api, api->GetAllocatorWithDefaultOptions(&allocator), impl_->error)) {
        impl_->gave_up = true;
        return false;
    }
    size_t n_in = 0;
    if (!ort_ok(api, api->SessionGetInputCount(impl_->session, &n_in), impl_->error)) {
        impl_->gave_up = true;
        return false;
    }
    impl_->input_names.reserve(n_in);
    for (size_t i = 0; i < n_in; ++i) {
        char* name = nullptr;
        if (!ort_ok(api, api->SessionGetInputName(impl_->session, i, allocator, &name),
                    impl_->error)) {
            impl_->gave_up = true;
            return false;
        }
        impl_->input_names.emplace_back(name ? name : "");
        if (name) allocator->Free(allocator, name);
    }
    bool have_ids = false;
    bool have_mask = false;
    for (const auto& name : impl_->input_names) {
        if (name == "input_ids") have_ids = true;
        if (name == "attention_mask") have_mask = true;
    }
    if (!have_ids || !have_mask) return fail("model is missing input_ids or attention_mask");

    impl_->error.clear();
    impl_->ready.store(true, std::memory_order_release);
    // Run before C++ static destructors so ReleaseSession happens while
    // the runtime's internal mutexes still exist.
    static bool exit_hook = false;
    if (!exit_hook) {
        exit_hook = true;
        std::atexit(OnnxTextEmbedder::release_at_exit);
    }
    return true;
}

bool OnnxTextEmbedder::ready() const {
    return impl_->ready.load(std::memory_order_acquire);
}

const std::string& OnnxTextEmbedder::last_error() const { return impl_->error; }

std::vector<float> OnnxTextEmbedder::embed(const std::string& text) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<float> empty;
    if (!impl_->ready.load(std::memory_order_relaxed) || !impl_->api || !impl_->session)
        return empty;
    const OrtApi* api = impl_->api;

    const std::vector<int64_t> ids = wordpiece_encode(impl_->vocab, text, 512);
    if (ids.size() < 2) return empty;
    std::vector<int64_t> mask(ids.size(), 1);
    std::vector<int64_t> types(ids.size(), 0);
    const int64_t shape[2] = {1, static_cast<int64_t>(ids.size())};

    std::vector<const char*> names;
    std::vector<OrtValue*> values;
    names.reserve(impl_->input_names.size());
    values.reserve(impl_->input_names.size());
    std::string error;
    auto release_values = [&]() {
        for (OrtValue* value : values) {
            if (value) api->ReleaseValue(value);
        }
    };

    for (const auto& name : impl_->input_names) {
        const int64_t* data = nullptr;
        std::size_t bytes = 0;
        if (name == "input_ids") {
            data = ids.data();
            bytes = ids.size() * sizeof(int64_t);
        } else if (name == "attention_mask") {
            data = mask.data();
            bytes = mask.size() * sizeof(int64_t);
        } else if (name == "token_type_ids") {
            data = types.data();
            bytes = types.size() * sizeof(int64_t);
        } else {
            impl_->error = "unexpected model input " + name;
            release_values();
            return empty;
        }
        OrtValue* value = nullptr;
        // The buffer is caller-owned and must outlive Run. ids/mask/types do.
        if (!ort_ok(api, api->CreateTensorWithDataAsOrtValue(
                             impl_->memory, const_cast<int64_t*>(data), bytes, shape, 2,
                             ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &value),
                    error)) {
            impl_->error = error;
            release_values();
            return empty;
        }
        names.push_back(name.c_str());
        values.push_back(value);
    }

    const char* output_name = "last_hidden_state";
    OrtValue* output = nullptr;
    OrtStatus* run = api->Run(impl_->session, nullptr, names.data(), values.data(),
                              names.size(), &output_name, 1, &output);
    release_values();
    if (!ort_ok(api, run, error) || !output) {
        impl_->error = error.empty() ? "onnx run failed" : error;
        if (output) api->ReleaseValue(output);
        return empty;
    }

    float* data = nullptr;
    if (!ort_ok(api, api->GetTensorMutableData(output, reinterpret_cast<void**>(&data)), error) ||
        !data) {
        impl_->error = error.empty() ? "onnx output missing" : error;
        api->ReleaseValue(output);
        return empty;
    }
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (!ort_ok(api, api->GetTensorTypeAndShape(output, &info), error) || !info) {
        impl_->error = error.empty() ? "onnx output shape missing" : error;
        api->ReleaseValue(output);
        return empty;
    }
    size_t rank = 0;
    if (!ort_ok(api, api->GetDimensionsCount(info, &rank), error)) {
        impl_->error = error;
        api->ReleaseTensorTypeAndShapeInfo(info);
        api->ReleaseValue(output);
        return empty;
    }
    std::vector<int64_t> dims(rank);
    if (rank > 0 && !ort_ok(api, api->GetDimensions(info, dims.data(), rank), error)) {
        impl_->error = error;
        api->ReleaseTensorTypeAndShapeInfo(info);
        api->ReleaseValue(output);
        return empty;
    }
    ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    if (!ort_ok(api, api->GetTensorElementType(info, &dtype), error)) {
        impl_->error = error;
        api->ReleaseTensorTypeAndShapeInfo(info);
        api->ReleaseValue(output);
        return empty;
    }
    api->ReleaseTensorTypeAndShapeInfo(info);
    if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || dims.empty()) {
        impl_->error = "expected a float hidden state";
        api->ReleaseValue(output);
        return empty;
    }
    const int64_t hidden = dims.back();
    if (hidden <= 0) {
        api->ReleaseValue(output);
        return empty;
    }
    // Rank 3 is [batch, seq, hidden]; token 0 is [CLS]. Rank 2 is already pooled.
    std::vector<float> vec(static_cast<std::size_t>(hidden));
    for (int64_t i = 0; i < hidden; ++i) vec[static_cast<std::size_t>(i)] = data[i];
    api->ReleaseValue(output);

    double sum = 0;
    for (float x : vec) sum += static_cast<double>(x) * static_cast<double>(x);
    if (sum > 0.0 && std::isfinite(sum)) {
        const float inv = static_cast<float>(1.0 / std::sqrt(sum));
        for (float& x : vec) x *= inv;
    }
    return vec;
}

} // namespace arbiter
