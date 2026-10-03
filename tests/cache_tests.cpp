// Tests for LocalCache and BookCache.

#include "TestHarness.hpp"

#include "caelitus/cache/LocalCache.hpp"
#include "caelitus/catalog/service/BookCache.hpp"

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <thread>

using namespace caelitus;
using namespace caelitus::catalog;

TEST(local_cache_basics) {
    cache::LocalCache<int, std::string> c;
    CHECK(!c.get(1).has_value());
    c.put(1, "one");
    c.put(1, "uno");
    CHECK_EQ(c.get(1).value(), "uno");
    CHECK_EQ(c.with(1, [](const std::string& s) { return s.size(); }).value(), 3u);
    c.erase(1);
    CHECK(!c.get(1).has_value());
    c.replaceAll({{2, "two"}, {3, "three"}});
    auto s = c.stats();
    CHECK_EQ(s.size, 2u);
    CHECK_EQ(s.hits, 2u);
    CHECK_EQ(s.misses, 2u);
}

TEST(local_cache_concurrent_readers_and_writers) {
    cache::LocalCache<int, int> c;
    for (int i = 0; i < 1000; ++i) c.put(i, i);
    std::atomic<bool> stop{false};
    std::atomic<long> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t)
        threads.emplace_back([&] {
            while (!stop)
                for (int i = 0; i < 1000; ++i)
                    if (auto v = c.get(i); v && *v % 1000 != i) ++wrong;  // values are i + k * 1000
        });
    threads.emplace_back([&] {
        for (int round = 1; round <= 200; ++round)
            for (int i = 0; i < 1000; ++i) c.put(i, i + round * 1000);
        stop = true;
    });
    for (auto& t : threads) t.join();
    CHECK_EQ(wrong.load(), 0);
}

namespace {

// Only what BookCache uses; listBriefs can be paused to stage a race.
class BriefsOnly final : public IBookRepository {
public:
    std::mutex mutex;
    std::map<std::int64_t, BookBrief> rows;
    std::promise<void> paused;        // set when listBriefs has taken its snapshot
    std::shared_future<void> resume;  // listBriefs waits for this, if valid

    std::vector<BookBrief> listBriefs() override {
        std::vector<BookBrief> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& [id, b] : rows) snapshot.push_back(b);
        }
        if (resume.valid()) {
            paused.set_value();
            resume.wait();
        }
        return snapshot;
    }
    std::optional<BookBrief> findBrief(BookId id) override {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = rows.find(id.value);
        if (it == rows.end()) return std::nullopt;
        return it->second;
    }

    BookId insert(const Book&) override { throw std::logic_error("unused"); }
    bool exists(BookId) override { throw std::logic_error("unused"); }
    std::optional<Book> findById(BookId) override { throw std::logic_error("unused"); }
    std::optional<BookDetails> details(BookId) override { throw std::logic_error("unused"); }
    Paged<BookSummary> search(const BookQuery&) override { throw std::logic_error("unused"); }
    UpdateResult update(const Book&, int) override { throw std::logic_error("unused"); }
    bool setReactionsEnabled(BookId, bool) override { throw std::logic_error("unused"); }
    bool remove(BookId) override { throw std::logic_error("unused"); }
    bool adjustRating(BookId, int, int) override { throw std::logic_error("unused"); }
};

}  // namespace

TEST(book_cache_change_during_reload_is_not_lost) {
    auto repo = std::make_shared<BriefsOnly>();
    repo->rows[1] = {BookId(1), "Dune", false};
    BookCache cache(repo);
    cache.reload();
    CHECK(!cache.reactionsEnabled(BookId(1)));

    std::promise<void> go;
    repo->resume = go.get_future().share();
    auto pausedAt = repo->paused.get_future();
    std::thread reloader([&] { cache.reload(); });  // snapshots "disabled", then waits
    pausedAt.wait();

    {  // meanwhile the book is switched on and the service refreshes the cache
        std::lock_guard<std::mutex> lock(repo->mutex);
        repo->rows[1].reactionsEnabled = true;
    }
    cache.refresh(BookId(1));
    CHECK(cache.reactionsEnabled(BookId(1)));

    go.set_value();  // the reload now swaps in its older snapshot...
    reloader.join();
    CHECK(cache.reactionsEnabled(BookId(1)));  // ...but the refresh is re-applied
}

TEST(book_cache_forgets_deleted_books_on_refresh) {
    auto repo = std::make_shared<BriefsOnly>();
    repo->rows[1] = {BookId(1), "Dune", true};
    BookCache cache(repo);
    cache.reload();
    repo->rows.erase(1);
    cache.refresh(BookId(1));
    CHECK(!cache.get(BookId(1)).has_value());
    CHECK(!cache.reactionsEnabled(BookId(1)));
}

int main() { return test::runAll(); }
