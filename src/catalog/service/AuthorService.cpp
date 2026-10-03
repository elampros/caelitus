#include "caelitus/catalog/service/AuthorService.hpp"

#include "caelitus/catalog/domain/Rules.hpp"

namespace caelitus::catalog {

AuthorService::AuthorService(std::shared_ptr<IAuthorRepository> authors, std::shared_ptr<db::ITransactionManager> tx,
                             Clock clock)
    : authors_(std::move(authors)),
      tx_(std::move(tx)),
      clock_(std::move(clock)) {
    if (!authors_ || !tx_) throw std::invalid_argument("AuthorService: null dependency");
}

Author AuthorService::validated(const AuthorInput& in) const {
    Author a;
    a.name = rules::requiredText("name", in.name, rules::kNameMax);
    a.bio = rules::optionalText("bio", in.bio, rules::kTextMax);
    a.birthDate = in.birthDate;
    if (a.birthDate && *a.birthDate > toParts(clock_()).date)
        throw ValidationError("birthDate", "must not be in the future");
    return a;
}

Author AuthorService::create(const AuthorInput& input) {
    Author a = validated(input);
    a.createdAt = a.updatedAt = clock_();
    a.id = authors_->insert(a);
    return a;
}

Author AuthorService::update(AuthorId id, int expectedVersion, const AuthorInput& input) {
    Author a = validated(input);
    a.id = id;
    a.updatedAt = clock_();
    return tx_->inTransaction([&] {
        switch (authors_->update(a, expectedVersion)) {
            case UpdateResult::NotFound: throw NotFoundError("author", id.value);
            case UpdateResult::VersionConflict:
                throw ConflictError("version_conflict", "The author was changed by someone else; reload and retry");
            case UpdateResult::Updated: break;
        }
        return *authors_->findById(id);
    });
}

void AuthorService::remove(AuthorId id) {
    if (!authors_->remove(id)) throw NotFoundError("author", id.value);
}

Author AuthorService::get(AuthorId id) {
    auto a = authors_->findById(id);
    if (!a) throw NotFoundError("author", id.value);
    return *a;
}

Paged<Author> AuthorService::search(const std::optional<std::string>& nameContains, const Page& page) {
    const auto name = rules::optionalText("name", nameContains, rules::kSearchTextMax);
    const Page p = validatePage(page);
    return tx_->inTransaction(db::TransactionOptions::readOnlyTx(), [&] { return authors_->search(name, p); });
}

}  // namespace caelitus::catalog
