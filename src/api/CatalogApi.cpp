/// @file
/// The catalog's JSON-RPC methods: parameter schemas, result schemas and the
/// handlers that call the catalog services (api::registerCatalogApi()).
/// @ingroup api

#include "caelitus/api/CatalogApi.hpp"

#include "api/CatalogJson.hpp"
#include "caelitus/Version.hpp"
#include "caelitus/api/Schema.hpp"
#include "caelitus/catalog/domain/Rules.hpp"

namespace caelitus::api {

using namespace caelitus::catalog;
namespace S = schema;

namespace {

// ---- parameter helpers -------------------------------------------------------

template <typename IdT>
IdT idParam(const Params& p, const char* name) {
    return IdT(p.required<std::int64_t>(name));  // the schema guarantees >= 1
}

template <typename IdT>
std::optional<IdT> optionalId(const Params& p, const char* name) {
    auto v = p.optional<std::int64_t>(name);
    return v ? std::optional<IdT>(IdT(*v)) : std::nullopt;
}

Page pageParams(const Params& p) { return {p.value<int>("page", 1), p.value<int>("pageSize", 20)}; }

template <typename E, std::size_t N>
E enumParam(const Params& p, const char* name, const std::pair<const char*, E> (&values)[N], E fallback) {
    auto text = p.optional<std::string>(name);
    if (!text) return fallback;
    for (const auto& [label, value] : values)
        if (*text == label) return value;
    throw InvalidParams(name, "unknown value");  // unreachable: the schema lists the labels
}

template <typename E, std::size_t N>
std::vector<std::string> labels(const std::pair<const char*, E> (&values)[N]) {
    std::vector<std::string> out;
    for (const auto& [label, value] : values) out.emplace_back(label);
    return out;
}

constexpr std::pair<const char*, BookSort> kSorts[] = {{"publishedDesc", BookSort::PublishedDesc},
                                                       {"publishedAsc", BookSort::PublishedAsc},
                                                       {"titleAsc", BookSort::TitleAsc},
                                                       {"ratingDesc", BookSort::RatingDesc},
                                                       {"createdDesc", BookSort::CreatedDesc}};
constexpr std::pair<const char*, TagMatch> kTagMatches[] = {{"any", TagMatch::Any}, {"all", TagMatch::All}};
constexpr std::pair<const char*, ReactionOrder> kOrders[] = {{"mostLiked", ReactionOrder::MostLiked},
                                                             {"mostDisliked", ReactionOrder::MostDisliked}};
const std::vector<std::string> kPeriods = {"today", "yesterday", "last7Days", "last30Days", "lastYear", "allTime"};

BookInput bookInput(const Params& p) {
    BookInput in;
    in.title = p.required<std::string>("title");
    in.isbn = p.optional<std::string>("isbn");
    in.description = p.optional<std::string>("description");
    in.publishedOn = p.required<Date>("publishedOn");
    in.language = p.required<std::string>("language");
    in.pageCount = p.optional<int>("pageCount");
    in.category = idParam<CategoryId>(p, "categoryId");
    for (auto id : p.required<std::vector<std::int64_t>>("authorIds")) in.authors.push_back(AuthorId(id));
    in.tags = p.value<std::vector<std::string>>("tags", {});
    in.reactionsEnabled = p.value<bool>("reactionsEnabled", false);
    return in;
}

AuthorInput authorInput(const Params& p) {
    return {p.required<std::string>("name"), p.optional<std::string>("bio"), p.optional<Date>("birthDate")};
}

ReviewInput reviewInput(const Params& p) {
    return {p.required<std::string>("reviewerName"), p.required<int>("rating"), p.optional<std::string>("title"),
            p.required<std::string>("body")};
}

// ---- shared parameter declarations -------------------------------------------

MethodBuilder& paging(MethodBuilder& m) {
    return m.optional("page", S::integer(1), "Page number, from 1 (default 1)")
        .optional("pageSize", S::integer(1, Page::kMaxSize), "Items per page, 1-100 (default 20)");
}

MethodBuilder& bookFields(MethodBuilder& m) {
    return m.required("title", S::string(1, rules::kTitleMax), "Title")
        .optional("isbn", S::string(1, 20), "ISBN-10 or ISBN-13; hyphens and spaces allowed; stored as ISBN-13")
        .optional("description", S::string(0, rules::kTextMax), "Free text")
        .required("publishedOn", S::date(), "Publication date")
        .required("language", S::string(2, 2), "ISO 639-1 language code, e.g. \"el\", \"en\"")
        .optional("pageCount", S::integer(1, 100000), "Number of pages")
        .required("categoryId", S::id(), "The book's category")
        .required(
            "authorIds",
            [] {
                Json a = S::array(S::id(), rules::kMaxAuthorsPerBook, 1);
                a["uniqueItems"] = true;
                return a;
            }(),
            "Authors in cover order")
        .optional("tags", S::array(S::string(1, rules::kTagMax), rules::kMaxTagsPerBook),
                  "Free-text tags; normalized (trimmed, lower case); unknown tags are created");
}

MethodBuilder& reviewFields(MethodBuilder& m) {
    return m.required("reviewerName", S::string(1, rules::kReviewerMax), "Who wrote the review")
        .required("rating", S::integer(1, 5), "1 (worst) to 5 (best)")
        .optional("title", S::string(0, rules::kReviewTitleMax), "Optional headline")
        .required("body", S::string(1, rules::kTextMax), "The review text");
}

// ---- schemas -----------------------------------------------------------------

S::Properties bookSummaryProperties() {
    return {{"id", S::id()},
            {"title", S::string()},
            {"publishedOn", S::date()},
            {"language", S::string(2, 2)},
            {"category", S::ref("Category")},
            {"authors", S::array(S::ref("AuthorRef"))},
            {"tags", S::array(S::string())},
            {"ratingCount", S::integer(0)},
            {"ratingAverage", S::nullable(S::number(1, 5))},
            {"likes", S::integer(0)},
            {"dislikes", S::integer(0)},
            {"reactionsEnabled", S::describe(S::boolean(), "Whether likes/dislikes are accepted over MQTT")}};
}

Json page(const std::string& item, const std::string& description) {
    return S::describe(S::object({{"items", S::array(S::ref(item))},
                                  {"total", S::describe(S::integer(0), "Matching items across all pages")},
                                  {"page", S::integer(1)},
                                  {"pageSize", S::integer(1, Page::kMaxSize)},
                                  {"pageCount", S::integer(0)}}),
                       description);
}

void registerSchemas(JsonRpcHandler& rpc) {
    rpc.addSchema("Category",
                  S::describe(S::object({{"id", S::id()},
                                         {"name", S::string(1, rules::kCategoryNameMax)},
                                         {"slug", S::describe(S::string(1, rules::kSlugMax), "a-z, 0-9 and '-'")}}),
                              "A book category (flat: one per book)"));
    rpc.addSchema("AuthorRef", S::describe(S::object({{"id", S::id()}, {"name", S::string()}}),
                                           "An author as listed in a book, in cover order"));
    rpc.addSchema("Author",
                  S::describe(S::object({{"id", S::id()},
                                         {"name", S::string()},
                                         {"bio", S::nullable(S::string())},
                                         {"birthDate", S::nullable(S::date())},
                                         {"version", S::describe(S::integer(1), "Pass it back to authors.update")},
                                         {"createdAt", S::dateTime()},
                                         {"updatedAt", S::dateTime()}}),
                              "An author with all their details"));
    rpc.addSchema("BookSummary", S::describe(S::object(bookSummaryProperties()), "A book as listed in search results"));
    auto book = bookSummaryProperties();
    book.insert(book.end(), {{"isbn", S::nullable(S::describe(S::string(13, 13), "ISBN-13"))},
                             {"description", S::nullable(S::string())},
                             {"pageCount", S::nullable(S::integer(1))},
                             {"version", S::describe(S::integer(1), "Pass it back to books.update")},
                             {"createdAt", S::dateTime()},
                             {"updatedAt", S::dateTime()}});
    rpc.addSchema("Book", S::describe(S::object(book), "A book with all its details"));
    rpc.addSchema("Review", S::describe(S::object({{"id", S::id()},
                                                   {"bookId", S::id()},
                                                   {"reviewerName", S::string()},
                                                   {"rating", S::integer(1, 5)},
                                                   {"title", S::nullable(S::string())},
                                                   {"body", S::string()},
                                                   {"createdAt", S::dateTime()},
                                                   {"updatedAt", S::dateTime()}}),
                                        "A reader's review of a book, with a rating of 1-5"));
    rpc.addSchema("TagUsage", S::describe(S::object({{"name", S::string()}, {"bookCount", S::integer(1)}}),
                                          "A tag and how many books carry it"));
    rpc.addSchema("ReactionCounts", S::describe(S::object({{"likes", S::integer(0)},
                                                           {"dislikes", S::integer(0)},
                                                           {"score", S::describe(S::integer(), "likes - dislikes")}}),
                                                "Likes and dislikes of one book in one period"));
    S::Properties periods;
    for (const auto& p : kPeriods) periods.emplace_back(p, S::ref("ReactionCounts"));
    rpc.addSchema("ReactionStats",
                  S::describe(S::object({{"bookId", S::id()}, {"periods", S::object(periods)}}),
                              "A book's likes and dislikes for every period (days in the catalog time zone)"));
    rpc.addSchema("RankedBook", S::describe(S::object({{"bookId", S::id()},
                                                       {"title", S::string()},
                                                       {"likes", S::integer(0)},
                                                       {"dislikes", S::integer(0)},
                                                       {"score", S::integer()}}),
                                            "One book of a ranking"));
    rpc.addSchema("TopBooks",
                  S::describe(S::object({{"period", S::enumOf(kPeriods)},
                                         {"from", S::describe(S::nullable(S::date()), "First day; null for allTime")},
                                         {"to", S::describe(S::nullable(S::date()), "Last day; null for allTime")},
                                         {"items", S::array(S::ref("RankedBook"))}}),
                              "The most liked (or disliked) books of a period, best first"));
    rpc.addSchema("BookPage", page("BookSummary", "One page of book search results"));
    rpc.addSchema("AuthorPage", page("Author", "One page of authors"));
    rpc.addSchema("ReviewPage", page("Review", "One page of a book's reviews, newest first"));
}

const Json kDeleted = S::describe(S::constant(true), "Always true");

}  // namespace

ApiInfo catalogApiInfo() {
    return {"Caelitus Catalog API", kVersion,
            "Books, authors, categories, tags, reviews, and likes/dislikes. Likes and dislikes arrive over MQTT "
            "(<prefix>/<bookId>/like or /dislike, default prefix catalog/in/books) for books with reactionsEnabled; "
            "this API reads the results. The catalog publishes events to catalog/books/<id>/created, updated, "
            "deleted and reviews."};
}

void registerCatalogApi(JsonRpcHandler& rpc, const CatalogServices& s) {
    registerSchemas(rpc);

    // ---- system ----
    MethodBuilder(rpc, "system.ping", "system", "Liveness check.")
        .returns("pong", S::object({{"time", S::describe(S::dateTime(), "Server time")}}))
        .handler([](const Params&) { return Json{{"time", nowUtc()}}; });

    // ---- categories ----
    MethodBuilder(rpc, "categories.list", "categories", "All categories, by name.")
        .returns("categories", S::array(S::ref("Category")))
        .handler([s](const Params&) { return Json(s.categories->list()); });
    MethodBuilder(rpc, "categories.get", "categories", "One category.")
        .required("id", S::id(), "Category id")
        .returns("category", S::ref("Category"))
        .errors({"NotFound"})
        .handler([s](const Params& p) { return Json(s.categories->get(idParam<CategoryId>(p, "id"))); });
    MethodBuilder(rpc, "categories.create", "categories", "Creates a category.")
        .required("name", S::string(1, rules::kCategoryNameMax), "Unique name")
        .optional("slug", S::string(1, rules::kSlugMax),
                  "Unique URL-friendly id (a-z, 0-9, '-'); derived from the name if omitted, which needs a name "
                  "with latin letters or digits")
        .returns("category", S::ref("Category"))
        .conflicts({"name_taken", "slug_taken"})
        .handler([s](const Params& p) {
            return Json(s.categories->create(p.required<std::string>("name"), p.optional<std::string>("slug")));
        });
    MethodBuilder(rpc, "categories.update", "categories", "Renames a category.")
        .required("id", S::id(), "Category id")
        .required("name", S::string(1, rules::kCategoryNameMax), "New name")
        .optional("slug", S::string(1, rules::kSlugMax), "New slug; kept if omitted")
        .returns("category", S::ref("Category"))
        .errors({"NotFound"})
        .conflicts({"name_taken", "slug_taken"})
        .handler([s](const Params& p) {
            return Json(s.categories->update(idParam<CategoryId>(p, "id"), p.required<std::string>("name"),
                                             p.optional<std::string>("slug")));
        });
    MethodBuilder(rpc, "categories.delete", "categories", "Deletes a category that has no books.")
        .required("id", S::id(), "Category id")
        .returns("deleted", kDeleted)
        .errors({"NotFound"})
        .conflicts({"category_in_use"})
        .handler([s](const Params& p) {
            s.categories->remove(idParam<CategoryId>(p, "id"));
            return Json(true);
        });

    // ---- authors ----
    {
        MethodBuilder m(rpc, "authors.search", "authors", "Authors by name, alphabetically.");
        m.optional("name", S::string(1, rules::kSearchTextMax), "Part of the name (case-insensitive)");
        paging(m).returns("authors", S::ref("AuthorPage")).handler([s](const Params& p) {
            return Json(s.authors->search(p.optional<std::string>("name"), pageParams(p)));
        });
    }
    MethodBuilder(rpc, "authors.get", "authors", "One author.")
        .required("id", S::id(), "Author id")
        .returns("author", S::ref("Author"))
        .errors({"NotFound"})
        .handler([s](const Params& p) { return Json(s.authors->get(idParam<AuthorId>(p, "id"))); });
    MethodBuilder(rpc, "authors.create", "authors", "Creates an author.")
        .required("name", S::string(1, rules::kNameMax), "Full name")
        .optional("bio", S::string(0, rules::kTextMax), "Biography")
        .optional("birthDate", S::date(), "Not in the future")
        .returns("author", S::ref("Author"))
        .handler([s](const Params& p) { return Json(s.authors->create(authorInput(p))); });
    MethodBuilder(rpc, "authors.update", "authors", "Replaces an author's fields.")
        .description(
            "Optimistic locking: pass the version you last read; if someone changed the author since, "
            "the call fails with version_conflict and nothing is changed.")
        .required("id", S::id(), "Author id")
        .required("version", S::integer(1), "The version you last read")
        .required("name", S::string(1, rules::kNameMax), "Full name")
        .optional("bio", S::string(0, rules::kTextMax), "Biography; omitted or null clears it")
        .optional("birthDate", S::date(), "Omitted or null clears it")
        .returns("author", S::ref("Author"))
        .errors({"NotFound"})
        .conflicts({"version_conflict"})
        .handler([s](const Params& p) {
            return Json(s.authors->update(idParam<AuthorId>(p, "id"), p.required<int>("version"), authorInput(p)));
        });
    MethodBuilder(rpc, "authors.delete", "authors", "Deletes an author who has no books.")
        .required("id", S::id(), "Author id")
        .returns("deleted", kDeleted)
        .errors({"NotFound"})
        .conflicts({"author_has_books"})
        .handler([s](const Params& p) {
            s.authors->remove(idParam<AuthorId>(p, "id"));
            return Json(true);
        });

    // ---- books ----
    {
        MethodBuilder m(rpc, "books.search", "books", "Searches books.");
        m.description("All filters are optional and combine with AND.")
            .optional("categoryId", S::id(), "Only this category")
            .optional("authorId", S::id(), "Only books by this author")
            .optional("tags", S::array(S::string(1, rules::kTagMax), rules::kMaxTagsPerBook), "Tag names")
            .optional("tagMatch", S::enumOf(labels(kTagMatches)), "any (default): at least one tag; all: every tag")
            .optional("publishedFrom", S::date(), "Published on or after")
            .optional("publishedTo", S::date(), "Published on or before")
            .optional("title", S::string(1, rules::kSearchTextMax), "Part of the title (case-insensitive)")
            .optional("minRating", S::number(1, 5), "Minimum average rating; unrated books are excluded")
            .optional("language", S::string(2, 2), "ISO 639-1 code")
            .optional("sort", S::enumOf(labels(kSorts)), "Order (default publishedDesc); ratingDesc puts unrated last");
        paging(m).returns("books", S::ref("BookPage")).handler([s](const Params& p) {
            BookQuery q;
            q.category = optionalId<CategoryId>(p, "categoryId");
            q.author = optionalId<AuthorId>(p, "authorId");
            q.tags = p.value<std::vector<std::string>>("tags", {});
            q.tagMatch = enumParam(p, "tagMatch", kTagMatches, TagMatch::Any);
            q.publishedFrom = p.optional<Date>("publishedFrom");
            q.publishedTo = p.optional<Date>("publishedTo");
            q.titleContains = p.optional<std::string>("title");
            q.minRating = p.optional<double>("minRating");
            q.language = p.optional<std::string>("language");
            q.sort = enumParam(p, "sort", kSorts, BookSort::PublishedDesc);
            q.page = pageParams(p);
            return Json(s.books->search(q));
        });
    }
    MethodBuilder(rpc, "books.get", "books", "One book with all its details.")
        .required("id", S::id(), "Book id")
        .returns("book", S::ref("Book"))
        .errors({"NotFound"})
        .handler([s](const Params& p) { return Json(s.books->get(idParam<BookId>(p, "id"))); });
    {
        MethodBuilder m(rpc, "books.create", "books", "Creates a book.");
        bookFields(m)
            .optional("reactionsEnabled", S::boolean(), "Accept likes/dislikes over MQTT (default false)")
            .returns("book", S::ref("Book"))
            .errors({})
            .conflicts({"isbn_taken", "stale_reference"})
            .handler([s](const Params& p) { return Json(s.books->create(bookInput(p))); });
    }
    {
        MethodBuilder m(rpc, "books.update", "books", "Replaces a book's fields, authors and tags.");
        m.description(
             "Optimistic locking: pass the version you last read; if someone changed the book since, the "
             "call fails with version_conflict and nothing is changed. Does not change reactionsEnabled "
             "(see books.setReactionsEnabled).")
            .required("id", S::id(), "Book id")
            .required("version", S::integer(1), "The version you last read");
        bookFields(m)
            .returns("book", S::ref("Book"))
            .errors({"NotFound"})
            .conflicts({"version_conflict", "isbn_taken", "stale_reference"})
            .handler([s](const Params& p) {
                return Json(s.books->update(idParam<BookId>(p, "id"), p.required<int>("version"), bookInput(p)));
            });
    }
    MethodBuilder(rpc, "books.setReactionsEnabled", "books", "Turns likes/dislikes over MQTT on or off for a book.")
        .description("Takes effect immediately. A switch, not an edit: the book's version does not change.")
        .required("id", S::id(), "Book id")
        .required("enabled", S::boolean(), "true to accept likes/dislikes")
        .returns("book", S::ref("Book"))
        .errors({"NotFound"})
        .handler([s](const Params& p) {
            return Json(s.books->setReactionsEnabled(idParam<BookId>(p, "id"), p.required<bool>("enabled")));
        });
    MethodBuilder(rpc, "books.delete", "books", "Deletes a book with its reviews and reactions.")
        .required("id", S::id(), "Book id")
        .returns("deleted", kDeleted)
        .errors({"NotFound"})
        .handler([s](const Params& p) {
            s.books->remove(idParam<BookId>(p, "id"));
            return Json(true);
        });
    MethodBuilder(rpc, "tags.list", "books", "Tags in use, by name, with how many books carry each.")
        .returns("tags", S::array(S::ref("TagUsage")))
        .handler([s](const Params&) { return Json(s.books->tags()); });

    // ---- reviews ----
    {
        MethodBuilder m(rpc, "reviews.list", "reviews", "A book's reviews, newest first.");
        m.required("bookId", S::id(), "Book id");
        paging(m).returns("reviews", S::ref("ReviewPage")).errors({"NotFound"}).handler([s](const Params& p) {
            return Json(s.reviews->listForBook(idParam<BookId>(p, "bookId"), pageParams(p)));
        });
    }
    MethodBuilder(rpc, "reviews.get", "reviews", "One review.")
        .required("id", S::id(), "Review id")
        .returns("review", S::ref("Review"))
        .errors({"NotFound"})
        .handler([s](const Params& p) { return Json(s.reviews->get(idParam<ReviewId>(p, "id"))); });
    {
        MethodBuilder m(rpc, "reviews.create", "reviews", "Adds a review and updates the book's rating.");
        m.required("bookId", S::id(), "Book id");
        reviewFields(m).returns("review", S::ref("Review")).errors({"NotFound"}).handler([s](const Params& p) {
            return Json(s.reviews->add(idParam<BookId>(p, "bookId"), reviewInput(p)));
        });
    }
    {
        MethodBuilder m(rpc, "reviews.update", "reviews", "Replaces a review's fields and updates the book's rating.");
        m.required("id", S::id(), "Review id");
        reviewFields(m).returns("review", S::ref("Review")).errors({"NotFound"}).handler([s](const Params& p) {
            return Json(s.reviews->update(idParam<ReviewId>(p, "id"), reviewInput(p)));
        });
    }
    MethodBuilder(rpc, "reviews.delete", "reviews", "Deletes a review and updates the book's rating.")
        .required("id", S::id(), "Review id")
        .returns("deleted", kDeleted)
        .errors({"NotFound"})
        .handler([s](const Params& p) {
            s.reviews->remove(idParam<ReviewId>(p, "id"));
            return Json(true);
        });

    // ---- reactions ----
    MethodBuilder(rpc, "reactions.get", "reactions", "A book's likes and dislikes for every period.")
        .description(
            "Periods are rolling windows ending today, in the catalog's time zone (default Europe/Athens). "
            "Counts lag by up to one flush interval (default 1 s).")
        .required("bookId", S::id(), "Book id")
        .returns("stats", S::ref("ReactionStats"))
        .errors({"NotFound"})
        .handler([s](const Params& p) {
            const auto stats = s.reactions->stats(idParam<BookId>(p, "bookId"));
            Json periods = Json::object();
            for (const auto& [period, counts] : stats.periods) periods[toString(period)] = counts;
            return Json{{"bookId", stats.book}, {"periods", periods}};
        });
    MethodBuilder(rpc, "reactions.top", "reactions", "Most liked (or disliked) books in a period.")
        .description(
            "Ranked by likes minus dislikes. mostLiked lists books with at least one like in the period; "
            "mostDisliked, books with at least one dislike.")
        .required("period", S::enumOf(kPeriods), "Time window")
        .optional("order", S::enumOf(labels(kOrders)), "mostLiked (default) or mostDisliked")
        .optional("limit", S::integer(1, Page::kMaxSize), "How many books, 1-100 (default 10)")
        .returns("top", S::ref("TopBooks"))
        .handler([s](const Params& p) {
            const std::string periodText = p.required<std::string>("period");
            const Period period = *parsePeriod(periodText);  // the schema lists the valid names
            const auto range = s.reactions->range(period);
            return Json{{"period", periodText},
                        {"from", range ? Json(range->from) : Json(nullptr)},
                        {"to", range ? Json(range->to) : Json(nullptr)},
                        {"items", s.reactions->top(period, enumParam(p, "order", kOrders, ReactionOrder::MostLiked),
                                                   p.value<int>("limit", 10))}};
        });

    addDiscover(rpc, catalogApiInfo());
}

}  // namespace caelitus::api
