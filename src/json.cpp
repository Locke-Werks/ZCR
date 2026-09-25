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
#include "json.h"

#include <cerrno>
#include <cwchar>
#include <cwctype>

namespace zcr::json {
namespace {

class Scanner {
public:
    explicit Scanner(std::wstring_view text) : text_(text)
    {
        // A UTF-8 BOM decodes to U+FEFF, and File.ReadAllText used to swallow it
        // silently. Anyone who has opened zcr.json in Notepad has one,
        // so failing on it here would look exactly like "the C++ version broke my
        // config".
        if (!text_.empty() && text_.front() == 0xFEFF) {
            pos_ = 1;
        }
    }

    [[nodiscard]] bool AtEnd() const { return pos_ >= text_.size(); }

    [[nodiscard]] wchar_t Peek() const { return AtEnd() ? L'\0' : text_[pos_]; }

    [[nodiscard]] wchar_t PeekAt(size_t ahead) const
    {
        const size_t at = pos_ + ahead;
        return at >= text_.size() ? L'\0' : text_[at];
    }

    wchar_t Take()
    {
        if (AtEnd()) {
            return L'\0';
        }
        const wchar_t c = text_[pos_++];
        if (c == L'\n') {
            ++line_;
        }
        return c;
    }

    bool TakeIf(wchar_t expected)
    {
        if (Peek() != expected) {
            return false;
        }
        Take();
        return true;
    }

    /// Whitespace, `//` line comments and `/* */` block comments.
    void SkipTrivia()
    {
        for (;;) {
            while (!AtEnd() && (Peek() == L' ' || Peek() == L'\t'
                                || Peek() == L'\r' || Peek() == L'\n')) {
                Take();
            }

            if (Peek() == L'/' && PeekAt(1) == L'/') {
                while (!AtEnd() && Peek() != L'\n') {
                    Take();
                }
                continue;
            }

            if (Peek() == L'/' && PeekAt(1) == L'*') {
                Take();
                Take();
                while (!AtEnd() && !(Peek() == L'*' && PeekAt(1) == L'/')) {
                    Take();
                }
                // An unterminated block comment runs to end of file rather than
                // failing. The value it swallowed falls back to its default,
                // which beats rejecting the whole file over a missing `*/`.
                if (!AtEnd()) {
                    Take();
                    Take();
                }
                continue;
            }

            return;
        }
    }

