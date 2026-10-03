#pragma once

/// @file
/// The MQTT client on libmosquitto.
/// @ingroup mqtt_mosquitto

#include "caelitus/mqtt/MqttClientBase.hpp"

#include <atomic>
#include <mutex>

/// @cond INTERNAL
struct mosquitto;
struct mosquitto_message;
/// @endcond

namespace caelitus::mqtt {

/// IMqttClient over libmosquitto (MQTT 3.1.1).
///
/// libmosquitto runs its own network thread and reconnects with exponential
/// backoff; this class only translates its callbacks into MqttClientBase
/// events. libmosquitto's own log goes to the "mqtt.lib" logger.
///
/// @code
/// MqttConfig cfg;
/// cfg.server = "127.0.0.1";
/// cfg.will = LastWill{"caelitus/status"};
/// auto client = std::make_shared<MosquittoClient>(cfg);
/// client->subscribe("catalog/in/#", [](const Message& m) { ... });
/// client->start();
/// @endcode
class MosquittoClient final : public MqttClientBase {
public:
    /// Validates the configuration and prepares the client; does not connect.
    /// @throws MqttError for an invalid configuration (topic, client id, ...).
    explicit MosquittoClient(MqttConfig config);
    /// Stops the client.
    ~MosquittoClient() override;

    MosquittoClient(const MosquittoClient&) = delete;
    MosquittoClient& operator=(const MosquittoClient&) = delete;

    /// Starts the network thread and connects in the background.
    /// @throws MqttError if the settings are invalid or the thread cannot start.
    void start() override;
    void stop() override;

protected:
    SendResult transportPublish(const std::string& topic, const std::string& payload, QoS qos, bool retain,
                                std::string& error) override;
    bool transportSubscribe(const std::string& filter, QoS qos, std::string& error) override;
    bool transportUnsubscribe(const std::string& filter, std::string& error) override;

private:
    static void onConnect(struct mosquitto*, void* self, int rc, int flags);
    static void onDisconnect(struct mosquitto*, void* self, int rc);
    static void onMessage(struct mosquitto*, void* self, const struct mosquitto_message* msg);
    static void onLog(struct mosquitto*, void* self, int level, const char* text);

    struct mosquitto* mosq_ = nullptr;
    std::mutex lifecycleMutex_;
    bool started_ = false;
    std::atomic<bool> stopping_{false};
    log::Logger libLog_;
};

}  // namespace caelitus::mqtt
