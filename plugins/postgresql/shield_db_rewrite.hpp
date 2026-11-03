// [SHIELD_PLUGIN] database.postgresql — ? → $N placeholder rewriting.
//
// libpq (PostgreSQL) uses $1, $2, ... positional parameters while shield's
// other SQL plugins use ? (MySQL/SQLite style). This header implements the
// rewrite as an inline function so both the plugin source and the unit test
// can use the exact same code (the test links it directly instead of loading
// the MODULE).
//
// The scanner is SQL-aware for the constructs that legitimately contain '?'
// but must not be treated as placeholders:
//   * single-quoted string literals (', with '' escaping, E'...' escapes)
//   * double-quoted identifiers ("...", with "" escaping)
//   * line comments (-- ...)
//   * block comments (/* ... */, non-nested per PostgreSQL)
//   * dollar-quoted strings ($tag$ ... $tag$, e.g. $body$ ... $body$)
//
// It intentionally does NOT understand PostgreSQL operator forms that use '?'
// (the JSON/JSONB ? / ?| / ?& operators, or the geometric ~ operators). The
// shield::data SQL helpers never emit those, and a '?' outside the quoted
// constructs above is treated as a placeholder, which matches the v1
// documented behaviour.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace shield::db::pgsql {

inline std::string rewrite_placeholders(const char* sql) {
    std::string out;
    if (!sql) return out;
    out.reserve(std::strlen(sql) + 8);
    int index = 0;
    const char* p = sql;
    while (*p) {
        // E'...' / e'...' escape-string constant: backslash escapes the next
        // character (including a quote). Detected before the plain literal
        // arm so its escape mode is honoured.
        const bool is_escape_string = (*p == 'E' || *p == 'e') && p[1] == '\'';
        // Single-quoted string literal ('' escapes a quote; E'...' / e'...'
        // uses backslash escapes, handled by the same single-quote scan).
        if (is_escape_string) {
            out.push_back(*p++);  // E or e
            out.push_back(*p++);  // opening quote
            while (*p) {
                if (*p == '\\' && p[1]) {
                    out.push_back(*p++);
                    out.push_back(*p++);
                    continue;
                }
                if (*p == '\'') {
                    out.push_back(*p++);
                    break;
                }
                out.push_back(*p++);
            }
            continue;
        }
        if (*p == '\'') {
            out.push_back(*p++);
            while (*p) {
                out.push_back(*p);
                if (*p == '\'' && p[1] == '\'') {
                    out.push_back(p[1]);
                    p += 2;
                    continue;
                }
                if (*p == '\'') {
                    ++p;
                    break;
                }
                ++p;
            }
            continue;
        }
        // Double-quoted identifier ("" escapes a quote).
        if (*p == '"') {
            out.push_back(*p++);
            while (*p) {
                out.push_back(*p);
                if (*p == '"' && p[1] == '"') {
                    out.push_back(p[1]);
                    p += 2;
                    continue;
                }
                if (*p == '"') {
                    ++p;
                    break;
                }
                ++p;
            }
            continue;
        }
        // Line comment.
        if (*p == '-' && p[1] == '-') {
            out.push_back(*p++);
            out.push_back(*p++);
            while (*p && *p != '\n') out.push_back(*p++);
            continue;
        }
        // Block comment.
        if (*p == '/' && p[1] == '*') {
            out.push_back(*p++);
            out.push_back(*p++);
            while (*p) {
                if (*p == '*' && p[1] == '/') {
                    out.push_back(*p++);
                    out.push_back(*p++);
                    break;
                }
                out.push_back(*p++);
            }
            continue;
        }
        // Dollar-quoted string: $tag$ ... $tag$ (tag may be empty).
        if (*p == '$') {
            const char* tag_end = std::strchr(p + 1, '$');
            if (tag_end) {
                // A valid tag is [A-Za-z_][A-Za-z0-9_]* or empty.
                bool valid = true;
                for (const char* q = p + 1; q < tag_end; ++q) {
                    if (!(std::isalnum(static_cast<unsigned char>(*q)) ||
                          *q == '_')) {
                        valid = false;
                        break;
                    }
                }
                if (valid && (tag_end == p + 1 ||
                              std::isalpha(static_cast<unsigned char>(p[1])) ||
                              p[1] == '_')) {
                    const std::string delim(p, tag_end + 1);
                    out += delim;
                    p = tag_end + 1;
                    // Find the closing delimiter.
                    const char* close = std::strstr(p, delim.c_str());
                    if (!close) {
                        out += p;
                        p += std::strlen(p);
                    } else {
                        out.append(p, close - p);
                        out += delim;
                        p = close + delim.size();
                    }
                    continue;
                }
            }
        }
        // Ordinary placeholder.
        if (*p == '?') {
            out.push_back('$');
            char buf[12];
            std::snprintf(buf, sizeof(buf), "%d", ++index);
            out += buf;
            ++p;
        } else {
            out.push_back(*p++);
        }
    }
    return out;
}

}  // namespace shield::db::pgsql
