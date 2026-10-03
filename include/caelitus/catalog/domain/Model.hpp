#pragma once

/// @file
/// The catalog's entities and read models.
/// @ingroup catalog
///
/// Entities are what services create, change and store; read models are what
/// queries return (joined, with derived values such as the average rating).
/// All are plain data: the rules live in rules:: and in the services.

#include "caelitus/catalog/domain/Ids.hpp"
#include "caelitus/core/DateTime.hpp"

#include <optional>
#include <string>
#include <vector>

namespace caelitus::catalog {

// ---- Entities (what services create, change and store) ----------------------

/// A book category. Flat: no sub-categories; each book has exactly one.
struct Category {
    CategoryId id;     ///< Database id.
    std::string name;  ///< Display name; unique.
    std::string slug;  ///< URL-friendly name; unique, `[a-z0-9-]`.
};

/// A book author.
struct Author {
    AuthorId id;                     ///< Database id.
    std::string name;                ///< Full name; not unique (two authors may share a name).
    std::optional<std::string> bio;  ///< Free text.
    std::optional<Date> birthDate;   ///< Not in the future.
    int version = 1;                 ///< Optimistic locking: incremented on every update.
    Timestamp createdAt;             ///< Set on insert.
    Timestamp updatedAt;             ///< Set on every update.
};

/// A free-form label on books, created on first use.
struct Tag {
    TagId id;          ///< Database id.
    std::string name;  ///< Normalized: trimmed, single spaces, ASCII lower case; unique.
};

/// A book, as stored.
struct Book {
    BookId id;                               ///< Database id.
    std::string title;                       ///< Required.
    std::optional<std::string> isbn;         ///< ISBN-13, digits only (ISBN-10 input is converted); unique.
    std::optional<std::string> description;  ///< Free text.
    Date publishedOn{1970, 1, 1};            ///< Publication date.
    std::string language;                    ///< ISO 639-1 code, lower case ("el", "en").
    std::optional<int> pageCount;            ///< Positive.
    CategoryId categoryId;                   ///< Its one category.
    std::vector<AuthorId> authorIds;         ///< At least one, in cover order.
    std::vector<TagId> tagIds;               ///< Any number.
    int ratingCount = 0;                     ///< Number of reviews; maintained together with them.
    int ratingSum = 0;                       ///< Sum of review ratings; average = ratingSum / ratingCount.
    /// Whether likes/dislikes are counted. Off by default; changed with its
    /// own call and not versioned, so toggling it never conflicts with an edit.
    bool reactionsEnabled = false;
    int version = 1;      ///< Optimistic locking: incremented on every update.
    Timestamp createdAt;  ///< Set on insert.
    Timestamp updatedAt;  ///< Set on every update.
};

/// A reader's review of a book. Reviewers are names, not user accounts.
struct Review {
    ReviewId id;                       ///< Database id.
    BookId bookId;                     ///< The reviewed book.
    std::string reviewerName;          ///< Free text, required.
    int rating = 0;                    ///< 1..5.
    std::optional<std::string> title;  ///< Optional headline.
    std::string body;                  ///< The review text, required.
    Timestamp createdAt;               ///< Set on insert.
    Timestamp updatedAt;               ///< Set on every update.
};

// ---- Read models (what queries return) ---------------------------------------

/// An author as listed on a book.
struct AuthorRef {
    AuthorId id;       ///< Database id.
    std::string name;  ///< Full name.
};

/// A tag with the number of books carrying it.
struct TagUsage {
    Tag tag;                     ///< The tag.
    std::int64_t bookCount = 0;  ///< Books that have it.
};

/// One entry of a book list: everything needed to render it, nothing heavy.
struct BookSummary {
    BookId id;                            ///< Database id.
    std::string title;                    ///< Title.
    Date publishedOn{1970, 1, 1};         ///< Publication date.
    std::string language;                 ///< ISO 639-1 code.
    Category category;                    ///< Its category, in full.
    std::vector<AuthorRef> authors;       ///< In cover order.
    std::vector<std::string> tags;        ///< Tag names, alphabetical.
    int ratingCount = 0;                  ///< Number of reviews.
    std::optional<double> ratingAverage;  ///< 1.0..5.0; none without reviews.
    std::int64_t likes = 0;               ///< All time.
    std::int64_t dislikes = 0;            ///< All time.
    bool reactionsEnabled = false;        ///< Whether likes/dislikes are counted.
};

/// The few fields of a book kept in memory (BookCache) for hot paths.
struct BookBrief {
    BookId id;                      ///< Database id.
    std::string title;              ///< Title.
    bool reactionsEnabled = false;  ///< Whether likes/dislikes are counted.
};

/// A single book with everything about it.
struct BookDetails : BookSummary {
    std::optional<std::string> isbn;         ///< ISBN-13.
    std::optional<std::string> description;  ///< Free text.
    std::optional<int> pageCount;            ///< Number of pages.
    int version = 1;                         ///< Pass back to BookService::update().
    Timestamp createdAt;                     ///< When it was added.
    Timestamp updatedAt;                     ///< Last change.
};

/// sum / count, or std::nullopt when there are no ratings.
inline std::optional<double> averageRating(int count, int sum) {
    if (count <= 0) return std::nullopt;
    return static_cast<double>(sum) / count;
}

}  // namespace caelitus::catalog
