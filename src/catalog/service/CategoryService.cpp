#include "caelitus/catalog/service/CategoryService.hpp"

#include "caelitus/catalog/domain/Rules.hpp"

namespace caelitus::catalog {

namespace {

std::string resolveSlug(const std::string& name, const std::optional<std::string>& slug) {
    if (slug) return rules::validateSlug(*slug);
    std::string derived = rules::slugFromName(name);
    if (derived.empty())
        throw ValidationError("slug", "cannot be derived from a name without latin letters or digits; give one");
    return derived;
}

}  // namespace

CategoryService::CategoryService(std::shared_ptr<ICategoryRepository> categories) : categories_(std::move(categories)) {
    if (!categories_) throw std::invalid_argument("CategoryService: repository is null");
}

Category CategoryService::create(const std::string& nameIn, const std::optional<std::string>& slug) {
    Category c;
    c.name = rules::requiredText("name", nameIn, rules::kCategoryNameMax);
    c.slug = resolveSlug(c.name, slug);
    c.id = categories_->insert(c);
    return c;
}

Category CategoryService::update(CategoryId id, const std::string& nameIn, const std::optional<std::string>& slug) {
    Category c = get(id);
    c.name = rules::requiredText("name", nameIn, rules::kCategoryNameMax);
    if (slug) c.slug = rules::validateSlug(*slug);
    if (!categories_->update(c)) throw NotFoundError("category", id.value);
    return c;
}

void CategoryService::remove(CategoryId id) {
    if (!categories_->remove(id)) throw NotFoundError("category", id.value);
}

Category CategoryService::get(CategoryId id) {
    auto c = categories_->findById(id);
    if (!c) throw NotFoundError("category", id.value);
    return *c;
}

std::vector<Category> CategoryService::list() { return categories_->listAll(); }

}  // namespace caelitus::catalog
