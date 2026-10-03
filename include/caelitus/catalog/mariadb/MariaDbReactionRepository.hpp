#pragma once

/// @file
/// Like/dislike counts in MariaDB.
/// @ingroup catalog_mariadb

#include "caelitus/catalog/domain/Reactions.hpp"
#include "caelitus/db/SqlExecutor.hpp"

#include <memory>

namespace caelitus::catalog::mariadb {

/// IReactionRepository on MariaDB.
///
/// Daily counts live in `book_reactions_daily` (one row per book and day,
/// upserted with INSERT ... ON DUPLICATE KEY UPDATE); all-time totals in
/// `books.likes` / `books.dislikes`, with a generated, indexed
/// `reaction_score` column so the all-time ranking needs no aggregation.
class MariaDbReactionRepository final : public IReactionRepository {
public:
    /// @param sql  Statement executor; runs in the caller's transaction.
    explicit MariaDbReactionRepository(std::shared_ptr<db::SqlExecutor> sql) : sql_(std::move(sql)) {}

    std::int64_t add(const std::vector<DailyReactions>& deltas) override;
    std::vector<ReactionCounts> counts(BookId book, const std::vector<std::optional<DateRange>>& ranges) override;
    std::vector<RankedBook> top(const std::optional<DateRange>& range, ReactionOrder order, int limit) override;

private:
    std::shared_ptr<db::SqlExecutor> sql_;
};

}  // namespace caelitus::catalog::mariadb
