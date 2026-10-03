// Tests for the JSON-RPC layer and the catalog API (over in-memory fakes),
// and for the MQTT reaction listener.

#include "CatalogFakes.hpp"
#include "TestHarness.hpp"

#include "caelitus/api/CatalogApi.hpp"
#include "caelitus/api/JsonRpc.hpp"
#include "caelitus/api/MqttReactionListener.hpp"
#include "caelitus/api/OpenRpc.hpp"
#include "caelitus/api/OperationsApi.hpp"
#include "caelitus/api/Schema.hpp"
#include "caelitus/api/SchemaValidator.hpp"
#include "caelitus/mqtt/Topic.hpp"

#include <atomic>
#include <fstream>
#include <set>
#include <thread>

using namespace caelitus;
using namespace caelitus::api;
using namespace fakes;

namespace {

const net::ConnectionInfo kConn{1, "127.0.0.1", 5000};

// Sends raw text, returns the parsed reply (null if none).
Json call(JsonRpcHandler& rpc, const std::string& text) {
    auto reply = rpc.handle(text, kConn);
    return reply ? Json::parse(*reply) : Json(nullptr);
}

Json request(const std::string& method, Json params = Json::object(), Json id = 1) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

struct Api {
    Store store;
    std::shared_ptr<FakeTx> tx = std::make_shared<FakeTx>(store);
    std::shared_ptr<FakeCategories> categoryRepo = std::make_shared<FakeCategories>(store);
    std::shared_ptr<FakeAuthors> authorRepo = std::make_shared<FakeAuthors>(store);
    std::shared_ptr<FakeTags> tagRepo = std::make_shared<FakeTags>(store);
    std::shared_ptr<FakeBooks> bookRepo = std::make_shared<FakeBooks>(store);
    std::shared_ptr<FakeReviews> reviewRepo = std::make_shared<FakeReviews>(store);
    std::shared_ptr<FakeReactions> reactionRepo = std::make_shared<FakeReactions>(store);
    std::shared_ptr<BookCache> bookCache = std::make_shared<BookCache>(bookRepo);
    CatalogServices services{
        std::make_shared<CategoryService>(categoryRepo),
        std::make_shared<AuthorService>(authorRepo, tx),
        std::make_shared<BookService>(bookRepo, authorRepo, categoryRepo, tagRepo, tx, nullptr, systemClock(),
                                      bookCache),
        std::make_shared<ReviewService>(reviewRepo, bookRepo, tx),
        std::make_shared<ReactionService>(reactionRepo, bookRepo, bookCache, tx, TimeZone::utc()),
    };
    JsonRpcHandler rpc;
    std::shared_ptr<scheduler::Scheduler> jobs = std::make_shared<scheduler::Scheduler>();
    std::optional<HealthReport> health;

    Api() {
        registerCatalogApi(rpc, services);
        registerOperationsApi(rpc, jobs, [this] { return health; });
    }

    // The result of a successful call, checked against the method's declared
    // result schema, so the JSON the API returns cannot drift from its
    // OpenRPC description.
    Json result(const std::string& method, Json params = Json::object()) {
        Json reply = call(rpc, request(method, std::move(params)).dump());
        if (reply.contains("error")) throw test::Failure{method + " failed: " + reply.dump()};
        const Json& r = reply.at("result");
        if (auto problem = SchemaValidator(rpc.schemas()).check(r, rpc.methods().at(method).result, "result"))
            throw test::Failure{method + " returned JSON that does not match its result schema: " + *problem};
        return r;
    }
    Json error(const std::string& method, Json params = Json::object()) {
        Json reply = call(rpc, request(method, std::move(params)).dump());
        if (!reply.contains("error")) throw test::Failure{method + " unexpectedly succeeded: " + reply.dump()};
        return reply.at("error");
    }

