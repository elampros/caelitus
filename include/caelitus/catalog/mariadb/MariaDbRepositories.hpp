#pragma once

/// @file
/// The catalog repositories on MariaDB.
/// @ingroup catalog_mariadb

#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/db/SqlExecutor.hpp"

#include <memory>

namespace caelitus::catalog::mariadb {

// MariaDB implementations of the catalog repositories. They hold SQL and row
// mapping only; rules live in the services. Constraint violations with domain
// meaning (UNIQUE, FOREIGN KEY) are translated into ConflictError.

/// ICategoryRepository on MariaDB (table `categories`).
class MariaDbCategoryRepository final : public ICategoryRepository {
public:
    /// @param sql  Statement executor; runs in the caller's transaction.
    explicit MariaDbCategoryRepository(std::shared_ptr<db::SqlExecutor> sql) : sql_(std::move(sql)) {}

    CategoryId insert(const Category& category) override;
    std::optional<Category> findById(CategoryId id) override;
    std::vector<Category> listAll() override;
    bool update(const Category& category) override;
    bool remove(CategoryId id) override;

private:
    std::shared_ptr<db::SqlExecutor> sql_;
};

/// IAuthorRepository on MariaDB (table `authors`).
class MariaDbAuthorRepository final : public IAuthorRepository {
public:
    /// @param sql  Statement executor; runs in the caller's transaction.
    explicit MariaDbAuthorRepository(std::shared_ptr<db::SqlExecutor> sql) : sql_(std::move(sql)) {}

    AuthorId insert(const Author& author) override;
    std::optional<Author> findById(AuthorId id) override;
    std::vector<Author> findByIds(const std::vector<AuthorId>& ids) override;
    Paged<Author> search(const std::optional<std::string>& nameContains, const Page& page) override;
    UpdateResult update(const Author& author, int expectedVersion) override;
    bool remove(AuthorId id) override;

private:
    std::shared_ptr<db::SqlExecutor> sql_;
};

/// ITagRepository on MariaDB (table `tags`).
class MariaDbTagRepository final : public ITagRepository {
public:
    /// @param sql  Statement executor; runs in the caller's transaction.
    explicit MariaDbTagRepository(std::shared_ptr<db::SqlExecutor> sql) : sql_(std::move(sql)) {}

    std::vector<Tag> findOrCreate(const std::vector<std::string>& names) override;
    std::vector<TagUsage> listUsed() override;

private:
    std::shared_ptr<db::SqlExecutor> sql_;
};

/// IBookRepository on MariaDB (tables `books`, `book_authors`, `book_tags`).
///
/// Searches build their WHERE clause with SqlFilter (values always bound as
/// parameters) and load authors and tags of a whole page in two extra queries,
/// not one per book.
class MariaDbBookRepository final : public IBookRepository {
public:
    /// @param sql  Statement executor; runs in the caller's transaction.
    explicit MariaDbBookRepository(std::shared_ptr<db::SqlExecutor> sql) : sql_(std::move(sql)) {}

    BookId insert(const Book& book) override;
    bool exists(BookId id) override;
    std::optional<Book> findById(BookId id) override;
    std::optional<BookDetails> details(BookId id) override;
    Paged<BookSummary> search(const BookQuery& query) override;
    std::vector<BookBrief> listBriefs() override;
    std::optional<BookBrief> findBrief(BookId id) override;
    UpdateResult update(const Book& book, int expectedVersion) override;
    bool setReactionsEnabled(BookId id, bool enabled) override;
    bool remove(BookId id) override;
    bool adjustRating(BookId id, int countDelta, int sumDelta) override;

private:
    void writeLinks(BookId id, const Book& book, bool replace);
    void loadAuthorsAndTags(std::vector<BookSummary*>& books);

    std::shared_ptr<db::SqlExecutor> sql_;
};

/// IReviewRepository on MariaDB (table `reviews`).
class MariaDbReviewRepository final : public IReviewRepository {
public:
    /// @param sql  Statement executor; runs in the caller's transaction.
    explicit MariaDbReviewRepository(std::shared_ptr<db::SqlExecutor> sql) : sql_(std::move(sql)) {}

    ReviewId insert(const Review& review) override;
    std::optional<Review> findById(ReviewId id) override;
    std::optional<Review> lockById(ReviewId id) override;
    Paged<Review> listByBook(BookId book, const Page& page) override;
    bool update(const Review& review) override;
    bool remove(ReviewId id) override;

private:
    std::shared_ptr<db::SqlExecutor> sql_;
};

}  // namespace caelitus::catalog::mariadb
