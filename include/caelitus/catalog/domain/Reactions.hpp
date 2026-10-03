#pragma once

/// @file
/// Likes and dislikes: types and the repository interface.
/// @ingroup catalog

#include "caelitus/catalog/domain/Ids.hpp"
#include "caelitus/core/DateTime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace caelitus::catalog {

/// A reader's reaction to a book.
enum class Reaction {
    Like,    ///< Received on "<prefix>/<id>/like".
    Dislike  ///< Received on "<prefix>/<id>/dislike".
};

/// Rolling periods ending today, in the catalog's time zone (default
/// Europe/Athens). "Last 7 days" is today and the 6 days before it.
enum class Period {
    Today,       ///< Today only.
    Yesterday,   ///< Yesterday only.
    Last7Days,   ///< Today and the previous 6 days.
    Last30Days,  ///< Today and the previous 29 days.
    LastYear,    ///< Today and the previous 364 days.
    AllTime      ///< Everything.
};

/// Ranking direction for IReactionRepository::top().
enum class ReactionOrder {
    MostLiked,     ///< Books with at least one like, highest likes - dislikes first.
    MostDisliked,  ///< Books with at least one dislike, lowest likes - dislikes first.
};

/// Likes and dislikes of one book in some period.
struct ReactionCounts {
    std::int64_t likes = 0;     ///< Number of likes.
    std::int64_t dislikes = 0;  ///< Number of dislikes.
    /// What rankings sort by.
    std::int64_t score() const { return likes - dislikes; }
};

/// An inclusive range of local calendar days.
struct DateRange {
    Date from{1970, 1, 1};  ///< First day (inclusive).
    Date to{1970, 1, 1};    ///< Last day (inclusive).
};

/// Reactions to add for one book on one local day.
struct DailyReactions {
    BookId book;                ///< The book.
    Date day{1970, 1, 1};       ///< The local day they arrived on.
    std::int64_t likes = 0;     ///< Likes to add.
    std::int64_t dislikes = 0;  ///< Dislikes to add.
};

/// An entry of a ranking.
struct RankedBook {
    BookId id;              ///< The book.
    std::string title;      ///< Its title.
    ReactionCounts counts;  ///< Its counts in the ranked period.
};

/// Storage of likes and dislikes: per-day counts plus all-time totals on the book.
class IReactionRepository {
public:
    virtual ~IReactionRepository() = default;

    /// Adds the deltas to the daily counts and to the books' all-time totals.
    ///
    /// Deltas for books that do not exist (deleted meanwhile) are skipped.
    /// @return How many reactions (likes + dislikes) were skipped that way.
    virtual std::int64_t add(const std::vector<DailyReactions>& deltas) = 0;

    /// One book's counts within each range.
    /// @param book    The book.
    /// @param ranges  std::nullopt stands for all time.
    /// @return One entry per range, in the same order.
    virtual std::vector<ReactionCounts> counts(BookId book, const std::vector<std::optional<DateRange>>& ranges) = 0;

    /// Books with reactions in the range (std::nullopt: all time), best or
    /// worst first; ties go to the book with more likes (or dislikes), then
    /// the lower id.
    /// @param range  The days to count; std::nullopt for all time.
    /// @param order  Best or worst first.
    /// @param limit  Most entries to return.
    virtual std::vector<RankedBook> top(const std::optional<DateRange>& range, ReactionOrder order, int limit) = 0;
};

}  // namespace caelitus::catalog
