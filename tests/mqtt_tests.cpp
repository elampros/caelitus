/// @file
/// Unit tests for the library-independent MQTT layer. A fake transport stands
/// in for libmosquitto, so connection events can be driven deterministically.
/// @ingroup tests

#include "LogCapture.hpp"
#include "TestHarness.hpp"

#include "caelitus/log/Log.hpp"
#include "caelitus/mqtt/MqttClientBase.hpp"
#include "caelitus/mqtt/Topic.hpp"

#include <atomic>
#include <future>
#include <thread>

using namespace caelitus::mqtt;
using test::CaptureSink;

namespace {

std::shared_ptr<CaptureSink> g_logs = std::make_shared<CaptureSink>();

MqttConfig testConfig() {
    MqttConfig c;
    c.server = "broker.test";
    c.clientId = "unit";
    return c;
}

class FakeClient final : public MqttClientBase {
public:
    explicit FakeClient(MqttConfig config = testConfig()) : MqttClientBase(std::move(config)) {
        g_logs->clear();
        startDispatcher();
    }
    ~FakeClient() override { stopDispatcher(); }

    void start() override {
        resetOfflineStatus();
        startDispatcher();
    }
    void stop() override {
        publishOfflineStatus();
        if (online) {
            online = false;
            handleDisconnected(true, "stopped");
        }
        stopDispatcher();
    }

    // ---- drive the client as a broker connection would ----
    void connect(bool sessionPresent = false) {
        online = true;
        handleConnected(sessionPresent);
    }
    void lose(const std::string& reason = "connection lost") {
        online = false;
        handleDisconnected(false, reason);
    }
    void refuse(const std::string& reason) { handleConnectRefused(reason); }
    void receive(const std::string& topic, const std::string& payload) { handleMessage({topic, payload}); }

    std::vector<std::string> sent() {
        std::lock_guard<std::mutex> lock(mutex);
        return log;
    }
    void clearSent() {
        std::lock_guard<std::mutex> lock(mutex);
        log.clear();
    }

    std::atomic<bool> online{false};
    std::atomic<bool> failSends{false};

protected:
    SendResult transportPublish(const std::string& topic, const std::string& payload, QoS qos, bool retain,
                                std::string& error) override {
        if (!online) return SendResult::NotConnected;
        if (failSends) {
            error = "socket error";
            return SendResult::Failed;
        }
        record("pub " + topic + " " + payload + " q" + std::to_string(toInt(qos)) + (retain ? " r" : ""));
        return SendResult::Ok;
    }
    bool transportSubscribe(const std::string& filter, QoS qos, std::string&) override {
        record("sub " + filter + " q" + std::to_string(toInt(qos)));
        return true;
    }
    bool transportUnsubscribe(const std::string& filter, std::string&) override {
        record("unsub " + filter);
        return true;
    }

private:
    void record(std::string s) {
        std::lock_guard<std::mutex> lock(mutex);
        log.push_back(std::move(s));
    }
    std::mutex mutex;
    std::vector<std::string> log;
};

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::vector<std::string> expect(std::initializer_list<const char*> items) { return {items.begin(), items.end()}; }

}  // namespace

// ---- Topics ------------------------------------------------------------------

TEST(topic_names_are_validated) {
    validateTopicName("sensors/42/temp");
    validateTopicName("a//b");  // empty levels are legal
    validateTopicName("θερμοκρασία/σαλόνι");
    CHECK_THROWS_AS(validateTopicName(""), MqttError);
    CHECK_THROWS_AS(validateTopicName("a/+/b"), MqttError);
    CHECK_THROWS_AS(validateTopicName("a/#"), MqttError);
    CHECK_THROWS_AS(validateTopicName(std::string("a\0b", 3)), MqttError);
    CHECK_THROWS_AS(validateTopicName("bad\xC3"), MqttError);      // truncated UTF-8
    CHECK_THROWS_AS(validateTopicName("bad\xC0\xAF"), MqttError);  // overlong encoding
    CHECK_THROWS_AS(validateTopicName(std::string(65536, 'a')), MqttError);
}