    [[nodiscard]] size_t Line() const { return line_; }

private:
    std::wstring_view text_;
    size_t pos_ = 0;
    size_t line_ = 1;
};

std::wstring Fail(Scanner& scanner, std::wstring_view what)
{
    return L"line " + std::to_wstring(scanner.Line()) + L": " + std::wstring(what);
}

bool ParseString(Scanner& scanner, std::wstring& out, std::wstring& error)
{
    if (!scanner.TakeIf(L'"')) {
        error = Fail(scanner, L"expected a quoted string");
        return false;
    }

    out.clear();
    for (;;) {
        if (scanner.AtEnd()) {
            error = Fail(scanner, L"string is missing its closing quote");
            return false;
        }

        const wchar_t c = scanner.Take();
        if (c == L'"') {
            return true;
        }

        if (c != L'\\') {
            out.push_back(c);
            continue;
        }

        const wchar_t escape = scanner.Take();
        switch (escape) {
        case L'"':  out.push_back(L'"');  break;
        case L'\\': out.push_back(L'\\'); break;
        case L'/':  out.push_back(L'/');  break;
        case L'b':  out.push_back(L'\b'); break;
        case L'f':  out.push_back(L'\f'); break;
        case L'n':  out.push_back(L'\n'); break;
        case L'r':  out.push_back(L'\r'); break;
        case L't':  out.push_back(L'\t'); break;
        case L'u': {
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
                const wchar_t hex = scanner.Take();
                unsigned digit = 0;
                if (hex >= L'0' && hex <= L'9') {
                    digit = static_cast<unsigned>(hex - L'0');
                } else if (hex >= L'a' && hex <= L'f') {
                    digit = static_cast<unsigned>(hex - L'a') + 10;
                } else if (hex >= L'A' && hex <= L'F') {
                    digit = static_cast<unsigned>(hex - L'A') + 10;
                } else {
                    error = Fail(scanner, L"\\u needs four hex digits");
                    return false;
                }
                code = (code << 4) | digit;
            }
            // Already UTF-16, so a \uXXXX is one code unit and a surrogate pair
            // written as two escapes lands correctly with no special handling.
            out.push_back(static_cast<wchar_t>(code));
            break;
        }
        default:
            error = Fail(scanner, L"unknown escape sequence");
            return false;
        }
    }
}

bool ParseNumber(Scanner& scanner, Value& out, std::wstring& error)
{
    std::wstring text;
    while (!scanner.AtEnd()) {
        const wchar_t c = scanner.Peek();
        const bool numeric = (c >= L'0' && c <= L'9') || c == L'-' || c == L'+'
                             || c == L'.' || c == L'e' || c == L'E';
        if (!numeric) {
            break;
        }
        text.push_back(scanner.Take());
    }

    if (text.empty()) {
        error = Fail(scanner, L"expected a number");
        return false;
    }

    // Every numeric key here is a pixel count, so parse as an integer and
    // truncate a fractional value rather than carrying a floating-point path
    // that nothing in the config would use.
    wchar_t* end = nullptr;
    errno = 0;
    const double value = wcstod(text.c_str(), &end);
    if (end == text.c_str() || *end != L'\0') {
        error = Fail(scanner, L"could not read the number '" + text + L"'");
        return false;
    }

    // Casting a double to long long is undefined when the value does not fit,
    // and "1e999" is four characters a person could plausibly type. Reject it
    // rather than letting the cast produce whatever the hardware felt like.
    if (errno == ERANGE || !(value >= -9.2e18 && value <= 9.2e18)) {
        error = Fail(scanner, L"the number '" + text + L"' is out of range");
        return false;
    }

    out.type = Value::Type::Number;
    out.number = static_cast<long long>(value);
    return true;
}

bool ParseValue(Scanner& scanner, Value& out, std::wstring& error, int depth,
                const ParseOptions& options);
bool ParseObjectBody(Scanner& scanner, Object& out, std::wstring& error, int depth,
                     const ParseOptions& options);

/// ZCR's config is one flat object, so nesting deeper than this is malformed by
/// definition there. The limit is not a style choice: the parse functions are mutually
/// recursive, so a file of "{\"a\":[[[[[..." would recurse once per bracket and
/// overflow the stack, which is a crash on a file the user can edit.
constexpr int kMaxDepthFlat = 16;
constexpr int kMaxDepthNested = 32;

bool TooDeep(Scanner& scanner, int depth, const ParseOptions& options, std::wstring& error)
{
    if (depth < (options.allow_nesting ? kMaxDepthNested : kMaxDepthFlat)) {
        return false;
    }
    error = Fail(scanner, options.allow_nesting
                              ? L"nested too deeply"
                              : L"nested too deeply for this config's schema");
    return true;
}

bool ParseArray(Scanner& scanner, Value& out, std::wstring& error, int depth,
                const ParseOptions& options)
{
    if (TooDeep(scanner, depth, options, error)) {
        return false;
    }

    scanner.Take(); // '['
    out.type = Value::Type::Array;
    out.array.clear();

    for (;;) {
        scanner.SkipTrivia();
        if (scanner.TakeIf(L']')) {
            return true; // also the trailing-comma exit
        }

        Value element;
        if (!ParseValue(scanner, element, error, depth + 1, options)) {
            return false;
        }

        // Only arrays of strings exist in ZCR's config. Anything else there
        // is a typo worth naming rather than silently dropping.
        if (!options.allow_nesting && element.type != Value::Type::String) {
            error = Fail(scanner, L"arrays in this file may only contain strings");
            return false;
        }
        out.array.push_back(std::move(element));

        scanner.SkipTrivia();
        if (scanner.TakeIf(L',')) {
            continue;
        }
        if (scanner.TakeIf(L']')) {
            return true;
        }

        error = Fail(scanner, L"expected ',' or ']' in an array");
        return false;
    }
}

bool ParseValue(Scanner& scanner, Value& out, std::wstring& error, int depth,
                const ParseOptions& options)
{
    if (TooDeep(scanner, depth, options, error)) {
        return false;
    }

    scanner.SkipTrivia();
    const wchar_t c = scanner.Peek();

    if (c == L'"') {
        out.type = Value::Type::String;
        return ParseString(scanner, out.string, error);
    }

    if (c == L'[') {
        return ParseArray(scanner, out, error, depth + 1, options);
    }

    if (c == L't' || c == L'f' || c == L'n') {
        std::wstring word;
        while (!scanner.AtEnd() && iswalpha(scanner.Peek())) {
            word.push_back(scanner.Take());
        }
        if (word == L"true" || word == L"false") {
            out.type = Value::Type::Bool;
            out.boolean = (word == L"true");
            return true;
        }
        if (word == L"null") {
            out.type = Value::Type::Null;
            return true;
        }
        error = Fail(scanner, L"expected true, false or null but found '" + word + L"'");
        return false;
    }

    if (c == L'{') {
        if (!options.allow_nesting) {
            error = Fail(scanner, L"nested objects are not part of this config's schema");
            return false;
        }
        scanner.Take(); // '{'
        out.type = Value::Type::Object;
        return ParseObjectBody(scanner, out.object, error, depth + 1, options);
    }

    return ParseNumber(scanner, out, error);
}

/// Everything after an object's opening brace, through its closing one.
bool ParseObjectBody(Scanner& scanner, Object& out, std::wstring& error, int depth,
                     const ParseOptions& options)
{
    for (;;) {
        scanner.SkipTrivia();
        if (scanner.TakeIf(L'}')) {
            return true; // also the trailing-comma exit
        }

        if (scanner.AtEnd()) {
            error = Fail(scanner, L"the file ended before its closing '}'");
            return false;
        }

        std::wstring key;
        if (!ParseString(scanner, key, error)) {
            return false;
        }

        scanner.SkipTrivia();
        if (!scanner.TakeIf(L':')) {
            error = Fail(scanner, L"expected ':' after the key '" + key + L"'");
            return false;
        }

        Value value;
        if (!ParseValue(scanner, value, error, depth, options)) {
            return false;
        }
        out.emplace_back(std::move(key), std::move(value));

        scanner.SkipTrivia();
        if (scanner.TakeIf(L',')) {
            continue;
        }
        if (scanner.TakeIf(L'}')) {
            return true;
        }

        error = Fail(scanner, L"expected ',' or '}' after a value");
        return false;
    }
}

} // namespace

