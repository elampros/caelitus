/// @file
/// catalog::ReactionService: the in-memory like buffer, its periodic flush,
/// per-period counts and rankings in the catalog time zone.
/// @ingroup catalog

#include "caelitus/catalog/service/ReactionService.hpp"

namespace caelitus::catalog {

namespace {

constexpr Period kAllPeriods[] = {Period::Today,      Period::Yesterday, Period::Last7Days,
                                  Period::Last30Days, Period::LastYear,  Period::AllTime};

}  // namespace

const char* toString(Period period) {
    switch (period) {
        case Period::Today: return "today";
        case Period::Yesterday: return "yesterday";
        case Period::Last7Days: return "last7Days";
        case Period::Last30Days: return "last30Days";
        case Period::LastYear: return "lastYear";
        case Period::AllTime: return "allTime";
    }
    return "?";
}

std::optional<Period> parsePeriod(const std::string& text) {
    for (Period p : kAllPeriods)
        if (text == toString(p)) return p;
    return std::nullopt;
}

ReactionService::ReactionService(std::shared_ptr<IReactionRepository> reactions, std::shared_ptr<IBookRepository> books,
                                 std::shared_ptr<BookCache> cache, std::shared_ptr<db::ITransactionManager> tx,
                                 TimeZone timeZone, std::size_t maxBufferedEntries, Clock clock)
    : reactions_(std::move(reactions)),
      books_(std::move(books)),
      cache_(std::move(cache)),
      tx_(std::move(tx)),
      timeZone_(std::move(timeZone)),
      maxBuffered_(maxBufferedEntries),
      clock_(std::move(clock)),
      log_(log::get("catalog.reactions")) {
    if (!reactions_ || !books_ || !cache_ || !tx_) throw std::invalid_argument("ReactionService: null dependency");
}

bool ReactionService::record(BookId book, Reaction reaction) {
    if (book.value <= 0) throw ValidationError("bookId", "must be a positive id");
    if (!cache_->reactionsEnabled(book)) return false;
    const Key key{book.value, timeZone_.localDate(clock_()).toDays()};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buffer_.find(key);
        if (it == buffer_.end()) {
            if (buffer_.size() >= maxBuffered_) {
                ++dropped_;
            } else {
                it = buffer_.emplace(key, ReactionCounts{}).first;
            }
        }
        if (it != buffer_.end()) {
            (reaction == Reaction::Like ? it->second.likes : it->second.dislikes) += 1;
            return true;
        }
    }
    if (auto suppressed = droppedLog_.allow())
        log_->warn("Reaction buffer full ({} entries; is the database down?); {} reactions dropped so far{}",
                   maxBuffered_, dropped_.load(), log::suppressedSuffix(*suppressed));
    return false;
}

void ReactionService::restore(const Buffer& unwritten) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, counts] : unwritten) {
        auto& slot = buffer_[key];  // may exceed the limit briefly; nothing is lost
        slot.likes += counts.likes;
        slot.dislikes += counts.dislikes;
    }
}

ReactionService::FlushResult ReactionService::flush() {
    std::lock_guard<std::mutex> flushLock(flushMutex_);
    Buffer batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(buffer_);
    }
    FlushResult result;
    if (batch.empty()) return result;

    std::vector<DailyReactions> deltas;
    deltas.reserve(batch.size());
    std::int64_t total = 0;
    for (const auto& [key, counts] : batch) {
        deltas.push_back({BookId(key.book), Date::fromDays(key.day), counts.likes, counts.dislikes});
        total += counts.likes + counts.dislikes;
    }

    try {
        result.skipped = tx_->inTransaction([&] { return reactions_->add(deltas); });
        result.written = total - result.skipped;
    } catch (const std::exception& e) {
        restore(batch);
        result.failed = true;
        if (auto suppressed = failedLog_.allow())
            log_->warn("Writing {} reactions failed; will retry: {}{}", total, e.what(),
                       log::suppressedSuffix(*suppressed));
        return result;
    }

    log_->debug("Stored {} reactions ({} book-days)", result.written, batch.size());
    if (result.skipped > 0)
        if (auto suppressed = skippedLog_.allow())
            log_->warn("Ignored {} reactions for books that do not exist{}", result.skipped,
                       log::suppressedSuffix(*suppressed));
    return result;
}

std::optional<DateRange> ReactionService::range(Period period) const {
    const std::int64_t today = timeZone_.localDate(clock_()).toDays();
    auto days = [&](std::int64_t back) { return DateRange{Date::fromDays(today - back), Date::fromDays(today)}; };
    switch (period) {
        case Period::Today: return days(0);
        case Period::Yesterday: return DateRange{Date::fromDays(today - 1), Date::fromDays(today - 1)};
        case Period::Last7Days: return days(6);
        case Period::Last30Days: return days(29);
        case Period::LastYear: return days(364);
        case Period::AllTime: return std::nullopt;
    }
    return std::nullopt;
}

ReactionStats ReactionService::stats(BookId book) {
    std::vector<std::optional<DateRange>> ranges;
    for (Period p : kAllPeriods) ranges.push_back(range(p));

    auto counts = tx_->inTransaction(db::TransactionOptions::readOnlyTx(), [&] {
        if (!books_->exists(book)) throw NotFoundError("book", book.value);
        return reactions_->counts(book, ranges);
    });
    ReactionStats out{book, {}};
    for (std::size_t i = 0; i < counts.size(); ++i) out.periods.emplace_back(kAllPeriods[i], counts[i]);
    return out;
}

std::vector<RankedBook> ReactionService::top(Period period, ReactionOrder order, int limit) {
    if (limit < 1 || limit > Page::kMaxSize)
        throw ValidationError("limit", "must be between 1 and " + std::to_string(Page::kMaxSize));
    return reactions_->top(range(period), order, limit);
}

std::size_t ReactionService::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

std::int64_t ReactionService::deleteOlderThan(int keepDays) {
    if (keepDays < 366) throw std::invalid_argument("keepDays must be at least 366");
    const Date cutoff = Date::fromDays(timeZone_.localDate(clock_()).toDays() - keepDays + 1);
    const std::int64_t deleted = tx_->inTransaction([&] { return reactions_->deleteBefore(cutoff); });
    log_->info("Deleted {} per-day reaction rows before {}", deleted, cutoff.toString());
    return deleted;
}

ReactionService::BufferStats ReactionService::bufferStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {buffer_.size(), maxBuffered_, dropped_.load()};
}

}  // namespace caelitus::catalog
