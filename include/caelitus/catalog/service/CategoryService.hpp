#pragma once

/// @file
/// Use cases for categories.
/// @ingroup catalog

#include "caelitus/catalog/domain/Repositories.hpp"
#include "caelitus/catalog/service/ServiceSupport.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace caelitus::catalog {

/// Use cases for categories: create, rename, delete, list.
///
/// Categories are flat (no hierarchy) and small in number, so they are not
/// paged.
class CategoryService {
public:
    /// @param categories  Storage.
    explicit CategoryService(std::shared_ptr<ICategoryRepository> categories);

    /// Creates a category.
    ///
    /// Without a slug, one is derived from the name ("Science Fiction" ->
    /// "science-fiction"); names without latin letters/digits need one.
    /// @throws ValidationError, ConflictError ("name_taken", "slug_taken").
    Category create(const std::string& name, const std::optional<std::string>& slug = std::nullopt);

    /// Renames a category. A missing slug keeps the current one.
    /// @throws NotFoundError, ValidationError, ConflictError ("name_taken", "slug_taken").
    Category update(CategoryId id, const std::string& name, const std::optional<std::string>& slug = std::nullopt);

    /// Deletes a category that no book uses (hard delete).
    /// @throws NotFoundError, ConflictError ("category_in_use").
    void remove(CategoryId id);

    /// One category.
    /// @throws NotFoundError
    Category get(CategoryId id);
    /// Every category, by name.
    std::vector<Category> list();

private:
    std::shared_ptr<ICategoryRepository> categories_;
};

}  // namespace caelitus::catalog