    // A category, an author and a book; returns the book id.
    std::int64_t seedBook(const std::string& title = "Foundation") {
        auto cat = store.categories.empty() ? result("categories.create", {{"name", "Science Fiction"}})["id"]
                                            : Json(store.categories.begin()->first);
        auto author = store.authors.empty() ? result("authors.create", {{"name", "Isaac Asimov"}})["id"]
                                            : Json(store.authors.begin()->first);
        return result("books.create", {{"title", title},
                                       {"publishedOn", "1951-06-01"},
                                       {"language", "en"},
                                       {"categoryId", cat},
                                       {"authorIds", {author}},
                                       {"tags", {"Classic"}},
                                       {"reactionsEnabled", true}})["id"]
            .get<std::int64_t>();
    }
};

}  // namespace

// ---- Protocol ----------------------------------------------------------------

TEST(protocol_errors) {
    Api a;
    CHECK_EQ(call(a.rpc, "{not json")["error"]["code"], errors::kParseError);
    CHECK(call(a.rpc, "{not json")["id"].is_null());
    CHECK_EQ(call(a.rpc, "42")["error"]["code"], errors::kInvalidRequest);
    CHECK_EQ(call(a.rpc, R"({"id":1,"method":"system.ping"})")["error"]["code"],
             errors::kInvalidRequest);  // no jsonrpc
    CHECK_EQ(call(a.rpc, R"({"jsonrpc":"1.0","id":1,"method":"system.ping"})")["error"]["code"],
             errors::kInvalidRequest);
    CHECK_EQ(call(a.rpc, R"({"jsonrpc":2,"id":1,"method":"system.ping"})")["error"]["code"], errors::kInvalidRequest);
    CHECK_EQ(call(a.rpc, R"({"jsonrpc":"2.0","id":{},"method":"system.ping"})")["error"]["code"],
             errors::kInvalidRequest);
    CHECK_EQ(call(a.rpc, R"({"jsonrpc":"2.0","id":1,"method":5})")["error"]["code"], errors::kInvalidRequest);
    CHECK_EQ(call(a.rpc, R"({"jsonrpc":"2.0","id":1,"method":"nope"})")["error"]["code"], errors::kMethodNotFound);
    CHECK_EQ(call(a.rpc, R"({"jsonrpc":"2.0","id":1,"method":"system.ping","params":[1]})")["error"]["code"],
             errors::kInvalidParams);
    CHECK_EQ(call(a.rpc, "[]")["error"]["code"], errors::kInvalidRequest);
}

TEST(ids_are_echoed_and_notifications_get_no_reply) {
    Api a;
    CHECK_EQ(call(a.rpc, request("system.ping", Json::object(), "abc-1").dump())["id"], "abc-1");
    CHECK_EQ(call(a.rpc, request("system.ping", Json::object(), 7).dump())["id"], 7);
    CHECK(call(a.rpc, R"({"jsonrpc":"2.0","method":"system.ping"})").is_null());
    CHECK(call(a.rpc, R"({"jsonrpc":"2.0","method":"categories.create","params":{"name":"Poetry"}})").is_null());
    CHECK_EQ(a.store.categories.size(), 1u);  // the notification still ran
}

TEST(batches) {
    Api a;
    Json batch = Json::array({request("system.ping", Json::object(), 1),
                              request("nope", Json::object(), 2),
                              {{"jsonrpc", "2.0"}, {"method", "system.ping"}}});  // notification
    Json replies = call(a.rpc, batch.dump());
    CHECK(replies.is_array());
    CHECK_EQ(replies.size(), 2u);
    CHECK(replies[0].contains("result"));
    CHECK_EQ(replies[1]["error"]["code"], errors::kMethodNotFound);
    CHECK(call(a.rpc, Json::array({{{"jsonrpc", "2.0"}, {"method", "system.ping"}}}).dump()).is_null());
}

TEST(params_are_checked_before_the_method_runs) {
    Api a;
    auto e = a.error("categories.create", {{"name", "Poetry"}, {"slgu", "poetry"}});
    CHECK_EQ(e["code"], errors::kInvalidParams);
    CHECK_EQ(e["data"]["field"], "slgu");
    CHECK(a.store.categories.empty());  // nothing created

    CHECK_EQ(a.error("categories.get", {{"id", "1"}})["data"]["field"], "id");
    CHECK_EQ(a.error("categories.get", {{"id", -1}})["data"]["field"], "id");
    CHECK_EQ(a.error("categories.get", Json::object())["data"]["field"], "id");
    CHECK_EQ(a.error("books.search", {{"publishedFrom", "2020-13-01"}})["data"]["field"], "publishedFrom");
    CHECK_EQ(a.error("books.search", {{"sort", "random"}})["data"]["field"], "sort");
    auto tagError = a.error("books.search", {{"tags", {"ok", 3}}})["data"];
    CHECK_EQ(tagError["field"], "tags");
    CHECK_EQ(tagError["reason"], "tags[1]: must be a string");
}