bool ParseObject(std::wstring_view text, Object& out, std::wstring& error,
                 ParseOptions options)
{
    out.clear();
    error.clear();

    Scanner scanner(text);
    scanner.SkipTrivia();

    if (!scanner.TakeIf(L'{')) {
        error = Fail(scanner, L"the file must start with '{'");
        return false;
    }

    return ParseObjectBody(scanner, out, error, 1, options);
}

const Value* Find(const Object& object, std::wstring_view key)
{
    for (const auto& [name, value] : object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

const Value* Find(const Value& value, std::wstring_view key)
{
    if (value.type != Value::Type::Object) {
        return nullptr;
    }
    return Find(value.object, key);
}

std::wstring Quote(std::wstring_view value)
{
    std::wstring out;
    out.reserve(value.size() + 2);
    out.push_back(L'"');

    for (const wchar_t c : value) {
        switch (c) {
        case L'"':  out.append(L"\\\""); break;
        case L'\\': out.append(L"\\\\"); break;
        case L'\b': out.append(L"\\b");  break;
        case L'\f': out.append(L"\\f");  break;
        case L'\n': out.append(L"\\n");  break;
        case L'\r': out.append(L"\\r");  break;
        case L'\t': out.append(L"\\t");  break;
        default:
            if (c < 0x20) {
                wchar_t escape[7];
                swprintf_s(escape, L"\\u%04X", static_cast<unsigned>(c));
                out.append(escape);
            } else {
                out.push_back(c);
            }
            break;
        }
    }

    out.push_back(L'"');
    return out;
}

} // namespace zcr::json
