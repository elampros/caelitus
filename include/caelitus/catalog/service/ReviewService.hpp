#pragma once

/// @file
/// Use cases for reviews.
/// @ingroup catalog

#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/catalog/service/ServiceSupport.hpp"
#include "caelitus/db/ITransactionManager.hpp"

#include <memory>
#include <optional>
#include <string>

namespace caelitus::catalog {

/// What a caller provides to add or update a review.
struct ReviewInput {
    std::string reviewerName;          ///< Required, at most rules::kReviewerMax characters.
    int rating = 0;                    ///< 1..5.
    std::optional<std::string> title;  ///< At most rules::kReviewTitleMax characters.
    std::string body;                  ///< Required, at most rules::kTextMax characters.
};

/// Use cases for reviews.
///
/// Keeps the book's rating totals (Book::ratingCount, Book::ratingSum) in step
/// with its reviews: every change to a review and to the totals happens in one
/// transaction, with the rows locked so concurrent changes serialize.
class ReviewService {
public:
    /// @param reviews  Review storage.
    /// @param books    To adjust the book's rating totals.
    /// @param tx       Groups a review change with the totals.
    /// @param events   Where "reviews" events go; may be null.
    /// @param clock    Source of createdAt / updatedAt.
    ReviewService(std::shared_ptr<IReviewRepository> reviews, std::shared_ptr<IBookRepository> books,
                  std::shared_ptr<db::ITransactionManager> tx, std::shared_ptr<mqtt::IMqttPublisher> events = nullptr,
                  Clock clock = systemClock());

    /// Adds a review and updates the book's totals. Publishes "reviews".
    /// @throws ValidationError, NotFoundError (book).
    Review add(BookId book, const ReviewInput& input);
    /// Replaces a review's fields and adjusts the book's totals.
    /// @throws ValidationError, NotFoundError (review).
    Review update(ReviewId id, const ReviewInput& input);
    /// Deletes a review and adjusts the book's totals.
    /// @throws NotFoundError (review).
    void remove(ReviewId id);

    /// One review.
    /// @throws NotFoundError
    Review get(ReviewId id);
    /// One page of a book's reviews, newest first.
    /// @throws NotFoundError (book), ValidationError (page).
    Paged<Review> listForBook(BookId book, const Page& page);

private:
    Review validated(const ReviewInput& input) const;

    std::shared_ptr<IReviewRepository> reviews_;
    std::shared_ptr<IBookRepository> books_;
    std::shared_ptr<db::ITransactionManager> tx_;
    CatalogEvents events_;
    Clock clock_;
};

}  // namespace caelitus::catalog
