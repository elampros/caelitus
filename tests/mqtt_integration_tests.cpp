/// @file
/// Integration tests against a real MQTT broker. Skipped (exit 0) unless
/// CAELITUS_TEST_MQTT_HOST is set.
///
/// @code{.sh}
/// docker run -d --rm --name caelitus-test-mqtt -p 1884:1883
///     eclipse-mosquitto mosquitto -c /mosquitto-no-auth.conf
/// CAELITUS_TEST_MQTT_HOST=127.0.0.1 CAELITUS_TEST_MQTT_PORT=1884
///     CAELITUS_TEST_MQTT_RESTART_CMD="docker restart caelitus-test-mqtt" ./mqtt_integration_tests
/// @endcode
/// @ingroup tests

#include "TestHarness.hpp"

#include "caelitus/log/Log.hpp"
#include "caelitus/mqtt/mosquitto/MosquittoClient.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>

using namespace caelitus::mqtt;
using namespace std::chrono_literals;

namespace {

std::string env(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

std::string uniqueId() {
    static std::mt19937_64 rng(std::random_device{}());
    return std::to_string(rng() % 1000000000);
}

MqttConfig config(const std::string& clientId) {
    MqttConfig c;
    c.server = env("CAELITUS_TEST_MQTT_HOST", "127.0.0.1");
    c.port = static_cast<std::uint16_t>(std::stoi(env("CAELITUS_TEST_MQTT_PORT", "1883")));
    c.clientId = "caelitus-it-" + clientId + "-" + uniqueId();
    c.keepAlive = 5s;
    c.reconnectMinDelay = 1s;
    c.reconnectMaxDelay = 2s;
    return c;
}

// Collects received messages thread-safely. The handler shares ownership of
// the storage, so a client may outlive the Inbox it delivers to.
struct Inbox {
    struct State {
        std::mutex mutex;
        std::vector<Message> messages;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    MessageHandler handler() {
        return [state = state](const Message& m) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->messages.push_back(m);
        };
    }
    std::size_t size() {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->messages.size();
    }
    Message at(std::size_t i) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->messages.at(i);
    }
    bool waitFor(std::size_t n, std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (size() < n)
            if (std::chrono::steady_clock::now() > deadline) return false;
            else std::this_thread::sleep_for(5ms);
        return true;
    }
};

std::unique_ptr<MosquittoClient> connected(const MqttConfig& cfg) {
    auto client = std::make_unique<MosquittoClient>(cfg);
    client->start();
    if (!client->waitUntilConnected(5s)) throw test::Failure{"could not connect to broker"};
    return client;
}

}  // namespace

TEST(publish_and_receive_text) {
    const std::string base = "caelitus-it/" + uniqueId();
    auto client = connected(config("pubsub"));
    Inbox inbox;
    client->subscribe(base + "/+/status", inbox.handler());
    std::this_thread::sleep_for(200ms);  // let SUBACK arrive

    CHECK(client->publish(base + "/42/status", "online"));
    CHECK(client->publish(base + "/7/status", "θερμοκρασία 21.5°C"));
    CHECK(client->publish(base + "/7/other", "not for us"));
    CHECK(inbox.waitFor(2));
    std::this_thread::sleep_for(200ms);

    CHECK_EQ(inbox.size(), 2u);
    CHECK_EQ(inbox.at(0).topic, base + "/42/status");
    CHECK_EQ(inbox.at(0).payload, "online");
    CHECK_EQ(inbox.at(1).payload, "θερμοκρασία 21.5°C");
}

TEST(qos1_round_trip) {
    const std::string topic = "caelitus-it/" + uniqueId() + "/q1";
    auto client = connected(config("qos1"));
    Inbox inbox;
    client->subscribe(topic, inbox.handler(), QoS::AtLeastOnce);
    std::this_thread::sleep_for(200ms);
    CHECK(client->publish(topic, "important", PublishOptions::withQos(QoS::AtLeastOnce)));
    CHECK(inbox.waitFor(1));
    CHECK(inbox.at(0).qos == QoS::AtLeastOnce);
}

TEST(retained_message_reaches_late_subscriber) {
    const std::string topic = "caelitus-it/" + uniqueId() + "/retained";
    auto publisher = connected(config("ret-pub"));
    CHECK(publisher->publish(topic, "last-known", PublishOptions::retained()));
    std::this_thread::sleep_for(200ms);

    auto late = connected(config("ret-sub"));
    Inbox inbox;
    late->subscribe(topic, inbox.handler());
    CHECK(inbox.waitFor(1));
    CHECK(inbox.at(0).retained);
    CHECK_EQ(inbox.at(0).payload, "last-known");

    publisher->publish(topic, "", PublishOptions::retained());  // clear it
}

TEST(unsubscribe_stops_delivery) {
    const std::string topic = "caelitus-it/" + uniqueId() + "/unsub";
    auto client = connected(config("unsub"));
    Inbox inbox;
    auto id = client->subscribe(topic, inbox.handler());
    std::this_thread::sleep_for(200ms);
    client->publish(topic, "1");
    CHECK(inbox.waitFor(1));
    client->unsubscribe(id);
    std::this_thread::sleep_for(200ms);
    client->publish(topic, "2");
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(inbox.size(), 1u);
}

