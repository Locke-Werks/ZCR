// ZCR, a zero-copy HDR screen recorder for Windows on NVIDIA hardware.
// Copyright (C) 2026 Locke Werks
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zcr::json {

/// One JSON value.
///
/// ZCR's config is a flat object, and by default that is all the parser
/// accepts. Nesting can be switched on per parse, and a value then carries its
/// children here. Plain members rather than a variant: a
/// recursive type has to be incomplete inside itself, and std::vector of an
/// incomplete type is the one container guaranteed to allow that.
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    long long number = 0;
    std::wstring string;
    std::vector<Value> array;
    std::vector<std::pair<std::wstring, Value>> object;
};

/// Key/value pairs in file order. A vector rather than a map because the file is
/// ten keys long and round-tripping wants the original order anyway.
using Object = std::vector<std::pair<std::wstring, Value>>;

struct ParseOptions {
    /// Accept objects inside values and non-string array elements. Off for
    /// ZCR's config, whose schema has neither, so a typo there is still
    /// named rather than silently accepted.
    bool allow_nesting = false;
};

/// Parses a JSON object.
///
/// Deliberately more forgiving than RFC 8259: `//` and block comments are
/// skipped and a trailing comma before the closing brace or bracket is allowed,
/// because the config is edited by hand. nlohmann/json is strict and offers no
/// trailing-comma option, which is why this is hand-written.
///
/// Returns false and fills `error` with a human-readable message including a
/// line number. The caller is expected to report that and carry on with
/// defaults rather than overwrite the file, because the file is the only copy of
/// something a person typed.
bool ParseObject(std::wstring_view text, Object& out, std::wstring& error,
                 ParseOptions options = {});

/// Finds a key, case-sensitively. Null when absent.
[[nodiscard]] const Value* Find(const Object& object, std::wstring_view key);

/// The same lookup on a nested value. Null when absent, or when `value` is not
/// an object at all.
[[nodiscard]] const Value* Find(const Value& value, std::wstring_view key);

/// Escapes and quotes a string for output. Escapes the two characters JSON
/// requires plus the control range; notably `\` is escaped, which is what makes
/// a monitor device name such as \\.\DISPLAY2 survive a round trip.
[[nodiscard]] std::wstring Quote(std::wstring_view value);

} // namespace zcr::json
