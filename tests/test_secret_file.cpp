// tests/test_secret_file.cpp — API-key / admin-token dest writes must
// not follow a planted symlink or hang on a FIFO.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "secret_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace arbiter;
namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& path) {
    std::ifstream f(path);
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

fs::path make_temp_dir(const char* tag) {
    const auto dir = fs::temp_directory_path()
        / (std::string(tag) + "-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("write_secret_file creates a 0600 regular file") {
    const auto dir = make_temp_dir("arbiter-secret-create");
    const auto path = dir / "admin_token";

    REQUIRE(write_secret_file(path.string(), "adm_fresh-token\n"));
    CHECK(read_all(path) == "adm_fresh-token\n");
    CHECK(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_symlink(path));
    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    fs::remove_all(dir);
}

TEST_CASE("write_secret_file overwrites an existing regular file") {
    const auto dir = make_temp_dir("arbiter-secret-overwrite");
    const auto path = dir / "openrouter_api_key";
    {
        std::ofstream f(path);
        f << "old-key\n";
    }
    ::chmod(path.c_str(), 0644);

    REQUIRE(write_secret_file(path.string(), "sk-new-key\n"));
    CHECK(read_all(path) == "sk-new-key\n");
    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    fs::remove_all(dir);
}

TEST_CASE("write_secret_file does not follow a planted dest symlink") {
    const auto dir = make_temp_dir("arbiter-secret-dest-link");
    const auto dest = dir / "admin_token";
    const auto victim = dir / "secret.txt";
    {
        std::ofstream f(victim);
        f << "do-not-clobber";
    }
    fs::create_symlink(victim, dest);

    CHECK_FALSE(write_secret_file(dest.string(), "adm_LEAKED_TOKEN\n"));
    CHECK(read_all(victim) == "do-not-clobber");
    CHECK(fs::is_symlink(dest));

    fs::remove_all(dir);
}

TEST_CASE("write_secret_file refuses a planted dest FIFO without hanging") {
    const auto dir = make_temp_dir("arbiter-secret-fifo");
    const auto dest = dir / "search_api_key";
    REQUIRE(::mkfifo(dest.c_str(), 0600) == 0);

    CHECK_FALSE(write_secret_file(dest.string(), "search-secret\n"));
    CHECK(fs::is_fifo(dest));

    fs::remove_all(dir);
}

TEST_CASE("write_secret_file refuses a directory dest") {
    const auto dir = make_temp_dir("arbiter-secret-dir");
    CHECK_FALSE(write_secret_file(dir.string(), "nope\n"));
    CHECK(fs::is_directory(dir));

    fs::remove_all(dir);
}
