// index_ai/src/auth.cpp
#include "auth.h"
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

#include <sys/stat.h>
#include <sys/types.h>

namespace index_ai {

static std::string bytes_to_hex(const unsigned char* data, size_t len) {
    std::ostringstream ss;
    for (size_t i = 0; i < len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return ss.str();
}

std::string Auth::generate_token() {
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        throw std::runtime_error("CSPRNG failure");
    return bytes_to_hex(buf, sizeof(buf));
}

std::string Auth::hash_token(const std::string& token) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(token.data()),
           token.size(), hash);
    return bytes_to_hex(hash, SHA256_DIGEST_LENGTH);
}

void Auth::add_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    token_hashes_.insert(hash_token(token));
}

void Auth::revoke_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    token_hashes_.erase(hash_token(token));
}

bool Auth::validate(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token_hashes_.count(hash_token(token)) > 0;
}

int Auth::token_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(token_hashes_.size());
}

void Auth::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] != '#')
            token_hashes_.insert(line);
    }
}

void Auth::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot write auth file: " + path);
    f << "# index_ai auth tokens (hashed)\n";
    for (auto& h : token_hashes_) f << h << "\n";
    f.close();
    // Restrict to owner-only — hashed or not, the token list is sensitive
    // and has no reason to be world- or group-readable.  umask might have
    // left this 0644 (a typical default), which would expose the hashes to
    // other users on a shared system.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
}

} // namespace index_ai
