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

// RFC 6750 §2.1 / RFC 9110 §11.1: parse an Authorization field value.
// Auth-scheme is case-insensitive; one or more SP separate scheme and
// token.  Returns empty when the field is not Bearer credentials.
std::string parse_bearer_authorization(const std::string& field_value);

} // namespace arbiter