TEST(domain_errors_map_to_codes) {
    Api a;
    a.seedBook();
    // Passes the schema, rejected by a business rule (the category must exist).
    auto validation = a.error("books.create", {{"title", "X"},
                                               {"publishedOn", "2000-01-01"},
                                               {"language", "en"},
                                               {"categoryId", 999},
                                               {"authorIds", {2}}});
    CHECK_EQ(validation["code"], errors::kInvalidParams);
    CHECK_EQ(validation["data"]["code"], "validation_failed");
    CHECK_EQ(validation["data"]["field"], "category");

    auto notFound = a.error("books.get", {{"id", 999}});
    CHECK_EQ(notFound["code"], errors::kNotFound);
    CHECK_EQ(notFound["data"]["entity"], "book");

    auto conflict = a.error("authors.delete", {{"id", 2}});
    CHECK_EQ(conflict["code"], errors::kConflict);
    CHECK_EQ(conflict["data"]["code"], "author_has_books");
}

TEST(internal_errors_hide_details) {
    JsonRpcHandler rpc;
    MethodBuilder(rpc, "boom", "test", "").handler([](const Params&) -> Json {
        throw std::runtime_error("secret connection string");
    });
    MethodBuilder(rpc, "flaky", "test", "").handler([](const Params&) -> Json {
        throw db::PoolTimeoutError("pool exhausted");
    });
    Json e = call(rpc, request("boom").dump())["error"];
    CHECK_EQ(e["code"], errors::kInternalError);
    CHECK(e.dump().find("secret") == std::string::npos);
    CHECK_EQ(call(rpc, request("flaky").dump())["error"]["code"], errors::kUnavailable);
}

// ---- Catalog methods ---------------------------------------------------------

TEST(book_lifecycle_over_json) {
    Api a;
    const auto id = a.seedBook();
    Json book = a.result("books.get", {{"id", id}});
    CHECK_EQ(book["title"], "Foundation");
    CHECK_EQ(book["publishedOn"], "1951-06-01");
    CHECK_EQ(book["tags"], Json::array({"classic"}));
    CHECK_EQ(book["authors"][0]["name"], "Isaac Asimov");
    CHECK(book["isbn"].is_null());
    CHECK(book["ratingAverage"].is_null());
    CHECK_EQ(book["reactionsEnabled"], true);
    CHECK_EQ(a.result("books.setReactionsEnabled", {{"id", id}, {"enabled", false}})["reactionsEnabled"], false);
    CHECK_EQ(a.result("books.get", {{"id", id}})["version"], 1);  // a switch, not a versioned edit
    a.result("books.setReactionsEnabled", {{"id", id}, {"enabled", true}});
    CHECK_EQ(book["version"], 1);

    Json updated = a.result("books.update", {{"id", id},
                                             {"version", 1},
                                             {"title", "Foundation (1951)"},
                                             {"isbn", "0-553-29335-4"},
                                             {"publishedOn", "1951-06-01"},
                                             {"language", "EN"},
                                             {"categoryId", book["category"]["id"]},
                                             {"authorIds", {book["authors"][0]["id"]}}});
    CHECK_EQ(updated["isbn"], "9780553293357");
    CHECK_EQ(updated["version"], 2);
    CHECK_EQ(a.error("books.update", {{"id", id},
                                      {"version", 1},
                                      {"title", "Stale"},
                                      {"publishedOn", "1951-06-01"},
                                      {"language", "en"},
                                      {"categoryId", book["category"]["id"]},
                                      {"authorIds", {book["authors"][0]["id"]}}})["data"]["code"],
             "version_conflict");

    a.result("reviews.create", {{"bookId", id}, {"reviewerName", "Maria"}, {"rating", 5}, {"body", "Great"}});
    Json reviews = a.result("reviews.list", {{"bookId", id}});
    CHECK_EQ(reviews["total"], 1);
    CHECK_EQ(reviews["items"][0]["reviewerName"], "Maria");
    CHECK_EQ(a.result("books.get", {{"id", id}})["ratingAverage"], 5.0);

    Json page = a.result("books.search", {{"tags", {"classic"}}, {"sort", "titleAsc"}, {"pageSize", 10}});
    CHECK_EQ(page["total"], 1);
    CHECK_EQ(page["pageSize"], 10);
    CHECK_EQ(a.result("tags.list").size(), 0u);  // fake listUsed returns nothing

    CHECK_EQ(a.result("books.delete", {{"id", id}}), true);
    CHECK_EQ(a.error("books.get", {{"id", id}})["code"], errors::kNotFound);
}

