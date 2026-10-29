/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "json_extract.h"

#include <cstring>
#include <limits>

namespace pcm_json
{

std::size_t skipContainer(const std::string& s, std::size_t i, std::size_t end)
{
    if (i >= end) return end;
    const char open  = s[i];
    char close;
    if (open == '{') close = '}';
    else if (open == '[') close = ']';
    else return end;
    std::size_t depth = 1U;
    ++i;
    while (i < end && depth > 0U)
    {
        const char c = s[i];
        if (c == '"')
        {
            const std::size_t after = skipJsonString(s, i, end);
            if (after == std::string::npos) return end;
            i = after;
            continue;
        }
        if (c == open) ++depth;
        else if (c == close) --depth;
        ++i;
    }
    return (depth == 0U) ? i : end;
}

std::size_t skipValue(const std::string& s, std::size_t i, std::size_t end)
{
    i = skipWs(s, i, end);
    if (i >= end) return end;
    const char c = s[i];
    if (c == '"') return skipJsonString(s, i, end);
    if (c == '{' || c == '[') return skipContainer(s, i, end);
    /* Number / true / false / null: scan to the delimiter. */
    while (i < end)
    {
        const char ch = s[i];
        if (ch == ',' || ch == '}' || ch == ']' || isWs(ch)) break;
        ++i;
    }
    return i;
}

std::size_t findKey(const std::string& s, const std::string& key,
                    std::size_t from, std::size_t end)
{
    if (end == std::string::npos) end = s.size();
    std::size_t i = from;
    /* Tolerate the caller pointing at the opening '{'. */
    if (i < end)
    {
        const std::size_t ws = skipWs(s, i, end);
        if (ws < end && s[ws] == '{') i = ws + 1U;
    }
    while (i < end)
    {
        i = skipWs(s, i, end);
        if (i >= end) break;
        const char c = s[i];
        if (c == ',') { ++i; continue; }
        if (c == '}') break;
        if (c != '"') return std::string::npos;  /* malformed */

        const std::size_t nameStart = i + 1U;
        const std::size_t after = skipJsonString(s, i, end);
        if (after == std::string::npos) return std::string::npos;
        const std::size_t nameEnd = after - 1U;

        std::size_t k = skipWs(s, after, end);
        if (k >= end || s[k] != ':') return std::string::npos;
        ++k;
        k = skipWs(s, k, end);
        if (k >= end) return std::string::npos;

        const bool match = (nameEnd - nameStart == key.size() &&
                            std::memcmp(s.data() + nameStart, key.data(), key.size()) == 0);
        if (match) return k;
        i = skipValue(s, k, end);
    }
    return std::string::npos;
}

bool findObject(const std::string& s, const std::string& key,
                std::size_t from, std::size_t end,
                std::size_t& objStart, std::size_t& objEnd)
{
    const std::size_t p = findKey(s, key, from, end);
    if (p == std::string::npos) return false;
    if (p >= end || s[p] != '{') return false;
    const std::size_t after = skipContainer(s, p, end);
    if (after >= end + 1U || after <= p + 1U) return false;
    objStart = p + 1U;
    objEnd   = after - 1U;
    return true;
}

bool extractString(const std::string& s, const std::string& key,
                   std::string& out, std::size_t from,
                   std::size_t end)
{
    if (end == std::string::npos) end = s.size();
    const std::size_t p = findKey(s, key, from, end);
    if (p == std::string::npos) return false;
    if (p >= end || s[p] != '"') return false;
    const std::size_t after = skipJsonString(s, p, end);
    if (after == std::string::npos) return false;
    out.clear();
    /* Unescape \\ \" \/ \n \r \t \b \f; \uXXXX passes through as-is. */
    for (std::size_t i = p + 1U; i + 1U < after; ++i)
    {
        char c = s[i];
        if (c == '\\' && i + 2U < after)
        {
            const char e = s[i + 1U];
            switch (e)
            {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                default:   out.push_back('\\'); out.push_back(e); break;
            }
            ++i;
        }
        else
        {
            out.push_back(c);
        }
    }
    return true;
}

bool extractBool(const std::string& s, const std::string& key,
                 bool& out, std::size_t from,
                 std::size_t end)
{
    if (end == std::string::npos) end = s.size();
    const std::size_t p = findKey(s, key, from, end);
    if (p == std::string::npos) return false;
    /* A Boolean prefix in malformed input (for example truegarbage) is not a
     * usable Docker state. Require the complete token and a value delimiter. */
    const auto terminated = [&](std::size_t next) {
        next = skipWs(s, next, end);
        return next == end || s[next] == ',' || s[next] == '}' || s[next] == ']';
    };
    if (p + 4U <= end && std::memcmp(s.data() + p, "true", 4U) == 0 && terminated(p + 4U)) {
        out = true;
        return true;
    }
    if (p + 5U <= end && std::memcmp(s.data() + p, "false", 5U) == 0 && terminated(p + 5U)) {
        out = false;
        return true;
    }
    return false;
}

bool extractInt(const std::string& s, const std::string& key,
                int& out, std::size_t from,
                std::size_t end)
{
    if (end == std::string::npos) end = s.size();
    std::size_t p = findKey(s, key, from, end);
    if (p == std::string::npos) return false;

    int sign = 1;
    if (p < end && s[p] == '-')
    {
        sign = -1;
        ++p;
    }
    if (p >= end || s[p] < '0' || s[p] > '9') return false;

    long long value = 0;
    while (p < end && s[p] >= '0' && s[p] <= '9')
    {
        value = (value * 10) + (s[p] - '0');
        if (sign > 0 && value > std::numeric_limits<int>::max()) return false;
        if (sign < 0 && -value < std::numeric_limits<int>::min()) return false;
        ++p;
    }

    out = static_cast<int>(sign * value);
    return true;
}

bool extractStringArrayFirst(const std::string& s, const std::string& key,
                             std::string& out, std::size_t from,
                             std::size_t end)
{
    if (end == std::string::npos) end = s.size();
    const std::size_t p = findKey(s, key, from, end);
    if (p == std::string::npos) return false;
    if (p >= end || s[p] != '[') return false;
    const std::size_t q = skipWs(s, p + 1U, end);
    if (q >= end || s[q] != '"') return false;
    const std::size_t after = skipJsonString(s, q, end);
    if (after == std::string::npos) return false;
    out.assign(s, q + 1U, after - q - 2U);
    return true;
}

}
