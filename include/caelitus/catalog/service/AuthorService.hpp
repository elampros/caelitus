#pragma once

/// @file
/// Use cases for authors.
/// @ingroup catalog

#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/catalog/service/ServiceSupport.hpp"
#include "caelitus/db/ITransactionManager.hpp"

#include <memory>
#include <optional>
#include <string>

namespace caelitus::catalog {

/// What a caller provides to create or update an author.
struct AuthorInput {
    std::string name;                ///< Required, at most rules::kNameMax characters.
    std::optional<std::string> bio;  ///< At most rules::kTextMax characters; empty means none.
    std::optional<Date> birthDate;   ///< Not in the future.
};

/// Use cases for authors.
class AuthorService {
public:
    /// @param authors  Storage.
    /// @param tx       Groups the read and write of an update.
    /// @param clock    Source of createdAt / updatedAt.
    AuthorService(std::shared_ptr<IAuthorRepository> authors, std::shared_ptr<db::ITransactionManager> tx,
                  Clock clock = systemClock());

    /// Creates an author (version 1).
    /// @throws ValidationError
    Author create(const AuthorInput& input);

    /// Replaces every field of an author (optimistic locking).
    /// @param id               The author.
    /// @param expectedVersion  The version the caller last read; if someone
    ///                         else changed the author since, nothing is written.
    /// @param input            The new values.
    /// @return The author with its new version.
    /// @throws NotFoundError, ValidationError, ConflictError ("version_conflict").
    Author update(AuthorId id, int expectedVersion, const AuthorInput& input);

    /// Deletes an author with no books (hard delete).
    /// @throws NotFoundError, ConflictError ("author_has_books").
    void remove(AuthorId id);

    /// One author.
    /// @throws NotFoundError
    Author get(AuthorId id);
    /// A page of authors by name, optionally filtered by a case-insensitive substring.
    /// @throws ValidationError for a bad page or a too long search string.
    Paged<Author> search(const std::optional<std::string>& nameContains, const Page& page);

private:
    Author validated(const AuthorInput& input) const;

    std::shared_ptr<IAuthorRepository> authors_;
    std::shared_ptr<db::ITransactionManager> tx_;
    Clock clock_;
};

}  // namespace caelitus::catalog
