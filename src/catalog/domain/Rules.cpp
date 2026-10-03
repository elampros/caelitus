/// @file
/// The catalog's normalization and validation rules (names, slugs, ISBNs,
/// languages, tags, ratings, paging).
/// @ingroup catalog

#include "caelitus/catalog/domain/Rules.hpp"

#include "caelitus/core/DomainErrors.hpp"

#include <cctype>

namespace caelitus::catalog::rules {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool isAsciiAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

void checkLength(const std::string& field, const std::string& value, std::size_t maxChars) {
    if (charCount(value) > maxChars)
        throw ValidationError(field, "must be at most " + std::to_string(maxChars) + " characters");
}

}  // namespace

std::size_t charCount(std::string_view text) {
    std::size_t n = 0;
    for (char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;  // count non-continuation bytes
    return n;
}

std::string trim(std::string_view text) {
    std::size_t b = 0, e = text.size();
    while (b < e && isSpace(text[b])) ++b;
    while (e > b && isSpace(text[e - 1])) --e;
    return std::string(text.substr(b, e - b));
}

std::string requiredText(const std::string& field, std::string_view value, std::size_t maxChars) {
    std::string t = trim(value);
    if (t.empty()) throw ValidationError(field, "is required");
    checkLength(field, t, maxChars);
    return t;
}

std::optional<std::string> optionalText(const std::string& field, const std::optional<std::string>& value,
                                        std::size_t maxChars) {
    if (!value) return std::nullopt;
    std::string t = trim(*value);
    if (t.empty()) return std::nullopt;
    checkLength(field, t, maxChars);
    return t;
}

std::string normalizeIsbn(std::string_view isbn) {
    std::string digits;
    for (char c : isbn) {
        if (c == '-' || c == ' ') continue;
        digits.push_back(c == 'x' ? 'X' : c);
    }
    auto allDigits = [](std::string_view s) {
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    };
    auto isbn13Check = [](std::string_view first12) {
        int sum = 0;
        for (std::size_t i = 0; i < 12; ++i) sum += (first12[i] - '0') * (i % 2 == 0 ? 1 : 3);
        return static_cast<char>('0' + (10 - sum % 10) % 10);
    };

    if (digits.size() == 10) {
        if (!allDigits(std::string_view(digits).substr(0, 9)) ||
            !(std::isdigit(static_cast<unsigned char>(digits[9])) || digits[9] == 'X'))
            throw ValidationError("isbn", "ISBN-10 must be 9 digits followed by a digit or X");
        int sum = 0;
        for (std::size_t i = 0; i < 10; ++i)
            sum += static_cast<int>(10 - i) * (digits[i] == 'X' ? 10 : digits[i] - '0');
        if (sum % 11 != 0) throw ValidationError("isbn", "invalid ISBN-10 check digit");
        std::string isbn13 = "978" + digits.substr(0, 9);
        isbn13.push_back(isbn13Check(isbn13));
        return isbn13;
    }
    if (digits.size() == 13) {
        if (!allDigits(digits)) throw ValidationError("isbn", "ISBN-13 must contain only digits");
        if (digits.compare(0, 3, "978") != 0 && digits.compare(0, 3, "979") != 0)
            throw ValidationError("isbn", "ISBN-13 must start with 978 or 979");
        if (isbn13Check(digits) != digits[12]) throw ValidationError("isbn", "invalid ISBN-13 check digit");
        return digits;
    }
    throw ValidationError("isbn", "must have 10 or 13 digits");
}

std::string slugFromName(std::string_view name) {
    std::string slug;
    bool pendingHyphen = false;
    for (char c : name) {
        if (isAsciiAlnum(c)) {
            if (pendingHyphen && !slug.empty()) slug.push_back('-');
            pendingHyphen = false;
            slug.push_back(asciiLower(c));
        } else {
            pendingHyphen = true;
        }
    }
    if (slug.size() > kSlugMax) {
        slug.resize(kSlugMax);
        while (!slug.empty() && slug.back() == '-') slug.pop_back();
    }
    return slug;
}

std::string validateSlug(std::string_view slugIn) {
    const std::string slug = trim(slugIn);
    if (slug.empty()) throw ValidationError("slug", "is required");
    if (slug.size() > kSlugMax)
        throw ValidationError("slug", "must be at most " + std::to_string(kSlugMax) + " characters");
    for (char c : slug)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            throw ValidationError("slug", "may contain only a-z, 0-9 and '-'");
    if (slug.front() == '-' || slug.back() == '-' || slug.find("--") != std::string::npos)
        throw ValidationError("slug", "must not start or end with '-' or contain '--'");
    return slug;
}

std::string normalizeTag(std::string_view tagIn) {
    std::string tag;
    bool space = false;
    for (char c : trim(tagIn)) {
        if (isSpace(c)) {
            space = true;
            continue;
        }
        if (space) tag.push_back(' ');
        space = false;
        tag.push_back(asciiLower(c));
    }
    if (tag.empty()) throw ValidationError("tags", "a tag must not be empty");
    if (charCount(tag) > kTagMax)
        throw ValidationError("tags", "tag '" + tag + "' is longer than " + std::to_string(kTagMax) + " characters");
    return tag;
}

std::string normalizeLanguage(std::string_view languageIn) {
    const std::string lang = trim(languageIn);
    if (lang.size() != 2 || !std::isalpha(static_cast<unsigned char>(lang[0])) ||
        !std::isalpha(static_cast<unsigned char>(lang[1])))
        throw ValidationError("language", "must be a two-letter ISO 639-1 code such as 'el' or 'en'");
    return {asciiLower(lang[0]), asciiLower(lang[1])};
}

int validateRating(int rating) {
    if (rating < 1 || rating > 5) throw ValidationError("rating", "must be between 1 and 5");
    return rating;
}

}  // namespace caelitus::catalog::rules