TEST(reactions_over_json) {
    Api a;
    const auto id = a.seedBook();
    for (int i = 0; i < 4; ++i) a.services.reactions->record(catalog::BookId(id), Reaction::Like);
    a.services.reactions->record(catalog::BookId(id), Reaction::Dislike);
    a.services.reactions->flush();

    Json stats = a.result("reactions.get", {{"bookId", id}});
    CHECK_EQ(stats["periods"]["today"]["likes"], 4);
    CHECK_EQ(stats["periods"]["allTime"]["score"], 3);
    CHECK_EQ(stats["periods"]["yesterday"]["likes"], 0);
    CHECK_EQ(stats["periods"].size(), 6u);

    Json top = a.result("reactions.top", {{"period", "last7Days"}, {"limit", 5}});
    CHECK_EQ(top["items"][0]["bookId"], id);
    CHECK_EQ(top["items"][0]["score"], 3);
    CHECK(top["from"].is_string());
    CHECK(a.result("reactions.top", {{"period", "allTime"}})["from"].is_null());
    CHECK_EQ(a.error("reactions.top", {{"period", "forever"}})["data"]["field"], "period");
    CHECK_EQ(a.error("reactions.top", {{"period", "today"}, {"order", "best"}})["data"]["field"], "order");
    CHECK_EQ(a.result("books.get", {{"id", id}})["likes"], 4);
}

// ---- Schemas and OpenRPC ------------------------------------------------------

TEST(schema_validator_keywords) {
    namespace S = schema;
    std::map<std::string, Json> components = {
        {"Point", S::object({{"x", S::integer()}, {"label", S::nullable(S::string())}})}};
    SchemaValidator v(components);
    auto ok = [&](const Json& value, const Json& s) { return !v.check(value, s, "v").has_value(); };
    auto problem = [&](const Json& value, const Json& s) { return v.check(value, s, "v").value_or("<valid>"); };

    CHECK(ok(5, S::integer(1, 10)));
    CHECK_EQ(problem(5.5, S::integer()), "v: must be an integer");
    CHECK_EQ(problem(0, S::integer(1)), "v: must be at least 1");
    CHECK_EQ(problem("", S::string(1)), "v: must not be empty");
    CHECK(ok("Καλημέρα", S::string(1, 8)));  // characters, not bytes
    CHECK_EQ(problem("Καλημέρα!", S::string(1, 8)), "v: must be at most 8 characters");
    CHECK_EQ(problem("2026-02-30", S::date()), "v: must be a date \"YYYY-MM-DD\"");
    CHECK(ok("2026-10-02T21:47:03.5+03:00", S::dateTime()));
    CHECK_EQ(problem("x", S::enumOf({"a", "b"})), "v: must be one of: a, b");
    CHECK(ok(nullptr, S::nullable(S::string())));
    CHECK_EQ(problem(3, S::nullable(S::string())), "v: must be a string");
    CHECK_EQ(problem(Json::array({1, 1}),
                     [] {
                         Json a = S::array(S::integer());
                         a["uniqueItems"] = true;
                         return a;
                     }()),
             "v: must not contain duplicates");
    CHECK_EQ(problem(Json::array({1, "2"}), S::array(S::integer())), "v[1]: must be an integer");
    CHECK(ok({{"x", 1}, {"label", nullptr}}, S::ref("Point")));
    CHECK_EQ(problem({{"x", 1}}, S::ref("Point")), "v.label: is required");
    CHECK_EQ(problem({{"x", 1}, {"label", "a"}, {"extra", 1}}, S::ref("Point")), "v.extra: is not allowed");
    CHECK_EQ(problem(1, S::ref("Nope")), "v: unresolvable schema reference #/components/schemas/Nope");
}