TEST(topic_filters_are_validated) {
    for (const char* ok : {"a/b", "a/+/c", "+", "#", "a/#", "+/+/#", "$share/g1/a/+", "$SYS/#"})
        validateTopicFilter(ok);
    for (const char* bad : {"", "a/b#", "a/#/c", "a+/b", "a/+b", "$share//a", "$share/g", "$share/g+/a"}) {
        bool threw = false;
        try {
            validateTopicFilter(bad);
        } catch (const MqttError&) {
            threw = true;
        }
        if (!threw) throw test::Failure{std::string("accepted bad filter '") + bad + "'"};
    }
}

TEST(topic_matching) {
    struct Case {
        const char* filter;
        const char* topic;
        bool match;
    };
    const Case cases[] = {
        {"a/b", "a/b", true},
        {"a/b", "a/c", false},
        {"a/+", "a/b", true},
        {"a/+", "a/b/c", false},
        {"a/+", "a", false},
        {"a/+", "a/", true},
        {"a/#", "a", true},
        {"a/#", "a/b/c", true},
        {"#", "a/b", true},
        {"+/+", "a/b", true},
        {"+", "a/b", false},
        {"a/+/c", "a/x/c", true},
        {"#", "$SYS/uptime", false},
        {"+/x", "$SYS/x", false},
        {"$SYS/#", "$SYS/uptime", true},
        {"$share/g/a/+", "a/b", true},
        {"a/b/c", "a/b", false},
        {"a", "a/b", false},
    };
    for (const auto& c : cases)
        if (topicMatches(c.filter, c.topic) != c.match)
            throw test::Failure{std::string("topicMatches('") + c.filter + "', '" + c.topic +
                                "') != " + (c.match ? "true" : "false")};
}

// ---- Publish -----------------------------------------------------------------

TEST(publish_uses_configured_defaults_and_overrides) {
    MqttConfig cfg = testConfig();
    cfg.qos = QoS::AtLeastOnce;
    cfg.retain = true;
    FakeClient client(cfg);
    client.connect();

    CHECK(client.publish("t", "a"));
    CHECK(client.publish("t", "b", PublishOptions::withQos(QoS::AtMostOnce)));
    CHECK(client.publish("t", "c", PublishOptions::retained(false)));
    CHECK(client.sent() == expect({"pub t a q1 r", "pub t b q0 r", "pub t c q1"}));
    CHECK_EQ(client.stats().published, 3u);
}

TEST(publish_rejects_invalid_topics) {
    FakeClient client;
    client.connect();
    CHECK_THROWS_AS(client.publish("a/+", "x"), MqttError);
    CHECK_THROWS_AS(client.publish("", "x"), MqttError);
}

TEST(publish_while_disconnected_returns_false_and_logs_once) {
    FakeClient client;
    for (int i = 0; i < 100; ++i) CHECK(!client.publish("t", "x"));
    CHECK_EQ(client.stats().publishDropped, 100u);
    CHECK_EQ(g_logs->count(spdlog::level::warn, "Not connected to broker"), 1u);

    // After a reconnect, a new outage is reported again.
    client.connect();
    client.lose();
    CHECK(!client.publish("t", "x"));
    CHECK_EQ(g_logs->count(spdlog::level::warn, "Not connected to broker"), 2u);
}

TEST(publish_failure_is_logged_as_error_and_throttled) {
    FakeClient client;
    client.connect();
    client.failSends = true;
    for (int i = 0; i < 10; ++i) CHECK(!client.publish("t", "x"));
    CHECK_EQ(g_logs->count(spdlog::level::err, "Publishing to 't' failed: socket error"), 1u);
}

// ---- Subscriptions -----------------------------------------------------------

TEST(subscriptions_made_before_connect_are_sent_on_connect) {
    FakeClient client;
    client.subscribe("a/#", [](const Message&) {});
    client.subscribe("b", [](const Message&) {}, QoS::AtLeastOnce);
    CHECK(client.sent().empty());
    client.connect();
    CHECK(client.sent() == expect({"sub a/# q0", "sub b q1"}));
}

