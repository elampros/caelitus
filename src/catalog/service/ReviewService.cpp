/// @file
/// catalog::ReviewService: review use cases; every change updates the book's
/// rating count and average in the same transaction.
/// @ingroup catalog

#include "caelitus/catalog/service/ReviewService.hpp"

#include "caelitus/catalog/domain/Rules.hpp"

namespace caelitus::catalog {

ReviewService::ReviewService(std::shared_ptr<IReviewRepository> reviews, std::shared_ptr<IBookRepository> books,
                             std::shared_ptr<db::ITransactionManager> tx, std::shared_ptr<mqtt::IMqttPublisher> events,
                             Clock clock)
    : reviews_(std::move(reviews)),
      books_(std::move(books)),
      tx_(std::move(tx)),
      events_(std::move(events)),
      clock_(std::move(clock)) {
    if (!reviews_ || !books_ || !tx_) throw std::invalid_argument("ReviewService: null dependency");
}

Review ReviewService::validated(const ReviewInput& in) const {
    Review r;
    r.reviewerName = rules::requiredText("reviewerName", in.reviewerName, rules::kReviewerMax);
    r.rating = rules::validateRating(in.rating);
    r.title = rules::optionalText("title", in.title, rules::kReviewTitleMax);
    r.body = rules::requiredText("body", in.body, rules::kTextMax);
    return r;
}

Review ReviewService::add(BookId book, const ReviewInput& input) {
    Review r = validated(input);
    r.bookId = book;
    r.createdAt = r.updatedAt = clock_();

    tx_->inTransaction([&] {
        // Updating the totals first doubles as the existence check and locks
        // the book row, so concurrent reviews of one book serialize here.
        if (!books_->adjustRating(book, +1, r.rating)) throw NotFoundError("book", book.value);
        r.id = reviews_->insert(r);
    });
    events_.bookEvent(book, "reviews", std::to_string(r.rating) + " " + r.reviewerName);
    return r;
}

Review ReviewService::update(ReviewId id, const ReviewInput& input) {
    Review changes = validated(input);
    return tx_->inTransaction([&] {
        auto current = reviews_->lockById(id);
        if (!current) throw NotFoundError("review", id.value);
        Review r = *current;
        books_->adjustRating(r.bookId, 0, changes.rating - r.rating);
        r.reviewerName = changes.reviewerName;
        r.rating = changes.rating;
        r.title = changes.title;
        r.body = changes.body;
        r.updatedAt = clock_();
        reviews_->update(r);
        return r;
    });
}

void ReviewService::remove(ReviewId id) {
    tx_->inTransaction([&] {
        auto current = reviews_->lockById(id);
        if (!current) throw NotFoundError("review", id.value);
        books_->adjustRating(current->bookId, -1, -current->rating);
        reviews_->remove(id);
    });
}

Review ReviewService::get(ReviewId id) {
    auto r = reviews_->findById(id);
    if (!r) throw NotFoundError("review", id.value);
    return *r;
}

Paged<Review> ReviewService::listForBook(BookId book, const Page& page) {
    const Page p = validatePage(page);
    return tx_->inTransaction(db::TransactionOptions::readOnlyTx(), [&] {
        if (!books_->exists(book)) throw NotFoundError("book", book.value);
        return reviews_->listByBook(book, p);
    });
}

}  // namespace caelitus::catalog
