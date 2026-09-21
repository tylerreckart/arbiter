// tests/test_atomic_file.cpp — crash-safe writes must not follow a
// planted `<path>.tmp` symlink (session / layout / migration markers).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "atomic_file.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace arbiter;
namespace fs = std::filesystem;

namespace {

std::string make_temp_dir() {
    static std::atomic<int> counter{0};
    std::random_device rd;
    std::ostringstream name;
    name << "arbiter_atomic_file_test_" << rd() << "_" << counter++;
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir.string();
}

std::string read_all(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_all(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << data;
}

} // namespace

TEST_CASE("atomic_write_file creates dest and leaves no stray tmp") {
    const std::string dir = make_temp_dir();
    const std::string path = dir + "/data.json";

    CHECK(atomic_write_file(path, "hello world"));
    CHECK(fs::is_regular_file(path));
    CHECK_FALSE(fs::exists(path + ".tmp"));
    CHECK(read_all(path) == "hello world");

    CHECK(atomic_write_file(path, "second write"));
    CHECK(read_all(path) == "second write");
    CHECK_FALSE(fs::exists(path + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("atomic_write_file writes an empty payload") {
    const std::string dir = make_temp_dir();
    const std::string path = dir + "/empty.json";

    CHECK(atomic_write_file(path, ""));
    CHECK(fs::is_regular_file(path));
    CHECK(read_all(path).empty());
    CHECK_FALSE(fs::exists(path + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("atomic_write_file replaces a leftover regular tmp") {
    const std::string dir = make_temp_dir();
    const std::string path = dir + "/data.json";
    write_all(path + ".tmp", "stale crash leftover");

    CHECK(atomic_write_file(path, "fresh"));
    CHECK(read_all(path) == "fresh");
    CHECK_FALSE(fs::exists(path + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("atomic_write_file does not follow a planted path.tmp symlink") {
    const std::string dir = make_temp_dir();
    const std::string path = dir + "/session.json";
    const std::string victim = dir + "/secret.txt";
    write_all(victim, "do-not-clobber");
    fs::create_symlink(victim, path + ".tmp");

    CHECK(atomic_write_file(path, "session-body"));

    CHECK(read_all(victim) == "do-not-clobber");
    CHECK(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_symlink(path));
    CHECK(read_all(path) == "session-body");
    CHECK_FALSE(fs::exists(path + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("atomic_write_file rename replaces a dest symlink instead of writing through it") {
    const std::string dir = make_temp_dir();
    const std::string path = dir + "/layout.json";
    const std::string victim = dir + "/outside.txt";
    write_all(victim, "keep-me");
    fs::create_symlink(victim, path);

    CHECK(atomic_write_file(path, "layout-body"));

    CHECK(read_all(victim) == "keep-me");
    CHECK(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_symlink(path));
    CHECK(read_all(path) == "layout-body");

    fs::remove_all(dir);
}
