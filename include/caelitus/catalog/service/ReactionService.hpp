#pragma once

/// @file
/// Use cases for likes and dislikes.
/// @ingroup catalog

#include "caelitus/catalog/domain/Reactions.hpp"
#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/catalog/service/BookCache.hpp"
#include "caelitus/catalog/service/ServiceSupport.hpp"
#include "caelitus/core/TimeZone.hpp"
#include "caelitus/db/ITransactionManager.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace caelitus::catalog {

/// The Period named by its API text ("today", "yesterday", "last7Days",
/// "last30Days", "lastYear", "allTime"), or std::nullopt.
std::optional<Period> parsePeriod(const std::string& text);
/// The API text of a Period; the inverse of parsePeriod().
const char* toString(Period period);

/// One book's counts for every period.
struct ReactionStats {
    BookId book;                                             ///< The book.
    std::vector<std::pair<Period, ReactionCounts>> periods;  ///< Every Period, in enum order.
};

/// Use cases for likes and dislikes.
///
/// record() only counts in memory (cheap, thread-safe, called for every
/// incoming MQTT message); flush() writes the accumulated counts in one
/// transaction and is called periodically (config
/// "catalog.reactions.flushIntervalMs", default 1 s) and at shutdown. Results
/// therefore lag by up to one flush interval, and a burst of 1000 likes costs
/// one small transaction instead of 1000.
///
/// Each reaction is counted on the day it was recorded, in the catalog's time
/// zone, even if it is flushed after midnight. Only books whose
/// reactionsEnabled switch is on accept reactions; the check uses the
/// in-memory BookCache, never the database.
///
/// If the database is down, flush() keeps the counts for the next attempt.
/// The buffer is bounded by `maxBufferedEntries` (book-day pairs); once full,
/// reactions for new book-days are dropped and logged, while those for
/// book-days already buffered are still counted.
///
/// Logs to "catalog.reactions".
class ReactionService {
public:
    /// Outcome of one flush().
    struct FlushResult {
        std::int64_t written = 0;  ///< Reactions stored.
        std::int64_t skipped = 0;  ///< Reactions for books that no longer exist.
        bool failed = false;       ///< Database error: the counts are kept for the next flush.
    };

    /// @param reactions           Storage of the counts.
    /// @param books               To check that a book exists (stats()).
    /// @param cache               Answers "does this book accept reactions?".
    /// @param tx                  Groups the writes of a flush.
    /// @param timeZone            Decides which day a reaction belongs to.
    /// @param maxBufferedEntries  Book-day pairs kept in memory between flushes.
    /// @param clock               Source of "now".
    /// @throws std::invalid_argument if a dependency is null.
    ReactionService(std::shared_ptr<IReactionRepository> reactions, std::shared_ptr<IBookRepository> books,
                    std::shared_ptr<BookCache> cache, std::shared_ptr<db::ITransactionManager> tx, TimeZone timeZone,
                    std::size_t maxBufferedEntries = 100000, Clock clock = systemClock());

    /// Counts one reaction in memory.
    /// @return false (and nothing counted) if the book is unknown, does not
    ///         accept reactions, or the buffer is full.
    /// @throws ValidationError for a non-positive id.
    bool record(BookId book, Reaction reaction);

    /// Writes the buffered counts in one transaction. Thread-safe; one flush
    /// runs at a time.
    FlushResult flush();

    /// A book's counts for every Period (stored counts only: not yet flushed
    /// reactions are not included).
    /// @throws NotFoundError
    ReactionStats stats(BookId book);

    /// Ranking of books in a period.
    /// @param period  Which days count.
    /// @param order   Best or worst first.
    /// @param limit  1..Page::kMaxSize.
    /// @throws ValidationError for a bad limit.
    std::vector<RankedBook> top(Period period, ReactionOrder order, int limit);

    /// The days a period covers, ending today; std::nullopt for Period::AllTime.
    std::optional<DateRange> range(Period period) const;

    /// Entries (book-day pairs) waiting for the next flush.
    std::size_t pending() const;

private:
    struct Key {
        std::int64_t book;
        std::int64_t day;
        bool operator==(const Key& o) const { return book == o.book && day == o.day; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return std::hash<std::int64_t>{}(k.book) * 31 + std::hash<std::int64_t>{}(k.day);
        }
    };
    using Buffer = std::unordered_map<Key, ReactionCounts, KeyHash>;

    void restore(const Buffer& unwritten);

    std::shared_ptr<IReactionRepository> reactions_;
    std::shared_ptr<IBookRepository> books_;
    std::shared_ptr<BookCache> cache_;
    std::shared_ptr<db::ITransactionManager> tx_;
    TimeZone timeZone_;
    std::size_t maxBuffered_;
    Clock clock_;
    log::Logger log_;

    mutable std::mutex mutex_;
    Buffer buffer_;
    std::mutex flushMutex_;  // one flush at a time
    std::atomic<std::int64_t> dropped_{0};
    log::LogThrottle droppedLog_, skippedLog_, failedLog_;
};

}  // namespace caelitus::catalog
