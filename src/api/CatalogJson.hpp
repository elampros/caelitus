#pragma once

/// @file
/// JSON representations of catalog types (private to the api module).
/// @ingroup api
///
/// camelCase names; absent optionals are null; ids are numbers; dates
/// "YYYY-MM-DD"; timestamps RFC 3339 UTC. Keep in sync with the schemas in
/// CatalogApi.cpp (the api tests check results against them).

#include "caelitus/catalog/domain/Model.hpp"
#include "caelitus/catalog/domain/Query.hpp"
#include "caelitus/catalog/domain/Reactions.hpp"
#include "caelitus/json/JsonTypes.hpp"

/// @cond INTERNAL
namespace caelitus::catalog {

template <typename Tag>
void to_json(Json& j, const Id<Tag>& id) {
    j = id.value;
}

inline void to_json(Json& j, const Category& c) { j = {{"id", c.id}, {"name", c.name}, {"slug", c.slug}}; }

inline void to_json(Json& j, const Author& a) {
    j = {{"id", a.id},
         {"name", a.name},
         {"bio", a.bio},
         {"birthDate", a.birthDate},
         {"version", a.version},
         {"createdAt", a.createdAt},
         {"updatedAt", a.updatedAt}};
}

inline void to_json(Json& j, const AuthorRef& a) { j = {{"id", a.id}, {"name", a.name}}; }

inline void to_json(Json& j, const TagUsage& t) { j = {{"name", t.tag.name}, {"bookCount", t.bookCount}}; }

inline void to_json(Json& j, const BookSummary& b) {
    j = {{"id", b.id},
         {"title", b.title},
         {"publishedOn", b.publishedOn},
         {"language", b.language},
         {"category", b.category},
         {"authors", b.authors},
         {"tags", b.tags},
         {"ratingCount", b.ratingCount},
         {"ratingAverage", b.ratingAverage},
         {"likes", b.likes},
         {"dislikes", b.dislikes},
         {"reactionsEnabled", b.reactionsEnabled}};
}

inline void to_json(Json& j, const BookDetails& b) {
    to_json(j, static_cast<const BookSummary&>(b));
    j["isbn"] = b.isbn;
    j["description"] = b.description;
    j["pageCount"] = b.pageCount;
    j["version"] = b.version;
    j["createdAt"] = b.createdAt;
    j["updatedAt"] = b.updatedAt;
}

inline void to_json(Json& j, const Review& r) {
    j = {{"id", r.id},       {"bookId", r.bookId}, {"reviewerName", r.reviewerName}, {"rating", r.rating},
         {"title", r.title}, {"body", r.body},     {"createdAt", r.createdAt},       {"updatedAt", r.updatedAt}};
}

inline void to_json(Json& j, const ReactionCounts& c) {
    j = {{"likes", c.likes}, {"dislikes", c.dislikes}, {"score", c.score()}};
}

inline void to_json(Json& j, const RankedBook& r) {
    j = {{"bookId", r.id},
         {"title", r.title},
         {"likes", r.counts.likes},
         {"dislikes", r.counts.dislikes},
         {"score", r.counts.score()}};
}

template <typename T>
void to_json(Json& j, const Paged<T>& p) {
    j = {{"items", p.items},
         {"total", p.total},
         {"page", p.page.number},
         {"pageSize", p.page.size},
         {"pageCount", p.pageCount()}};
}

}  // namespace caelitus::catalog
/// @endcond