TEST(params_are_validated_against_their_schemas) {
    Api a;
    const auto id = a.seedBook();
    auto reason = [&](const std::string& method, Json params) {
        return a.error(method, std::move(params))["data"]["reason"];
    };
    CHECK_EQ(reason("reviews.create", {{"bookId", id}, {"reviewerName", "M"}, {"rating", 6}, {"body", "x"}}),
             "rating: must be at most 5");
    CHECK_EQ(reason("reviews.create", {{"bookId", id}, {"reviewerName", "M"}, {"rating", 5}}), "body: is required");
    CHECK_EQ(reason("reviews.create", {{"bookId", id}, {"reviewerName", ""}, {"rating", 5}, {"body", "x"}}),
             "reviewerName: must not be empty");
    CHECK_EQ(reason("books.search", {{"pageSize", 101}}), "pageSize: must be at most 100");
    CHECK_EQ(reason("reactions.top", {{"period", "forever"}}),
             "period: must be one of: today, yesterday, last7Days, last30Days, lastYear, allTime");
    CHECK_EQ(reason("books.update", {{"id", id},
                                     {"version", 1},
                                     {"title", "T"},
                                     {"publishedOn", "2000-01-01"},
                                     {"language", "en"},
                                     {"categoryId", 1},
                                     {"authorIds", {2, 2}}}),
             "authorIds: must not contain duplicates");
    // null is "not given": fine for optional params, missing for required ones
    CHECK(a.result("books.search", {{"title", nullptr}})["total"] == 1);
    CHECK_EQ(reason("books.get", {{"id", nullptr}}), "id: is required");
}

