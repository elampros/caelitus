#pragma once

/// @file
/// Typed entity ids.
/// @ingroup catalog

#include <cstdint>
#include <functional>

namespace caelitus::catalog {

/// Strongly typed id: a BookId cannot be passed where an AuthorId is expected.
///
/// @tparam Tag  An empty type naming the entity; only used to make the types distinct.
template <typename Tag>
struct Id {
    std::int64_t value = 0;  ///< The database id; 0 for "not stored yet".

    constexpr Id() = default;
    /// Wraps a database id.
    constexpr explicit Id(std::int64_t v) : value(v) {}

    friend constexpr bool operator==(Id a, Id b) { return a.value == b.value; }
    friend constexpr bool operator!=(Id a, Id b) { return a.value != b.value; }
    friend constexpr bool operator<(Id a, Id b) { return a.value < b.value; }
};

using CategoryId = Id<struct CategoryTag>;  ///< Id of a Category.
using AuthorId = Id<struct AuthorTag>;      ///< Id of an Author.
using TagId = Id<struct TagTag>;            ///< Id of a Tag.
using BookId = Id<struct BookTag>;          ///< Id of a Book.
using ReviewId = Id<struct ReviewTag>;      ///< Id of a Review.

}  // namespace caelitus::catalog

/// Lets ids be keys of unordered containers.
template <typename Tag>
struct std::hash<caelitus::catalog::Id<Tag>> {
    /// Hash of the id's value.
    std::size_t operator()(caelitus::catalog::Id<Tag> id) const noexcept { return std::hash<std::int64_t>{}(id.value); }
};
