#pragma once

/// @file
/// Use cases for books.
/// @ingroup catalog

#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/catalog/service/BookCache.hpp"
#include "caelitus/catalog/service/ServiceSupport.hpp"
#include "caelitus/db/ITransactionManager.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace caelitus::catalog {

/// What a caller provides to create or update a book.
struct BookInput {
    std::string title;                       ///< Required, at most rules::kTitleMax characters.
    std::optional<std::string> isbn;         ///< ISBN-10 or -13, hyphens allowed; stored as ISBN-13.
    std::optional<std::string> description;  ///< At most rules::kTextMax characters.
    Date publishedOn{1970, 1, 1};            ///< Publication date.
    std::string language;                    ///< ISO 639-1 code: "el", "en", ...
    std::optional<int> pageCount;            ///< Positive.
    CategoryId category;                     ///< Must exist.
    std::vector<AuthorId> authors;           ///< At least one, distinct, all existing; in cover order.
    std::vector<std::string> tags;           ///< Free text, normalized; created on first use.
    bool reactionsEnabled = false;           ///< create() only; afterwards use setReactionsEnabled().
};

/// Use cases for books.
///
/// Every change runs in one transaction, then refreshes the BookCache entry
/// and publishes a CatalogEvents event (after the commit, so listeners never
/// see a change that was rolled back).
class BookService {
public:
    /// @param books       Book storage.
    /// @param authors     To check that the authors exist.
    /// @param categories  To check that the category exists.
    /// @param tags        To resolve tag names to ids.
    /// @param tx          Groups the checks and writes of one use case.
    /// @param events      Where book events go; may be null.
    /// @param clock       Source of createdAt / updatedAt.
    /// @param cache       Refreshed after each change; may be null.
    BookService(std::shared_ptr<IBookRepository> books, std::shared_ptr<IAuthorRepository> authors,
                std::shared_ptr<ICategoryRepository> categories, std::shared_ptr<ITagRepository> tags,
                std::shared_ptr<db::ITransactionManager> tx, std::shared_ptr<mqtt::IMqttPublisher> events = nullptr,
                Clock clock = systemClock(), std::shared_ptr<BookCache> cache = nullptr);

    /// Adds a book. Publishes "created".
    /// @throws ValidationError (an unknown category or author included),
    ///         ConflictError ("isbn_taken").
    BookDetails create(const BookInput& input);

    /// Replaces all fields, authors and tags (not reactionsEnabled), with
    /// optimistic locking. Publishes "updated".
    /// @param id               The book.
    /// @param expectedVersion  The version the caller last read.
    /// @param input            The new values.
    /// @throws NotFoundError, ValidationError,
    ///         ConflictError ("version_conflict", "isbn_taken").
    BookDetails update(BookId id, int expectedVersion, const BookInput& input);

    /// Turns likes/dislikes on or off for a book; takes effect immediately
    /// (the BookCache entry is refreshed). Not versioned: it is a switch, not
    /// an edit.
    /// @throws NotFoundError
    BookDetails setReactionsEnabled(BookId id, bool enabled);

    /// Deletes a book with its reviews and reaction counts (hard delete).
    /// Publishes "deleted".
    /// @throws NotFoundError
    void remove(BookId id);

    /// One book with everything about it.
    /// @throws NotFoundError
    BookDetails get(BookId id);

    /// Searches books.
    ///
    /// Validates and normalizes the query (tags, language, page), then
    /// searches in a read-only transaction so the page and its total are
    /// consistent.
    /// @throws ValidationError
    Paged<BookSummary> search(BookQuery query);

    /// Tags in use, by name, with their book counts.
    std::vector<TagUsage> tags();

private:
    Book validated(const BookInput& input, std::vector<std::string>& tagNames) const;
    void resolveReferences(Book& book, const std::vector<std::string>& tagNames);

    std::shared_ptr<IBookRepository> books_;
    std::shared_ptr<IAuthorRepository> authors_;
    std::shared_ptr<ICategoryRepository> categories_;
    std::shared_ptr<ITagRepository> tags_;
    std::shared_ptr<db::ITransactionManager> tx_;
    CatalogEvents events_;
    Clock clock_;
    std::shared_ptr<BookCache> cache_;  // optional; refreshed after each commit
};

}  // namespace caelitus::catalog
