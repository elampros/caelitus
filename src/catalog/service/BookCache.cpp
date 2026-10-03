/// @file
/// catalog::BookCache: full preload, per-book refresh after changes, periodic
/// reload, and the memory estimate reported by the health check.
/// @ingroup catalog

#include "caelitus/catalog/service/BookCache.hpp"

#include <chrono>

namespace caelitus::catalog {

BookCache::BookCache(std::shared_ptr<IBookRepository> books) : books_(std::move(books)), log_(log::get("cache.books")) {
    if (!books_) throw std::invalid_argument("BookCache: repository is null");
}

void BookCache::markDirty(BookId id) {
    std::lock_guard<std::mutex> lock(dirtyMutex_);
    if (reloading_) dirty_.insert(id.value);
}

void BookCache::reload() {
    std::lock_guard<std::mutex> reloadLock(reloadMutex_);
    {
        std::lock_guard<std::mutex> lock(dirtyMutex_);
        reloading_ = true;
        dirty_.clear();
    }
    const auto start = std::chrono::steady_clock::now();

    std::unordered_map<std::int64_t, BookBrief> content;
    std::size_t enabled = 0;
    try {
        for (auto& brief : books_->listBriefs()) {
            enabled += brief.reactionsEnabled ? 1 : 0;
            content.emplace(brief.id.value, std::move(brief));
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(dirtyMutex_);
        reloading_ = false;
        throw;
    }
    const std::size_t total = content.size();
    cache_.replaceAll(std::move(content));

    std::unordered_set<std::int64_t> changedMeanwhile;
    {
        std::lock_guard<std::mutex> lock(dirtyMutex_);
        reloading_ = false;
        changedMeanwhile.swap(dirty_);
    }
    for (auto id : changedMeanwhile) refresh(BookId(id));

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    log_->debug("Book cache reloaded: {} books ({} accept reactions) in {:.1f} ms", total, enabled, ms);
}

void BookCache::refresh(BookId id) {
    markDirty(id);
    if (auto brief = books_->findBrief(id)) cache_.put(id.value, std::move(*brief));
    else cache_.erase(id.value);
}

void BookCache::forget(BookId id) {
    markDirty(id);
    cache_.erase(id.value);
}

std::size_t BookCache::approxBytes() const {
    // Per entry: the key and value, one hash node (two pointers) and one bucket
    // pointer, plus the title's heap buffer when it does not fit inline.
    constexpr std::size_t perEntry = sizeof(std::int64_t) + sizeof(BookBrief) + 3 * sizeof(void*);
    std::size_t bytes = 0;
    cache_.forEach([&](const std::int64_t&, const BookBrief& b) {
        bytes += perEntry + (b.title.capacity() > 15 ? b.title.capacity() + 1 : 0);
    });
    return bytes;
}

}  // namespace caelitus::catalog