TEST(persistent_session_receives_messages_sent_while_offline) {
    const std::string topic = "caelitus-it/" + uniqueId() + "/session";
    MqttConfig cfg = config("session");
    cfg.cleanSession = false;
    cfg.qos = QoS::AtLeastOnce;

    {
        auto client = connected(cfg);
        client->subscribe(topic, [](const Message&) {});
        std::this_thread::sleep_for(200ms);
        client->stop();
    }

    auto sender = connected(config("session-sender"));
    sender->publish(topic, "while you were away", PublishOptions::withQos(QoS::AtLeastOnce));
    std::this_thread::sleep_for(200ms);

    auto client = std::make_unique<MosquittoClient>(cfg);  // same clientId
    Inbox inbox;
    client->subscribe(topic, inbox.handler());
    client->start();
    CHECK(inbox.waitFor(1));
    CHECK_EQ(inbox.at(0).payload, "while you were away");
    client->stop();

    // Remove the persistent session from the broker.
    cfg.cleanSession = true;
    connected(cfg)->stop();
}

std::string g_self;  ///< Path of this executable, to start it again as the crash child.

TEST(will_reports_online_and_offline_on_clean_stop) {
    const std::string status = "caelitus-it/" + uniqueId() + "/status";
    MqttConfig cfg = config("will-clean");
    cfg.will = LastWill{status};

    auto watcher = connected(config("will-watch"));
    Inbox inbox;
    watcher->subscribe(status, inbox.handler());
    std::this_thread::sleep_for(200ms);

    auto client = connected(cfg);
    CHECK(inbox.waitFor(1));
    CHECK_EQ(inbox.at(0).payload, "online");
    client->stop();
    CHECK(inbox.waitFor(2));
    CHECK_EQ(inbox.at(1).payload, "offline");

    // Retained: a late subscriber sees the current state.
    auto late = connected(config("will-late"));
    Inbox lateInbox;
    late->subscribe(status, lateInbox.handler());
    CHECK(lateInbox.waitFor(1));
    CHECK_EQ(lateInbox.at(0).payload, "offline");
    CHECK(lateInbox.at(0).retained);
    late->publish(status, "", PublishOptions::retained());  // clean up
}

TEST(will_is_sent_by_broker_when_process_dies) {
    const std::string status = "caelitus-it/" + uniqueId() + "/crash";
    auto watcher = connected(config("crash-watch"));
    Inbox inbox;
    watcher->subscribe(status, inbox.handler());
    std::this_thread::sleep_for(200ms);

    // A child process connects with the will and exits without stop().
    const std::string cmd = g_self + " --crash-child " + status;
    CHECK_EQ(std::system(cmd.c_str()), 0);

    CHECK(inbox.waitFor(2));
    CHECK_EQ(inbox.at(0).payload, "online");
    CHECK_EQ(inbox.at(1).payload, "offline");
    watcher->publish(status, "", PublishOptions::retained());  // clean up
}

TEST(unreachable_broker_does_not_throw_and_stops_quickly) {
    MqttConfig cfg = config("unreachable");
    cfg.port = 1;  // nothing listens there
    MosquittoClient client(cfg);
    client.start();
    CHECK(!client.waitUntilConnected(500ms));
    CHECK(!client.publish("caelitus-it/x", "dropped"));
    CHECK_EQ(client.stats().publishDropped, 1u);

    const auto t0 = std::chrono::steady_clock::now();
    client.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < 3s);
}

TEST(reconnects_and_resubscribes_after_broker_restart) {
    const char* restart = std::getenv("CAELITUS_TEST_MQTT_RESTART_CMD");
    if (!restart) {
        std::cout << "       (skipped: CAELITUS_TEST_MQTT_RESTART_CMD not set)\n";
        return;
    }
    const std::string topic = "caelitus-it/" + uniqueId() + "/restart";
    auto client = connected(config("restart"));
    Inbox inbox;
    client->subscribe(topic, inbox.handler());
    std::this_thread::sleep_for(200ms);

    CHECK_EQ(std::system(restart), 0);
    // Wait for the drop to be noticed, then for the reconnect.
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (client->isConnected() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
    CHECK(client->waitUntilConnected(15s));
    std::this_thread::sleep_for(300ms);  // resubscribe

    auto other = connected(config("restart-sender"));
    other->publish(topic, "after restart");
    CHECK(inbox.waitFor(1));
}

/// Child process of will_is_sent_by_broker_when_process_dies: connects with a
/// last will, then exits without a clean disconnect, like a crash.
int crashChild(const std::string& status) {
    MqttConfig cfg = config("crash");
    cfg.will = LastWill{status};
    MosquittoClient client(cfg);
    client.start();
    if (!client.waitUntilConnected(5s)) return 1;
    std::this_thread::sleep_for(200ms);  // let "online" go out
    std::_Exit(0);                       // no stop(), no destructors: like a crash
}

/// Runs every test case of this file; with `--crash-child <topic>`, plays the
/// crashing client of the last-will test instead.
int main(int argc, char** argv) {
    g_self = argv[0];
    if (argc == 3 && std::string(argv[1]) == "--crash-child") return crashChild(argv[2]);
    if (!std::getenv("CAELITUS_TEST_MQTT_HOST")) {
        std::cout << "CAELITUS_TEST_MQTT_HOST not set; skipping MQTT integration tests\n";
        return 0;
    }
    caelitus::log::LogConfig logConfig;
    logConfig.level = env("CAELITUS_TEST_LOG_LEVEL", "info");
    caelitus::log::init(logConfig);
    return test::runAll();
}
