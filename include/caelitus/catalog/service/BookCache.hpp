#pragma once

/// @file
/// Every book's brief in memory.
/// @ingroup catalog

#include "caelitus/cache/LocalCache.hpp"
#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/log/Log.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace caelitus::catalog {

/// Every book's BookBrief (id, title, reactionsEnabled) in memory, so hot
/// paths such as "does book 42 accept likes?" never touch the database.
///
/// Kept current by:
/// - reload(): the whole table, at startup and periodically as a safety net
///   (e.g. after manual database edits; config "catalog.bookCacheReloadSec");
/// - refresh() / forget(): called by BookService after each commit.
///
/// A refresh that happens while a reload is running is re-applied after it,
/// so the reload's older snapshot never wins. Assumes a single server instance:
/// changes made by another instance only show up at the next reload.
///
/// Thread-safe. Logs to "cache.books".
class BookCache {
public:
    /// @param books  Where the briefs are read from.
    explicit BookCache(std::shared_ptr<IBookRepository> books);

    /// Replaces the whole content with a fresh read of every book.
    /// @throws db::DatabaseError on database errors (at startup that should
    ///         stop the app; periodic callers log and keep the previous content).
    void reload();

    /// Re-reads one book, or forgets it if it is gone.
    void refresh(BookId id);
    /// Removes one book.
    void forget(BookId id);

    /// The book's brief, or std::nullopt if unknown.
    std::optional<BookBrief> get(BookId id) const { return cache_.get(id.value); }
    /// Whether the book accepts likes/dislikes; false for unknown books.
    bool reactionsEnabled(BookId id) const {
        return cache_.with(id.value, [](const BookBrief& b) { return b.reactionsEnabled; }).value_or(false);
    }

    using Stats = cache::LocalCache<std::int64_t, BookBrief>::Stats;  ///< Size and hit/miss counters.
    /// Size and hit/miss counters.
    Stats stats() const { return cache_.stats(); }
    /// Estimated memory use in bytes: entries, titles and hash-table overhead.
    /// Walks every entry, so call it occasionally (the health job does, every 15 s).
    std::size_t approxBytes() const;

private:
    void markDirty(BookId id);

    std::shared_ptr<IBookRepository> books_;
    cache::LocalCache<std::int64_t, BookBrief> cache_;
    log::Logger log_;

    std::mutex reloadMutex_;  // one reload at a time
    std::mutex dirtyMutex_;
    bool reloading_ = false;
    std::unordered_set<std::int64_t> dirty_;  // refreshed during the running reload
};

}  // namespace caelitus::catalog
