// End to end: the application's wiring in-process, a real MariaDB, a real
// MQTT broker and JSON-RPC over a real TCP socket. Skipped (exit 0) unless
// both CAELITUS_TEST_DB_HOST and CAELITUS_TEST_MQTT_HOST are set.

#include "TestHarness.hpp"

#include "caelitus/api/CatalogApi.hpp"
#include "caelitus/api/JsonRpc.hpp"
#include "caelitus/api/MqttReactionListener.hpp"
#include "caelitus/catalog/mariadb/CatalogMigrations.hpp"
#include "caelitus/catalog/mariadb/MariaDbReactionRepository.hpp"
#include "caelitus/catalog/mariadb/MariaDbRepositories.hpp"
#include "caelitus/core/PeriodicTask.hpp"
#include "caelitus/db/TransactionManager.hpp"
#include "caelitus/db/mariadb/MariaDbConnection.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/mqtt/mosquitto/MosquittoClient.hpp"
#include "caelitus/net/TcpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>
#include <thread>

using namespace caelitus;
using namespace std::chrono_literals;

namespace {

std::string env(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

// Minimal blocking JSON-RPC client over TCP (\0-terminated messages).
class RpcClient {
public:
    explicit RpcClient(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) throw test::Failure{"connect failed"};
        timeval tv{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~RpcClient() { ::close(fd_); }

    Json send(const Json& request) {
        std::string out = request.dump();
        out.push_back('\0');
        if (::send(fd_, out.data(), out.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(out.size()))
            throw test::Failure{"send failed"};
        for (;;) {
            auto pos = buf_.find('\0');
            if (pos != std::string::npos) {
                Json reply = Json::parse(buf_.substr(0, pos));
                buf_.erase(0, pos + 1);
                return reply;
            }
            char chunk[4096];
            const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
            if (n <= 0) throw test::Failure{"no reply"};
            buf_.append(chunk, static_cast<std::size_t>(n));
        }
    }

    Json call(const std::string& method, Json params = Json::object()) {
        Json reply = send({{"jsonrpc", "2.0"}, {"id", ++id_}, {"method", method}, {"params", std::move(params)}});
        if (reply.contains("error")) throw test::Failure{method + " failed: " + reply.dump()};
        return reply["result"];
    }

private:
    int fd_;
    int id_ = 0;
    std::string buf_;
};

mqtt::MqttConfig mqttConfig(const std::string& name) {
    mqtt::MqttConfig c;
    c.server = env("CAELITUS_TEST_MQTT_HOST", "127.0.0.1");
    c.port = static_cast<std::uint16_t>(std::stoi(env("CAELITUS_TEST_MQTT_PORT", "1883")));
    c.clientId = "caelitus-e2e-" + name + "-" + std::to_string(::getpid());
    return c;
}

// The application, as main() wires it.
struct App {
    std::shared_ptr<db::ConnectionPool> pool;
    std::shared_ptr<db::SqlExecutor> sql;
    std::shared_ptr<db::TransactionManager> tx;
    std::shared_ptr<mqtt::MosquittoClient> mqtt = std::make_shared<mqtt::MosquittoClient>(mqttConfig("app"));
    std::shared_ptr<catalog::BookCache> bookCache;
    api::CatalogServices services;
    std::unique_ptr<api::MqttReactionListener> listener;
    std::unique_ptr<PeriodicTask> flusher;
    std::shared_ptr<api::JsonRpcHandler> rpc = std::make_shared<api::JsonRpcHandler>();
    std::unique_ptr<net::TcpServer> server;

    App() {
        db::DbConfig dbc;
        dbc.host = env("CAELITUS_TEST_DB_HOST", "127.0.0.1");
        dbc.port = static_cast<std::uint16_t>(std::stoi(env("CAELITUS_TEST_DB_PORT", "3306")));
        dbc.user = env("CAELITUS_TEST_DB_USER", "root");
        dbc.password = env("CAELITUS_TEST_DB_PASSWORD", "test");
        dbc.database = env("CAELITUS_TEST_DB_NAME", "caelitus_test");
        pool = db::ConnectionPool::create(std::make_shared<db::mariadb::MariaDbConnectionFactory>(dbc));
        {
            auto conn = pool->acquire();
            for (const char* t : {"book_reactions_daily", "reviews", "book_tags", "tags", "book_authors", "books",
                                  "authors", "categories", "schema_migrations"})
                conn->execute(std::string("DROP TABLE IF EXISTS ") + t, {});
        }
        db::MigrationRunner(pool).migrate(catalog::mariadb::catalogMigrations());
        sql = std::make_shared<db::SqlExecutor>(pool);
        tx = std::make_shared<db::TransactionManager>(pool);

        mqtt->start();
        if (!mqtt->waitUntilConnected(5s)) throw test::Failure{"broker not reachable"};

        using namespace catalog::mariadb;
        auto categoryRepo = std::make_shared<MariaDbCategoryRepository>(sql);
        auto authorRepo = std::make_shared<MariaDbAuthorRepository>(sql);
        auto tagRepo = std::make_shared<MariaDbTagRepository>(sql);
        auto bookRepo = std::make_shared<MariaDbBookRepository>(sql);
        bookCache = std::make_shared<catalog::BookCache>(bookRepo);
        bookCache->reload();
        services = {
            std::make_shared<catalog::CategoryService>(categoryRepo),
            std::make_shared<catalog::AuthorService>(authorRepo, tx),
            std::make_shared<catalog::BookService>(bookRepo, authorRepo, categoryRepo, tagRepo, tx, mqtt,
                                                   catalog::systemClock(), bookCache),
            std::make_shared<catalog::ReviewService>(std::make_shared<MariaDbReviewRepository>(sql), bookRepo, tx,
                                                     mqtt),
            std::make_shared<catalog::ReactionService>(std::make_shared<MariaDbReactionRepository>(sql), bookRepo,
                                                       bookCache, tx, TimeZone::named("Europe/Athens")),
        };
        topicPrefix = "caelitus-e2e/" + std::to_string(::getpid()) + "/books";
        listener = std::make_unique<api::MqttReactionListener>(mqtt, services.reactions, topicPrefix);
        auto reactions = services.reactions;
        flusher = std::make_unique<PeriodicTask>("flush", 100ms, [reactions] { reactions->flush(); });
        flusher->start();

        api::registerCatalogApi(*rpc, services);
        net::TcpServerConfig sc;
        sc.bindAddress = "127.0.0.1";
        sc.port = 0;
        server = std::make_unique<net::TcpServer>(sc, rpc);
        server->start();
    }

    ~App() {
        server->stop();
        listener.reset();
        flusher->stop();
        services.reactions->flush();
        mqtt->stop();
    }

    std::string topicPrefix;
};

// Collects MQTT messages (thread-safe).
struct Inbox {
    std::mutex mutex;
    std::vector<mqtt::Message> messages;
    mqtt::MessageHandler handler() {
        return [this](const mqtt::Message& m) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back(m);
        };
    }
    bool has(const std::string& topic) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& m : messages)
            if (m.topic == topic) return true;
        return false;
    }
};

template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(50ms);
    }
    return true;
}

}  // namespace