TEST(every_method_returns_what_its_schema_says) {
    Api a;
    SchemaValidator validator(a.rpc.schemas());
    std::set<std::string> covered;
    auto checked = [&](const std::string& method, Json params = Json::object()) {
        Json result = a.result(method, std::move(params));
        if (auto problem = validator.check(result, a.rpc.methods().at(method).result, "result"))
            throw test::Failure{method + " returned a result that does not match its schema: " + *problem + "\n" +
                                result.dump()};
        covered.insert(method);
        return result;
    };

    checked("system.ping");
    const auto cat = checked("categories.create", {{"name", "Science Fiction"}})["id"];
    checked("categories.update", {{"id", cat}, {"name", "SF"}});
    checked("categories.get", {{"id", cat}});
    checked("categories.list");
    auto author = checked("authors.create", {{"name", "Isaac Asimov"}, {"birthDate", "1920-01-02"}});
    checked("authors.update", {{"id", author["id"]}, {"version", 1}, {"name", "Isaac Asimov"}, {"bio", "Prolific"}});
    checked("authors.get", {{"id", author["id"]}});
    checked("authors.search", {{"name", "asimov"}});
    Json bookParams =
        {{"title", "Foundation"},   {"isbn", "0-553-29335-4"}, {"publishedOn", "1951-06-01"}, {"language", "en"},
         {"pageCount", 255},        {"categoryId", cat},       {"authorIds", {author["id"]}}, {"tags", {"classic"}},
         {"reactionsEnabled", true}};
    auto book = checked("books.create", bookParams);
    Json update = bookParams;
    update.erase("reactionsEnabled");
    update["id"] = book["id"];
    update["version"] = 1;
    checked("books.update", update);
    checked("books.setReactionsEnabled", {{"id", book["id"]}, {"enabled", true}});
    checked("books.get", {{"id", book["id"]}});
    checked("books.search", {{"tags", {"classic"}}, {"sort", "ratingDesc"}});
    checked("tags.list");
    auto review =
        checked("reviews.create", {{"bookId", book["id"]}, {"reviewerName", "Maria"}, {"rating", 4}, {"body", "Good"}});
    checked("reviews.update",
            {{"id", review["id"]}, {"reviewerName", "Maria"}, {"rating", 5}, {"title", "Great"}, {"body", "Great"}});
    checked("reviews.get", {{"id", review["id"]}});
    checked("reviews.list", {{"bookId", book["id"]}});
    a.services.reactions->record(catalog::BookId(book["id"].get<std::int64_t>()), Reaction::Like);
    a.services.reactions->flush();
    checked("reactions.get", {{"bookId", book["id"]}});
    checked("reactions.top", {{"period", "today"}});
    checked("reactions.top", {{"period", "allTime"}, {"order", "mostDisliked"}});
    checked("rpc.discover");

    std::atomic<bool> release{false};
    a.jobs->add({"slow", "test", scheduler::Schedule::every(std::chrono::hours(1)), [&](scheduler::JobContext&) {
                     while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                 }});
    a.jobs->start();
    CHECK_EQ(checked("scheduler.list").size(), 1u);
    CHECK_EQ(checked("scheduler.get", {{"name", "slow"}})["lastResult"], "never");
    CHECK_EQ(checked("scheduler.pause", {{"name", "slow"}})["paused"], true);
    CHECK_EQ(checked("scheduler.run", {{"name", "slow"}})["running"], true);  // runs while paused
    CHECK_EQ(checked("scheduler.resume", {{"name", "slow"}})["paused"], false);
    release = true;
    a.health = HealthReport{};
    a.health->status = "degraded";
    a.health->problems = {"database unreachable: test"};
    a.health->database.pingMs = std::nullopt;
    a.health->process.memoryBytes = 1024;
    CHECK_EQ(checked("system.health")["problems"].size(), 1u);
    a.jobs->stop();
    checked("reviews.delete", {{"id", review["id"]}});
    checked("books.delete", {{"id", book["id"]}});
    checked("authors.delete", {{"id", author["id"]}});
    checked("categories.delete", {{"id", cat}});

    for (const auto& [name, m] : a.rpc.methods())
        if (!covered.count(name)) throw test::Failure{"method not covered by this test: " + name};
}

TEST(scheduler_methods_report_errors) {
    Api a;
    std::atomic<bool> release{false};
    a.jobs->add({"busy", "test", scheduler::Schedule::every(std::chrono::hours(1)), [&](scheduler::JobContext&) {
                     while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                 }});
    a.jobs->start();
    auto e = a.error("scheduler.get", {{"name", "nope"}});
    CHECK_EQ(e["code"], errors::kNotFound);
    CHECK_EQ(e["data"]["entity"], "job");
    CHECK_EQ(e["data"]["id"], "nope");
    CHECK_EQ(a.error("scheduler.pause", {{"name", "nope"}})["code"], errors::kNotFound);
    CHECK_EQ(a.error("scheduler.resume", {{"name", "nope"}})["code"], errors::kNotFound);
    CHECK_EQ(a.error("scheduler.run", {{"name", "nope"}})["code"], errors::kNotFound);
    a.result("scheduler.run", {{"name", "busy"}});
    auto conflict = a.error("scheduler.run", {{"name", "busy"}});
    CHECK_EQ(conflict["code"], errors::kConflict);
    CHECK_EQ(conflict["data"]["code"], "job_running");
    CHECK_EQ(a.error("scheduler.get", {{"name", ""}})["code"], errors::kInvalidParams);
    release = true;
    a.jobs->stop();
    // Before the first health check there is nothing to return.
    CHECK_EQ(a.error("system.health")["code"], errors::kInternalError);
}

