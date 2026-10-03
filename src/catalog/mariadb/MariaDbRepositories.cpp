// Category, author, tag and review repositories.

#include "caelitus/catalog/mariadb/MariaDbRepositories.hpp"

#include "caelitus/core/DomainErrors.hpp"
#include "caelitus/db/DbErrors.hpp"
#include "catalog/mariadb/SqlFilter.hpp"

namespace caelitus::catalog::mariadb {

using db::Row;

namespace {

Category toCategory(const Row& r) {
    return {CategoryId(r.get<std::int64_t>("id")), r.get<std::string>("name"), r.get<std::string>("slug")};
}

constexpr const char* kAuthorColumns = "id, name, bio, birth_date, version, created_at, updated_at";

Author toAuthor(const Row& r) {
    Author a;
    a.id = AuthorId(r.get<std::int64_t>("id"));
    a.name = r.get<std::string>("name");
    a.bio = r.getOptional<std::string>("bio");
    a.birthDate = r.getOptional<Date>("birth_date");
    a.version = r.get<int>("version");
    a.createdAt = r.get<Timestamp>("created_at");
    a.updatedAt = r.get<Timestamp>("updated_at");
    return a;
}

constexpr const char* kReviewColumns = "id, book_id, reviewer_name, rating, title, body, created_at, updated_at";

Review toReview(const Row& r) {
    Review v;
    v.id = ReviewId(r.get<std::int64_t>("id"));
    v.bookId = BookId(r.get<std::int64_t>("book_id"));
    v.reviewerName = r.get<std::string>("reviewer_name");
    v.rating = r.get<int>("rating");
    v.title = r.getOptional<std::string>("title");
    v.body = r.get<std::string>("body");
    v.createdAt = r.get<Timestamp>("created_at");
    v.updatedAt = r.get<Timestamp>("updated_at");
    return v;
}

// Which unique constraint a duplicate-key error hit.
bool hits(const db::DuplicateKeyError& e, const char* constraint) {
    return std::string(e.what()).find(constraint) != std::string::npos;
}

[[noreturn]] void rethrowCategoryDuplicate(const db::DuplicateKeyError& e) {
    if (hits(e, "uq_categories_slug")) throw ConflictError("slug_taken", "A category with this slug already exists");
    throw ConflictError("name_taken", "A category with this name already exists");
}

}  // namespace

// ---- Categories --------------------------------------------------------------

CategoryId MariaDbCategoryRepository::insert(const Category& c) {
    try {
        auto r = sql_->insert("INSERT INTO categories (name, slug) VALUES (?, ?)", {c.name, c.slug});
        return CategoryId(static_cast<std::int64_t>(r.lastInsertId));
    } catch (const db::DuplicateKeyError& e) {
        rethrowCategoryDuplicate(e);
    }
}

std::optional<Category> MariaDbCategoryRepository::findById(CategoryId id) {
    auto row = sql_->queryOne("SELECT id, name, slug FROM categories WHERE id = ?", {id.value});
    if (!row) return std::nullopt;
    return toCategory(*row);
}

std::vector<Category> MariaDbCategoryRepository::listAll() {
    return sql_->queryList("SELECT id, name, slug FROM categories ORDER BY name, id", {}, toCategory);
}

bool MariaDbCategoryRepository::update(const Category& c) {
    try {
        auto r = sql_->execute("UPDATE categories SET name = ?, slug = ? WHERE id = ?", {c.name, c.slug, c.id.value});
        // 0 affected rows also means "found but unchanged".
        return r.affectedRows > 0 || findById(c.id).has_value();
    } catch (const db::DuplicateKeyError& e) {
        rethrowCategoryDuplicate(e);
    }
}

bool MariaDbCategoryRepository::remove(CategoryId id) {
    try {
        return sql_->execute("DELETE FROM categories WHERE id = ?", {id.value}).affectedRows > 0;
    } catch (const db::ForeignKeyError&) {
        throw ConflictError("category_in_use", "The category still has books");
    }
}

// ---- Authors -----------------------------------------------------------------

AuthorId MariaDbAuthorRepository::insert(const Author& a) {
    auto r = sql_->insert(
        "INSERT INTO authors (name, bio, birth_date, version, created_at, updated_at) VALUES (?, ?, ?, 1, ?, ?)",
        {a.name, a.bio, a.birthDate, a.createdAt, a.updatedAt});
    return AuthorId(static_cast<std::int64_t>(r.lastInsertId));
}

std::optional<Author> MariaDbAuthorRepository::findById(AuthorId id) {
    auto row = sql_->queryOne(std::string("SELECT ") + kAuthorColumns + " FROM authors WHERE id = ?", {id.value});
    if (!row) return std::nullopt;
    return toAuthor(*row);
}

std::vector<Author> MariaDbAuthorRepository::findByIds(const std::vector<AuthorId>& ids) {
    if (ids.empty()) return {};
    return sql_->queryList(std::string("SELECT ") + kAuthorColumns + " FROM authors WHERE id IN (" +
                               placeholders(ids.size()) + ")",
                           idParams(ids), toAuthor);
}

Paged<Author> MariaDbAuthorRepository::search(const std::optional<std::string>& nameContains, const Page& page) {
    SqlFilter filter;
    if (nameContains) filter.add("name LIKE ?", {likeContains(*nameContains)});  // collation is case-insensitive

    Paged<Author> out;
    out.page = page;
    out.total =
        sql_->queryScalar<std::int64_t>("SELECT COUNT(*) FROM authors" + filter.where(), filter.params()).value_or(0);

    db::Params params = filter.params();
    params.emplace_back(page.size);
    params.emplace_back(page.offset());
    out.items = sql_->queryList(std::string("SELECT ") + kAuthorColumns + " FROM authors" + filter.where() +
                                    " ORDER BY name, id LIMIT ? OFFSET ?",
                                params, toAuthor);
    return out;
}

UpdateResult MariaDbAuthorRepository::update(const Author& a, int expectedVersion) {
    auto r = sql_->execute(
        "UPDATE authors SET name = ?, bio = ?, birth_date = ?, updated_at = ?, version = version + 1"
        " WHERE id = ? AND version = ?",
        {a.name, a.bio, a.birthDate, a.updatedAt, a.id.value, expectedVersion});
    if (r.affectedRows > 0) return UpdateResult::Updated;
    return findById(a.id) ? UpdateResult::VersionConflict : UpdateResult::NotFound;
}

bool MariaDbAuthorRepository::remove(AuthorId id) {
    try {
        return sql_->execute("DELETE FROM authors WHERE id = ?", {id.value}).affectedRows > 0;
    } catch (const db::ForeignKeyError&) {
        throw ConflictError("author_has_books", "The author still has books");
    }
}

// ---- Tags --------------------------------------------------------------------

std::vector<Tag> MariaDbTagRepository::findOrCreate(const std::vector<std::string>& names) {
    if (names.empty()) return {};
    db::Params params(names.begin(), names.end());

    std::string values;
    for (std::size_t i = 0; i < names.size(); ++i) values += (i == 0 ? "(?)" : ", (?)");
    // A no-op update instead of INSERT IGNORE, which would also hide real errors.
    sql_->execute("INSERT INTO tags (name) VALUES " + values + " ON DUPLICATE KEY UPDATE name = name", params);

    return sql_->queryList("SELECT id, name FROM tags WHERE name IN (" + placeholders(names.size()) + ") ORDER BY name",
                           params, [](const Row& r) {
                               return Tag{TagId(r.get<std::int64_t>("id")), r.get<std::string>("name")};
                           });
}

std::vector<TagUsage> MariaDbTagRepository::listUsed() {
    return sql_->queryList(
        "SELECT t.id, t.name, COUNT(*) AS books FROM tags t JOIN book_tags bt ON bt.tag_id = t.id"
        " GROUP BY t.id, t.name ORDER BY t.name",
        {}, [](const Row& r) {
            return TagUsage{Tag{TagId(r.get<std::int64_t>("id")), r.get<std::string>("name")},
                            r.get<std::int64_t>("books")};
        });
}

// ---- Reviews -----------------------------------------------------------------

ReviewId MariaDbReviewRepository::insert(const Review& v) {
    auto r = sql_->insert(
        "INSERT INTO reviews (book_id, reviewer_name, rating, title, body, created_at, updated_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)",
        {v.bookId.value, v.reviewerName, v.rating, v.title, v.body, v.createdAt, v.updatedAt});
    return ReviewId(static_cast<std::int64_t>(r.lastInsertId));
}

std::optional<Review> MariaDbReviewRepository::findById(ReviewId id) {
    auto row = sql_->queryOne(std::string("SELECT ") + kReviewColumns + " FROM reviews WHERE id = ?", {id.value});
    if (!row) return std::nullopt;
    return toReview(*row);
}

std::optional<Review> MariaDbReviewRepository::lockById(ReviewId id) {
    auto row =
        sql_->queryOne(std::string("SELECT ") + kReviewColumns + " FROM reviews WHERE id = ? FOR UPDATE", {id.value});
    if (!row) return std::nullopt;
    return toReview(*row);
}

Paged<Review> MariaDbReviewRepository::listByBook(BookId book, const Page& page) {
    Paged<Review> out;
    out.page = page;
    out.total =
        sql_->queryScalar<std::int64_t>("SELECT COUNT(*) FROM reviews WHERE book_id = ?", {book.value}).value_or(0);
    out.items =
        sql_->queryList(std::string("SELECT ") + kReviewColumns +
                            " FROM reviews WHERE book_id = ? ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?",
                        {book.value, page.size, page.offset()}, toReview);
    return out;
}

bool MariaDbReviewRepository::update(const Review& v) {
    auto r = sql_->execute(
        "UPDATE reviews SET reviewer_name = ?, rating = ?, title = ?, body = ?, updated_at = ? WHERE id = ?",
        {v.reviewerName, v.rating, v.title, v.body, v.updatedAt, v.id.value});
    return r.affectedRows > 0 || findById(v.id).has_value();
}

bool MariaDbReviewRepository::remove(ReviewId id) {
    return sql_->execute("DELETE FROM reviews WHERE id = ?", {id.value}).affectedRows > 0;
}

}  // namespace caelitus::catalog::mariadb
