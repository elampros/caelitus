#pragma once

/// @file
/// The storage contracts the catalog services depend on.
/// @ingroup catalog
///
/// Implementations (catalog::mariadb, or in-memory fakes in tests) must:
/// - return std::nullopt / false for "does not exist" rather than throwing;
/// - throw ConflictError for constraint clashes that have domain meaning
///   (codes listed per method);
/// - run inside the caller's transaction when there is one.

#include "caelitus/catalog/domain/Model.hpp"
#include "caelitus/catalog/domain/Query.hpp"

#include <optional>
#include <string>
#include <vector>

namespace caelitus::catalog {

/// Outcome of an update with optimistic locking.
enum class UpdateResult {
    Updated,         ///< Written; the version was incremented.
    NotFound,        ///< No such row.
    VersionConflict  ///< The stored version differs from the expected one; nothing written.
};

/// Storage of categories.
class ICategoryRepository {
public:
    virtual ~ICategoryRepository() = default;

    /// Stores a new category.
    /// @return Its id.
    /// @throws ConflictError "name_taken" or "slug_taken".
    virtual CategoryId insert(const Category& category) = 0;
    /// The category, or std::nullopt.
    virtual std::optional<Category> findById(CategoryId id) = 0;
    /// Every category, by name.
    virtual std::vector<Category> listAll() = 0;
    /// Writes name and slug.
    /// @return false if not found.
    /// @throws ConflictError "name_taken" or "slug_taken".
    virtual bool update(const Category& category) = 0;
    /// Deletes a category.
    /// @return false if not found.
    /// @throws ConflictError "category_in_use" if books still use it.
    virtual bool remove(CategoryId id) = 0;
};

/// Storage of authors.
class IAuthorRepository {
public:
    virtual ~IAuthorRepository() = default;

    /// Stores a new author (version 1).
    /// @return Its id.
    virtual AuthorId insert(const Author& author) = 0;
    /// The author, or std::nullopt.
    virtual std::optional<Author> findById(AuthorId id) = 0;
    /// The authors among `ids` that exist, in any order.
    virtual std::vector<Author> findByIds(const std::vector<AuthorId>& ids) = 0;
    /// A page of authors by name; `nameContains` filters case-insensitively.
    virtual Paged<Author> search(const std::optional<std::string>& nameContains, const Page& page) = 0;
    /// Writes `author` if the stored version is `expectedVersion`, then
    /// increments the version.
    virtual UpdateResult update(const Author& author, int expectedVersion) = 0;
    /// Deletes an author.
    /// @return false if not found.
    /// @throws ConflictError "author_has_books" if any book lists the author.
    virtual bool remove(AuthorId id) = 0;
};

/// Storage of tags.
class ITagRepository {
public:
    virtual ~ITagRepository() = default;

    /// Tags for the given names, creating missing ones.
    /// @param names  Normalized (rules::normalizeTag) and distinct.
    virtual std::vector<Tag> findOrCreate(const std::vector<std::string>& names) = 0;
    /// Tags in use, by name, with how many books carry them.
    virtual std::vector<TagUsage> listUsed() = 0;
};

/// Storage of books, with their author and tag links.
class IBookRepository {
public:
    virtual ~IBookRepository() = default;

    /// Stores the book with its author and tag links (version 1).
    /// @return Its id.
    /// @throws ConflictError "isbn_taken".
    virtual BookId insert(const Book& book) = 0;
    /// Whether the book exists.
    virtual bool exists(BookId id) = 0;
    /// The stored book, or std::nullopt.
    virtual std::optional<Book> findById(BookId id) = 0;
    /// The book with its category, authors and tags, or std::nullopt.
    virtual std::optional<BookDetails> details(BookId id) = 0;
    /// One page of books matching the query.
    virtual Paged<BookSummary> search(const BookQuery& query) = 0;
    /// Every book, briefly; for loading BookCache.
    virtual std::vector<BookBrief> listBriefs() = 0;
    /// One book, briefly; for refreshing BookCache.
    virtual std::optional<BookBrief> findBrief(BookId id) = 0;
    /// Replaces fields and links if the stored version matches.
    ///
    /// Leaves reactionsEnabled alone (see setReactionsEnabled()).
    /// @throws ConflictError "isbn_taken".
    virtual UpdateResult update(const Book& book, int expectedVersion) = 0;
    /// Turns likes/dislikes on or off. A switch, not an edit: does not change
    /// the version.
    /// @return false if not found.
    virtual bool setReactionsEnabled(BookId id, bool enabled) = 0;
    /// Deletes the book with its links, reviews and reaction counts.
    /// @return false if not found.
    virtual bool remove(BookId id) = 0;
    /// Adds to the rating totals atomically. Does not change the book's version.
    /// @return false if the book does not exist.
    virtual bool adjustRating(BookId id, int countDelta, int sumDelta) = 0;
};

/// Storage of reviews.
class IReviewRepository {
public:
    virtual ~IReviewRepository() = default;

    /// Stores a new review.
    /// @return Its id.
    virtual ReviewId insert(const Review& review) = 0;
    /// The review, or std::nullopt.
    virtual std::optional<Review> findById(ReviewId id) = 0;
    /// findById() that also locks the review until the transaction ends, so a
    /// read-modify-write of it cannot interleave with another one.
    virtual std::optional<Review> lockById(ReviewId id) = 0;
    /// One page of a book's reviews, newest first.
    virtual Paged<Review> listByBook(BookId book, const Page& page) = 0;
    /// Writes rating, title and body.
    /// @return false if not found.
    virtual bool update(const Review& review) = 0;
    /// Deletes a review.
    /// @return false if not found.
    virtual bool remove(ReviewId id) = 0;
};

}  // namespace caelitus::catalog