TEST(openrpc_document_describes_every_method) {
    Api a;
    Json doc = a.result("rpc.discover");
    CHECK_EQ(doc["openrpc"], "1.3.2");
    CHECK_EQ(doc["methods"].size(), a.rpc.methods().size());
    for (const auto& m : doc["methods"]) {
        if (m["summary"].get<std::string>().empty()) throw test::Failure{"no summary: " + m["name"].get<std::string>()};
        if (!m["result"].contains("schema")) throw test::Failure{"no result schema: " + m["name"].get<std::string>()};
    }
    // every $ref resolves
    const std::string text = doc.dump();
    for (std::size_t pos = text.find("#/components/"); pos != std::string::npos;
         pos = text.find("#/components/", pos + 1)) {
        const auto end = text.find('"', pos);
        const std::string path = text.substr(pos + 13, end - pos - 13);  // "schemas/Book"
        const auto slash = path.find('/');
        if (!doc["components"][path.substr(0, slash)].contains(path.substr(slash + 1)))
            throw test::Failure{"unresolved reference #/components/" + path};
    }
}

TEST(committed_openrpc_document_is_current) {
    JsonRpcHandler rpc;  // as `caelitus --openrpc` builds it
    registerCatalogApi(rpc, {});
    registerOperationsApi(rpc, nullptr, nullptr);
    const Json generated = openRpcDocument(rpc, catalogApiInfo());
    std::ifstream in(std::string(CAELITUS_SOURCE_DIR) + "/docs/openrpc.json");
    if (!in) throw test::Failure{"docs/openrpc.json is missing; run: cmake --build <build-dir> --target openrpc"};
    const Json committed = Json::parse(in);
    if (committed != generated)
        throw test::Failure{"docs/openrpc.json is out of date; run: cmake --build <build-dir> --target openrpc"};
}

// ---- MQTT listener -----------------------------------------------------------

namespace {

class FakeSubscriber final : public mqtt::IMqttSubscriber {
public:
    std::map<mqtt::SubscriptionId, std::pair<std::string, mqtt::MessageHandler>> subs;
    void deliver(const std::string& topic) {
        for (auto& [id, sub] : subs)
            if (mqtt::topicMatches(sub.first, topic)) sub.second({topic, "", mqtt::QoS::AtMostOnce, false});
    }

protected:
    mqtt::SubscriptionId doSubscribe(std::string_view filter, mqtt::MessageHandler h,
                                     std::optional<mqtt::QoS>) override {
        subs[next_] = {std::string(filter), std::move(h)};
        return next_++;
    }
    void doUnsubscribe(mqtt::SubscriptionId id) override { subs.erase(id); }

private:
    mqtt::SubscriptionId next_ = 1;
};

}  // namespace

TEST(listener_turns_topics_into_reactions) {
    Api a;
    const auto id = a.seedBook();
    auto mqtt = std::make_shared<FakeSubscriber>();
    {
        MqttReactionListener listener(mqtt, a.services.reactions, "catalog/in/books/");
        CHECK_EQ(mqtt->subs.size(), 2u);
        const std::string base = "catalog/in/books/" + std::to_string(id);
        mqtt->deliver(base + "/like");
        mqtt->deliver(base + "/like");
        mqtt->deliver(base + "/dislike");
        mqtt->deliver("catalog/in/books/abc/like");  // not an id
        mqtt->deliver("catalog/in/books/0/like");    // not a valid id
        CHECK_EQ(listener.accepted(), 3u);
        CHECK_EQ(listener.rejected(), 2u);
    }
    CHECK(mqtt->subs.empty());  // unsubscribed on destruction

    // Switched off: messages are ignored, not counted.
    a.result("books.setReactionsEnabled", {{"id", id}, {"enabled", false}});
    {
        MqttReactionListener listener(mqtt, a.services.reactions);
        mqtt->deliver("catalog/in/books/" + std::to_string(id) + "/like");
        mqtt->deliver("catalog/in/books/777/like");  // unknown book
        CHECK_EQ(listener.accepted(), 0u);
        CHECK_EQ(listener.ignored(), 2u);
    }

    a.services.reactions->flush();
    Json stats = a.result("reactions.get", {{"bookId", id}});
    CHECK_EQ(stats["periods"]["today"]["likes"], 2);
    CHECK_EQ(stats["periods"]["today"]["dislikes"], 1);
}

int main() { return test::runAll(); }