TEST(catalog_over_tcp_and_likes_over_mqtt) {
    App app;
    RpcClient client(app.server->port());

    // Someone watching the catalog's outgoing events.
    mqtt::MosquittoClient observer(mqttConfig("observer"));
    Inbox events;
    observer.subscribe("catalog/books/+/created", events.handler());
    observer.start();
    CHECK(observer.waitUntilConnected(5s));
    std::this_thread::sleep_for(200ms);

    const auto cat = client.call("categories.create", {{"name", "Science Fiction"}})["id"];
    const auto author = client.call("authors.create", {{"name", "Frank Herbert"}, {"birthDate", "1920-10-08"}})["id"];
    auto book = [&](const std::string& title, bool reactions) {
        return client
            .call("books.create", {{"title", title},
                                   {"publishedOn", "1965-08-01"},
                                   {"language", "en"},
                                   {"categoryId", cat},
                                   {"authorIds", {author}},
                                   {"tags", {"Classic", "desert"}},
                                   {"reactionsEnabled", reactions}})["id"]
            .get<std::int64_t>();
    };
    const auto dune = book("Dune", true);
    const auto messiah = book("Dune Messiah", false);
    CHECK(eventually([&] { return events.has("catalog/books/" + std::to_string(dune) + "/created"); }));

    // Readers react over MQTT.
    mqtt::MosquittoClient phone(mqttConfig("phone"));
    phone.start();
    CHECK(phone.waitUntilConnected(5s));
    const std::string duneTopic = app.topicPrefix + "/" + std::to_string(dune);
    const std::string messiahTopic = app.topicPrefix + "/" + std::to_string(messiah);
    for (int i = 0; i < 20; ++i) phone.publish(duneTopic + "/like", "");
    for (int i = 0; i < 5; ++i) phone.publish(duneTopic + "/dislike", "");
    for (int i = 0; i < 10; ++i) phone.publish(messiahTopic + "/like", "");  // switched off: ignored

    auto today = [&](std::int64_t id) { return client.call("reactions.get", {{"bookId", id}})["periods"]["today"]; };
    CHECK(eventually([&] { return today(dune)["likes"] == 20 && today(dune)["dislikes"] == 5; }));
    CHECK_EQ(today(messiah)["likes"], 0);
    CHECK_EQ(app.listener->ignored(), 10u);

    Json top = client.call("reactions.top", {{"period", "today"}});
    CHECK_EQ(top["items"].size(), 1u);
    CHECK_EQ(top["items"][0]["title"], "Dune");
    CHECK_EQ(top["items"][0]["score"], 15);

    // Switching a book on takes effect at once (book cache), no restart.
    CHECK_EQ(client.call("books.setReactionsEnabled", {{"id", messiah}, {"enabled", true}})["reactionsEnabled"], true);
    for (int i = 0; i < 3; ++i) phone.publish(messiahTopic + "/like", "");
    CHECK(eventually([&] { return today(messiah)["likes"] == 3; }));
    CHECK_EQ(client.call("books.get", {{"id", messiah}})["likes"], 3);

    // Batches and errors over the wire.
    Json batch = client.send(
        Json::array({{{"jsonrpc", "2.0"}, {"id", "a"}, {"method", "system.ping"}},
                     {{"jsonrpc", "2.0"}, {"id", "b"}, {"method", "books.get"}, {"params", {{"id", 999999}}}}}));
    CHECK(batch.is_array());
    CHECK_EQ(batch.size(), 2u);
    CHECK(batch[0].contains("result"));
    CHECK_EQ(batch[1]["error"]["code"], api::errors::kNotFound);

    Json search = client.call("books.search", {{"tags", {"desert"}}, {"sort", "titleAsc"}});
    CHECK_EQ(search["total"], 2);
    CHECK_EQ(search["items"][0]["likes"], 20);

    observer.stop();
    phone.stop();
}

int main() {
    if (!std::getenv("CAELITUS_TEST_DB_HOST") || !std::getenv("CAELITUS_TEST_MQTT_HOST")) {
        std::cout << "CAELITUS_TEST_DB_HOST / CAELITUS_TEST_MQTT_HOST not set; skipping end-to-end tests\n";
        return 0;
    }
    log::LogConfig logConfig;
    logConfig.level = env("CAELITUS_TEST_LOG_LEVEL", "warn");
    log::init(logConfig);
    return test::runAll();
}
