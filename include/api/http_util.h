#pragma once
// arbiter/include/api/http_util.h
//
// URL/path helpers for route dispatch.

#include <map>
#include <string>
#include <vector>

namespace arbiter {

std::vector<std::string> split_path(const std::string& path);
std::string url_decode(const std::string& s);
std::map<std::string, std::string> parse_query(const std::string& path);

} // namespace arbiter
