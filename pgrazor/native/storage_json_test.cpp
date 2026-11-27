// Pure regression for the Pokemon storage JSON contract; no game runtime needed.
#include "storage_json.h"
#include <cassert>
#include <cstdio>

int main() {
    std::string a; pgr_storage_json_string(a, "Sparky");
    assert(a == "\"Sparky\"");

    std::string b; pgr_storage_json_string(b, "a\"b\\c\nd\te\rf");
    assert(b == "\"a\\\"b\\\\c\\nd\\te\\rf\"");

    std::string c; pgr_storage_json_string(c, std::string("\x01", 1));
    assert(c == "\"\\u0001\"");

    std::string d; pgr_storage_json_string(d, "");
    assert(d == "\"\"");

    std::string e; pgr_storage_json_string(e, "Pok\u00e9mon"); // UTF-8 passes through
    assert(e.find("Pok") == 1 && e.size() > 6);

    std::puts("PASS storage JSON: strings, quotes, backslash, control chars, empty and UTF-8");
    return 0;
}
