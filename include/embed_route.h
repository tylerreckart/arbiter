#pragma once
// Local embedding router: nearest centroid over each specialist's
// description and optional routing examples, then a temperature softmax.
//
// The model is bge-small-en-v1.5 (BERT WordPiece, CLS pooling, L2 norm).
// Queries and centroids share that space, so there is no retrieval
// instruction prefix. The session loads once. Centroids are rebuilt only
// when the roster text changes.
//
// When the runtime or the weight file is absent, EmbeddingRouter::prepare
// fails and ingress uses the heuristic classifier. ARBITER_EMBED_NO_FETCH
// and ARBITER_OFFLINE skip the download.

#include "decision.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arbiter {

struct EmbedAssetPaths {
    std::string model;
    std::string vocab;
    std::string error;  // set when both paths could not be resolved
};

// Pinned files. Operator overrides: ARBITER_EMBED_MODEL and
// ARBITER_EMBED_VOCAB (both required; checksum is not applied). Otherwise
// a configure-time copy, then ~/.cache/arbiter/models/bge-small-en-v1.5/.
// Download runs unless ARBITER_OFFLINE or ARBITER_EMBED_NO_FETCH is set.
EmbedAssetPaths resolve_embed_assets();

// Softmax temperature and the confidence cutoff live with the route
// defaults. These are the same numbers intent_route_defaults() copies.
inline constexpr double kEmbedDefaultTemperature = 0.02;
inline constexpr double kEmbedDefaultMinConfidence = 0.35;

// BERT WordPiece. Tests load a tiny vocab; the ONNX path loads vocab.txt.
struct WordPieceVocab {
    std::unordered_map<std::string, int> ids;
    int cls_id = -1;
    int sep_id = -1;
    int unk_id = -1;
    int pad_id = -1;
};

bool wordpiece_load_lines(WordPieceVocab& vocab, const std::vector<std::string>& lines);
bool wordpiece_load_file(WordPieceVocab& vocab, const std::string& path);
// [CLS] + pieces + [SEP], truncated to max_len. Empty if specials are missing.
std::vector<int64_t> wordpiece_encode(const WordPieceVocab& vocab,
                                      const std::string& text,
                                      int max_len);

struct Centroid {
    std::string id;
    std::vector<float> vector;
};

// Cosine similarity (after L2-normalizing copies) then softmax(sim / T).
// Confidence is decision_confidence(p_max, centroids.size()).
DecisionResult score_embedding_centroids(const std::vector<float>& query,
                                         const std::vector<Centroid>& centroids,
                                         double temperature);

using EmbedFn = std::function<std::vector<float>(const std::string& text)>;

// decide() embeds request.state and each option's description plus examples.
class EmbeddingCentroidBackend : public DecisionBackend {
public:
    void set_embed(EmbedFn fn);
    void set_temperature(double temperature);
    // Builds centroids without embedding a query. Used at startup.
    bool precompute(const std::vector<DecisionOption>& options, std::string& error);

    DecisionResult decide(const DecisionRequest& request) override;
    const char* name() const override { return "embed-centroid"; }

private:
    EmbedFn embed_;
    double temperature_ = kEmbedDefaultTemperature;
    std::string fingerprint_;
    std::vector<Centroid> centroids_;

    bool ensure_centroids(const std::vector<DecisionOption>& options, std::string& error);
};

// Process-wide backend. Model load and centroid cache live here.
class EmbeddingRouter {
public:
    static EmbeddingRouter& instance();

    bool prepare();
    bool ready() const;
    const std::string& last_error() const;
    void precompute(const std::vector<DecisionOption>& options);
    DecisionResult decide(const DecisionRequest& request, double temperature);

    EmbeddingRouter(const EmbeddingRouter&) = delete;
    EmbeddingRouter& operator=(const EmbeddingRouter&) = delete;

private:
    EmbeddingRouter() = default;

    mutable std::mutex mu_;
    EmbeddingCentroidBackend backend_;
    bool embed_set_ = false;
    std::string error_;
};

// Description is role + goal. Up to 8 routing examples are kept (centroid
// cost, not a confidence cutoff). "index" is skipped.
std::vector<DecisionOption> embed_options_from_roster(
    const std::vector<IntentRosterEntry>& roster);

// ARBITER_INTENT_SHADOW=embed. Does not change routing.
bool intent_shadow_embed_enabled();

void shadow_embed_decision(const std::string& text,
                           const std::vector<IntentRosterEntry>& roster,
                           const Intent& alongside);

} // namespace arbiter
