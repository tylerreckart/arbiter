#pragma once
// index/include/auth.h — Token-based auth for remote access

#include <string>
#include <unordered_set>
#include <mutex>

namespace index_ai {

class Auth {
public:
    // Generate a new access token (SHA-256 of random bytes)
    static std::string generate_token();

    // Hash a token for storage (we never store plaintext)
    static std::string hash_token(const std::string& token);

    // Token management
    void add_token(const std::string& token);
    void revoke_token(const std::string& token);
    bool validate(const std::string& token) const;

    // Load/save token hashes
    void load(const std::string& path);
    void save(const std::string& path) const;

    int token_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::string> token_hashes_;
};

} // namespace index_ai
