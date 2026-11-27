#pragma once
#include <cstdio>
#include <string>

// Escapes a plain-data value for the Pokemon storage JSON. Kept pure so the export contract can be
// tested without the game runtime; future plugins can reuse it for their own JSON lists.
inline void pgr_storage_json_string(std::string &out, const std::string &value) {
    out += '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += char(c);
    }
    out += '"';
}