TEST(subscriptions_are_restored_after_reconnect) {
    FakeClient client;
    client.connect();
    client.subscribe("a", [](const Message&) {});
    client.lose();
    client.clearSent();
    client.connect();
    CHECK(client.sent() == expect({"sub a q0"}));
    CHECK(g_logs->contains(spdlog::level::info, "Reconnected to broker.test:1883"));
    CHECK(g_logs->contains(spdlog::level::info, "Restored 1 of 1 subscriptions: a"));
    CHECK(g_logs->contains(spdlog::level::warn, "Connection to broker.test:1883 lost"));
}

TEST(same_filter_is_subscribed_once_unless_qos_rises) {
    FakeClient client;
    client.connect();
    auto id1 = client.subscribe("a", [](const Message&) {});
    auto id2 = client.subscribe("a", [](const Message&) {});
    client.subscribe("a", [](const Message&) {}, QoS::ExactlyOnce);
    CHECK(client.sent() == expect({"sub a q0", "sub a q2"}));

    client.clearSent();
    client.unsubscribe(id1);
    client.unsubscribe(id2);
    CHECK(client.sent().empty());  // a handler is still subscribed
    client.unsubscribe(999);       // unknown: ignored
}

TEST(last_unsubscribe_unsubscribes_at_broker) {
    FakeClient client;
    client.connect();
    auto id = client.subscribe("a", [](const Message&) {});
    client.clearSent();
    client.unsubscribe(id);
    CHECK(client.sent() == expect({"unsub a"}));
}

TEST(subscribe_rejects_bad_filters_and_empty_handlers) {
    FakeClient client;
    CHECK_THROWS_AS(client.subscribe("a/b#", [](const Message&) {}), MqttError);
    CHECK_THROWS_AS(client.subscribe("a", MessageHandler{}), MqttError);
}

// ---- Receiving ---------------------------------------------------------------

TEST(messages_reach_matching_handlers_in_order) {
    FakeClient client;
    std::mutex m;
    std::vector<std::string> got;
    client.subscribe("sensors/+/temp", [&](const Message& msg) {
        std::lock_guard<std::mutex> lock(m);
        got.push_back("temp:" + msg.topic + "=" + msg.payload);
    });
    client.subscribe("sensors/#", [&](const Message& msg) {
        std::lock_guard<std::mutex> lock(m);
        got.push_back("all:" + msg.payload);
    });
    client.connect();
    client.receive("sensors/1/temp", "21.5");
    client.receive("sensors/1/hum", "40");
    client.receive("other", "ignored");
    client.stop();  // drains the queue

    CHECK(got == expect({"temp:sensors/1/temp=21.5", "all:21.5", "all:40"}));
    CHECK_EQ(client.stats().received, 3u);
}

TEST(throwing_handler_is_logged_and_does_not_stop_others) {
    FakeClient client;
    std::atomic<int> calls{0};
    client.subscribe("t", [](const Message&) { throw std::runtime_error("bad payload"); });
    client.subscribe("t", [&](const Message&) { ++calls; });
    for (int i = 0; i < 5; ++i) client.receive("t", "x");
    client.stop();
    CHECK_EQ(calls.load(), 5);
    CHECK_EQ(client.stats().handlerErrors, 5u);
    CHECK_EQ(g_logs->count(spdlog::level::err, "Handler for 't' failed on message from 't': bad payload"), 1u);
}

TEST(handlers_may_publish) {
    FakeClient client;
    client.connect();
    client.subscribe("ping", [&](const Message& m) { client.publish("pong", m.payload); });
    client.receive("ping", "42");
    CHECK(waitFor([&] { return client.sent().size() == 2; }));
    CHECK_EQ(client.sent().back(), "pub pong 42 q0");
}

