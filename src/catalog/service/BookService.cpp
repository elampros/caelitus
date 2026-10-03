#include "caelitus/catalog/service/BookService.hpp"

#include "caelitus/catalog/domain/Rules.hpp"

#include <algorithm>
#include <unordered_set>

namespace caelitus::catalog {

namespace {

constexpr int kMaxPageCount = 100000;

// Normalized, de-duplicated (first occurrence wins), bounded tag names.
std::vector<std::string> normalizeTags(const std::vector<std::string>& input) {
    std::vector<std::string> out;
    for (const auto& t : input) {
        std::string tag = rules::normalizeTag(t);
        if (std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(std::move(tag));
    }
    if (out.size() > rules::kMaxTagsPerBook)
        throw ValidationError("tags", "at most " + std::to_string(rules::kMaxTagsPerBook) + " tags per book");
    return out;
}

std::string joinIds(const std::vector<AuthorId>& ids) {
    std::string out;
    for (const auto& id : ids) out += (out.empty() ? "" : ", ") + std::to_string(id.value);
    return out;
}

}  // namespace

BookService::BookService(std::shared_ptr<IBookRepository> books, std::shared_ptr<IAuthorRepository> authors,
                         std::shared_ptr<ICategoryRepository> categories, std::shared_ptr<ITagRepository> tags,
                         std::shared_ptr<db::ITransactionManager> tx, std::shared_ptr<mqtt::IMqttPublisher> events,
                         Clock clock, std::shared_ptr<BookCache> cache)
    : books_(std::move(books)),
      authors_(std::move(authors)),
      categories_(std::move(categories)),
      tags_(std::move(tags)),
      tx_(std::move(tx)),
      events_(std::move(events)),
      clock_(std::move(clock)),
      cache_(std::move(cache)) {
    if (!books_ || !authors_ || !categories_ || !tags_ || !tx_)
        throw std::invalid_argument("BookService: null dependency");
}

// Checks everything that needs no database.
Book BookService::validated(const BookInput& in, std::vector<std::string>& tagNames) const {
    Book b;
    b.title = rules::requiredText("title", in.title, rules::kTitleMax);
    if (auto isbn = rules::optionalText("isbn", in.isbn, 20)) b.isbn = rules::normalizeIsbn(*isbn);
    b.description = rules::optionalText("description", in.description, rules::kTextMax);
    b.publishedOn = in.publishedOn;
    b.language = rules::normalizeLanguage(in.language);
    if (in.pageCount && (*in.pageCount < 1 || *in.pageCount > kMaxPageCount))
        throw ValidationError("pageCount", "must be between 1 and " + std::to_string(kMaxPageCount));
    b.pageCount = in.pageCount;

    if (in.category.value <= 0) throw ValidationError("category", "is required");
    b.categoryId = in.category;

    if (in.authors.empty()) throw ValidationError("authors", "at least one author is required");
    if (in.authors.size() > rules::kMaxAuthorsPerBook)
        throw ValidationError("authors", "at most " + std::to_string(rules::kMaxAuthorsPerBook) + " authors per book");
    std::unordered_set<AuthorId> seen;
    for (const auto& a : in.authors)
        if (!seen.insert(a).second)
            throw ValidationError("authors", "author " + std::to_string(a.value) + " is listed twice");
    b.authorIds = in.authors;

    tagNames = normalizeTags(in.tags);
    b.reactionsEnabled = in.reactionsEnabled;
    return b;
}

// Checks references and creates tags; runs inside the write transaction.
void BookService::resolveReferences(Book& b, const std::vector<std::string>& tagNames) {
    if (!categories_->findById(b.categoryId))
        throw ValidationError("category", "category " + std::to_string(b.categoryId.value) + " does not exist");

    const auto found = authors_->findByIds(b.authorIds);
    if (found.size() != b.authorIds.size()) {
        std::vector<AuthorId> missing;
        for (const auto& id : b.authorIds)
            if (std::none_of(found.begin(), found.end(), [&](const Author& a) { return a.id == id; }))
                missing.push_back(id);
        throw ValidationError("authors", "unknown author id(s): " + joinIds(missing));
    }

    b.tagIds.clear();
    for (const auto& tag : tags_->findOrCreate(tagNames)) b.tagIds.push_back(tag.id);
}

BookDetails BookService::create(const BookInput& input) {
    std::vector<std::string> tagNames;
    Book book = validated(input, tagNames);
    book.createdAt = book.updatedAt = clock_();

    BookDetails created = tx_->inTransaction([&] {
        resolveReferences(book, tagNames);
        const BookId id = books_->insert(book);
        return *books_->details(id);
    });
    if (cache_) cache_->refresh(created.id);
    events_.bookEvent(created.id, "created", created.title);
    return created;
}

BookDetails BookService::update(BookId id, int expectedVersion, const BookInput& input) {
    std::vector<std::string> tagNames;
    Book book = validated(input, tagNames);
    book.id = id;
    book.updatedAt = clock_();

    BookDetails updated = tx_->inTransaction([&] {
        resolveReferences(book, tagNames);
        switch (books_->update(book, expectedVersion)) {
            case UpdateResult::NotFound: throw NotFoundError("book", id.value);
            case UpdateResult::VersionConflict:
                throw ConflictError("version_conflict", "The book was changed by someone else; reload and retry");
            case UpdateResult::Updated: break;
        }
        return *books_->details(id);
    });
    if (cache_) cache_->refresh(id);
    events_.bookEvent(id, "updated", updated.title);
    return updated;
}

BookDetails BookService::setReactionsEnabled(BookId id, bool enabled) {
    if (!books_->setReactionsEnabled(id, enabled)) throw NotFoundError("book", id.value);
    if (cache_) cache_->refresh(id);
    return get(id);
}

void BookService::remove(BookId id) {
    if (!books_->remove(id)) throw NotFoundError("book", id.value);
    if (cache_) cache_->forget(id);
    events_.bookEvent(id, "deleted", "");
}

BookDetails BookService::get(BookId id) {
    auto d = tx_->inTransaction(db::TransactionOptions::readOnlyTx(), [&] { return books_->details(id); });
    if (!d) throw NotFoundError("book", id.value);
    return *d;
}

Paged<BookSummary> BookService::search(BookQuery q) {
    q.page = validatePage(q.page);
    q.tags = normalizeTags(q.tags);
    q.titleContains = rules::optionalText("title", q.titleContains, rules::kSearchTextMax);
    if (q.language) q.language = rules::normalizeLanguage(*q.language);
    if (q.minRating && (*q.minRating < 1.0 || *q.minRating > 5.0))
        throw ValidationError("minRating", "must be between 1 and 5");
    if (q.publishedFrom && q.publishedTo && *q.publishedFrom > *q.publishedTo)
        throw ValidationError("publishedFrom", "must not be after publishedTo");

    return tx_->inTransaction(db::TransactionOptions::readOnlyTx(), [&] { return books_->search(q); });
}

std::vector<TagUsage> BookService::tags() { return tags_->listUsed(); }

}  // namespace caelitus::catalog
