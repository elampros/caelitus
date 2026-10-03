#pragma once

/// @file
/// Paging and the book search query.
/// @ingroup catalog

#include "caelitus/catalog/domain/Ids.hpp"
#include "caelitus/core/DateTime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace caelitus::catalog {

/// Which page of a list to return.
struct Page {
    static constexpr int kMaxSize = 100;  ///< Largest allowed page size.

    int number = 1;  ///< 1-based.
    int size = 20;   ///< Items per page, 1..kMaxSize.

    /// Items to skip: (number - 1) * size.
    std::int64_t offset() const { return static_cast<std::int64_t>(number - 1) * size; }
};

/// One page of a list.
template <typename T>
struct Paged {
    std::vector<T> items;    ///< This page's items.
    std::int64_t total = 0;  ///< Items across all pages.
    Page page;               ///< The page that was requested.

    /// Number of pages: ceil(total / size).
    std::int64_t pageCount() const { return page.size > 0 ? (total + page.size - 1) / page.size : 0; }
};

/// How BookQuery::tags combine.
enum class TagMatch {
    Any,  ///< Books with at least one of the tags.
    All,  ///< Books with every tag.
};

/// Order of a book list. Ties are broken by id, so paging is stable.
enum class BookSort {
    PublishedDesc,  ///< Newest first (default).
    PublishedAsc,   ///< Oldest first.
    TitleAsc,       ///< Alphabetical.
    RatingDesc,     ///< Best rated first, unrated last.
    CreatedDesc,    ///< Most recently added first.
};

/// A book search. Filters combine with AND; unset filters do not restrict.
struct BookQuery {
    std::optional<CategoryId> category;        ///< In this category.
    std::optional<AuthorId> author;            ///< By this author (any position).
    std::vector<std::string> tags;             ///< Tag names; normalized by the service.
    TagMatch tagMatch = TagMatch::Any;         ///< Whether any or all `tags` must match.
    std::optional<Date> publishedFrom;         ///< Published on or after (inclusive).
    std::optional<Date> publishedTo;           ///< Published on or before (inclusive).
    std::optional<std::string> titleContains;  ///< Case-insensitive substring of the title.
    std::optional<double> minRating;           ///< Average rating >= this; unrated books excluded.
    std::optional<std::string> language;       ///< ISO 639-1 code.
    BookSort sort = BookSort::PublishedDesc;   ///< Order.
    Page page;                                 ///< Which page.
};

}  // namespace caelitus::catalog