TEST(full_incoming_queue_drops_and_warns) {
    MqttConfig cfg = testConfig();
    cfg.incomingQueueSize = 2;
    FakeClient client(cfg);

    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<bool> inHandler{false};
    client.subscribe("t", [&](const Message&) {
        inHandler = true;
        gate.wait();
    });

    client.receive("t", "1");  // taken by the (blocked) handler
    CHECK(waitFor([&] { return inHandler.load(); }));
    client.receive("t", "2");  // queued
    client.receive("t", "3");  // queued
    client.receive("t", "4");  // dropped
    client.receive("t", "5");  // dropped
    release.set_value();
    client.stop();

    CHECK_EQ(client.stats().receiveDropped, 2u);
    CHECK_EQ(g_logs->count(spdlog::level::warn, "Incoming queue full"), 1u);
}

// ---- Connection events -------------------------------------------------------

TEST(connection_refusal_is_throttled) {
    FakeClient client;
    for (int i = 0; i < 20; ++i) client.refuse("not authorised");
    CHECK_EQ(g_logs->count(spdlog::level::err, "refused the connection: not authorised"), 1u);
}

TEST(wait_until_connected) {
    FakeClient client;
    CHECK(!client.waitUntilConnected(std::chrono::milliseconds(10)));
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client.connect();
    });
    CHECK(client.waitUntilConnected(std::chrono::milliseconds(2000)));
    t.join();
    CHECK(client.isConnected());
}

// ---- Last will ---------------------------------------------------------------

/// A test configuration with a last will on its status topic.
MqttConfig willConfig() {
    MqttConfig cfg = testConfig();
    cfg.will = LastWill{"app/status"};
    return cfg;
}

TEST(will_online_status_is_published_on_every_connect) {
    FakeClient client(willConfig());
    client.subscribe("cmd", [](const Message&) {});
    client.connect();
    CHECK(client.sent() == expect({"pub app/status online q1 r", "sub cmd q0"}));
    client.lose();
    client.clearSent();
    client.connect();
    CHECK(client.sent() == expect({"pub app/status online q1 r", "sub cmd q0"}));
    CHECK(g_logs->contains(spdlog::level::info, "last will on 'app/status'"));
}

TEST(will_offline_status_is_published_on_clean_stop) {
    FakeClient client(willConfig());
    client.connect();
    client.clearSent();
    client.stop();
    CHECK(client.sent() == expect({"pub app/status offline q1 r"}));
}

// The connect callback runs on the network thread and may publish "online"
// after stop() has already published "offline". Both are retained, so the
// broker would keep reporting "online" for a stopped client.
TEST(will_online_status_is_not_published_after_clean_stop) {
    FakeClient client(willConfig());
    client.connect();
    client.stop();
    client.clearSent();
    client.connect();  // a connect callback that lost the race with stop()
    CHECK(client.sent().empty());
}

TEST(will_online_status_is_published_again_after_restart) {
    FakeClient client(willConfig());
    client.connect();
    client.stop();
    client.lose();
    client.clearSent();
    client.start();
    client.connect();
    CHECK(client.sent() == expect({"pub app/status online q1 r"}));
}

TEST(will_without_online_payload_publishes_nothing_on_connect) {
    MqttConfig cfg = willConfig();
    cfg.will->onlinePayload.reset();
    FakeClient client(cfg);
    client.connect();
    CHECK(client.sent().empty());
}

TEST(will_topic_is_validated) {
    MqttConfig cfg = testConfig();
    cfg.will = LastWill{"app/+/status"};
    CHECK_THROWS_AS(FakeClient(cfg), MqttError);
}

TEST(persistent_session_requires_client_id) {
    MqttConfig cfg = testConfig();
    cfg.cleanSession = false;
    cfg.clientId = "";
    CHECK_THROWS_AS(FakeClient(cfg), MqttError);
}

/// Runs every test case of this file (see TestHarness.hpp).
int main() {
    caelitus::log::LogConfig config;
    config.level = "trace";
    config.console = false;
    config.extraSinks = {g_logs};
    caelitus::log::init(config);
    return test::runAll();
}
