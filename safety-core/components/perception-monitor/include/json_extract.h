/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_JSON_EXTRACT_H
#define PCM_JSON_EXTRACT_H

#include <cstddef>
#include <string>

/*
 * Targeted JSON field extraction for the PCM Docker client. Not a
 * general-purpose parser: just enough to pluck known fields from
 * well-formed Docker responses. Depth-aware (matches keys only at the
 * current object scope), string-aware, length-exact ("Image" != "ImageID").
 * Escape decode is minimal; \uXXXX is passed through verbatim. All
 * functions are re-entrant on the input string.
 */

namespace pcm_json
{

inline bool isWs(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline std::size_t skipWs(const std::string& s, std::size_t i, std::size_t end)
{
    while (i < end && isWs(s[i])) ++i;
    return i;
}

/* Advance past a JSON string (@p i points at the opening '"'). Returns
 * the position after the closing '"', or npos on malformed input. */
inline std::size_t skipJsonString(const std::string& s, std::size_t i, std::size_t end)
{
    if (i >= end || s[i] != '"') return std::string::npos;
    ++i;
    while (i < end)
    {
        const char c = s[i];
        if (c == '\\')
        {
            if (i + 1U >= end) return std::string::npos;
            i += 2U;
            continue;
        }
        if (c == '"') return i + 1U;
        ++i;
    }
    return std::string::npos;
}

/* Skip a JSON container ({...} or [...]) at @p i. Returns the position
 * after the matching close, or @p end on malformed input. */
std::size_t skipContainer(const std::string& s, std::size_t i, std::size_t end);

/* Skip a JSON value at @p i. Returns the position after the value. */
std::size_t skipValue(const std::string& s, std::size_t i, std::size_t end);

/* Find a key at the current object scope (depth-aware). Returns the
 * position of the value, or npos. */
std::size_t findKey(const std::string& s, const std::string& key,
                    std::size_t from, std::size_t end);

/* Descend into the object value of @p key. On success sets
 * [@p objStart, @p objEnd) to the nested object's content. */
bool findObject(const std::string& s, const std::string& key,
                std::size_t from, std::size_t end,
                std::size_t& objStart, std::size_t& objEnd);

/* "key":"value" -- copy unescaped string into @p out. */
bool extractString(const std::string& s, const std::string& key,
                   std::string& out,
                   std::size_t from = 0U,
                   std::size_t end  = std::string::npos);

/* "key":true / false. */
bool extractBool(const std::string& s, const std::string& key,
                 bool& out,
                 std::size_t from = 0U,
                 std::size_t end  = std::string::npos);

/* "key":123 / -123. */
bool extractInt(const std::string& s, const std::string& key,
                int& out,
                std::size_t from = 0U,
                std::size_t end  = std::string::npos);

/* "key":["first", ...] -- read the first array element if it is a string. */
bool extractStringArrayFirst(const std::string& s, const std::string& key,
                             std::string& out,
                             std::size_t from = 0U,
                             std::size_t end  = std::string::npos);

/* Iterate top-level objects in a JSON array. @p fn is called as
 * fn(elemStart, elemEnd), the slice covering the element incl. braces.
 * Returns false on malformed input. */
template <typename Fn>
bool forEachArrayObject(const std::string& s, Fn fn)
{
    std::size_t i = skipWs(s, 0U, s.size());
    if (i >= s.size() || s[i] != '[') return false;
    ++i;

    while (true)
    {
        i = skipWs(s, i, s.size());
        if (i >= s.size()) return false;
        if (s[i] == ']') return true;
        if (s[i] != '{') return false;

        const std::size_t elemStart = i;
        std::size_t depth = 0U;
        while (i < s.size())
        {
            const char c = s[i];
            if (c == '"')
            {
                const std::size_t after = skipJsonString(s, i, s.size());
                if (after == std::string::npos) return false;
                i = after;
                continue;
            }
            if (c == '{') ++depth;
            else if (c == '}')
            {
                --depth;
                if (depth == 0U) { ++i; break; }
            }
            ++i;
        }
        const std::size_t elemEnd = i;
        fn(elemStart, elemEnd);

        i = skipWs(s, i, s.size());
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { return true; }
        return false;
    }
}

}

#endif  /* PCM_JSON_EXTRACT_H */
