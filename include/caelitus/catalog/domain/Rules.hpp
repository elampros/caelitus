#pragma once

/// @file
/// Input normalization and validation shared by the catalog services.
/// @ingroup catalog

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

/// Input normalization and validation shared by the catalog services.
///
/// Every function throws ValidationError naming the field on bad input, and
/// returns the normalized value otherwise.
namespace caelitus::catalog::rules {

/// @name Limits
/// In characters (UTF-8 code points), not bytes; they match the column sizes.
/// @{
constexpr std::size_t kNameMax = 200;           ///< Author name.
constexpr std::size_t kCategoryNameMax = 100;   ///< Category name.
constexpr std::size_t kSlugMax = 100;           ///< Category slug.
constexpr std::size_t kTitleMax = 300;          ///< Book title.
constexpr std::size_t kReviewerMax = 100;       ///< Reviewer name.
constexpr std::size_t kReviewTitleMax = 200;    ///< Review headline.
constexpr std::size_t kTextMax = 20000;         ///< Descriptions, bios, review bodies.
constexpr std::size_t kTagMax = 50;             ///< One tag.
constexpr std::size_t kMaxTagsPerBook = 20;     ///< Tags on one book.
constexpr std::size_t kMaxAuthorsPerBook = 20;  ///< Authors of one book.
constexpr std::size_t kSearchTextMax = 100;     ///< Search strings ("titleContains", ...).
/// @}

/// Number of UTF-8 code points.
std::size_t charCount(std::string_view text);

/// Without leading and trailing ASCII whitespace.
std::string trim(std::string_view text);

/// Trimmed; must be non-empty and at most `maxChars` long.
/// @throws ValidationError for `field` otherwise.
std::string requiredText(const std::string& field, std::string_view value, std::size_t maxChars);

/// Trimmed; empty becomes std::nullopt; at most `maxChars` long.
/// @throws ValidationError for `field` if too long.
std::optional<std::string> optionalText(const std::string& field, const std::optional<std::string>& value,
                                        std::size_t maxChars);

/// Accepts ISBN-10 or ISBN-13, with or without hyphens/spaces, verifies the
/// check digit and returns the ISBN-13 digits ("9780306406157").
/// @throws ValidationError for "isbn".
std::string normalizeIsbn(std::string_view isbn);

/// A slug from a name: ASCII letters/digits lower-cased, everything else
/// collapsed to single hyphens ("Science Fiction" -> "science-fiction").
/// Empty if the name has no ASCII letters/digits.
std::string slugFromName(std::string_view name);

/// Validates an explicit slug: 1..kSlugMax of `[a-z0-9-]`, no leading,
/// trailing or double hyphens.
/// @throws ValidationError for "slug".
std::string validateSlug(std::string_view slug);

/// Trimmed, inner whitespace collapsed to one space, ASCII lower-cased.
/// (Non-ASCII case differences are folded by the database collation.)
/// @throws ValidationError for "tags" if empty or longer than kTagMax.
std::string normalizeTag(std::string_view tag);

/// ISO 639-1: two ASCII letters, returned lower-case ("EL" -> "el").
/// @throws ValidationError for "language".
std::string normalizeLanguage(std::string_view language);

/// 1..5, returned unchanged.
/// @throws ValidationError for "rating".
int validateRating(int rating);

}  // namespace caelitus::catalog::rules
