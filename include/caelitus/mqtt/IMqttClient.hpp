#pragma once

/// @file
/// The MQTT client interfaces.
/// @ingroup mqtt

#include "caelitus/mqtt/MqttTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace caelitus::mqtt {

/// What a service needs to send messages.
///
/// Knows nothing about the client library or broker behind it.
/// Implementations must be thread-safe: services publish from many threads at
/// once.
///
/// @code
/// publisher->publish("devices/42/status", "online");
/// publisher->publish("devices/42/status", "online", PublishOptions::retained());
/// @endcode
class IMqttPublisher {
public:
    virtual ~IMqttPublisher() = default;

    /// Sends a message, without waiting for the broker.
    ///
    /// Publishing is best effort: a missing broker never makes a service call
    /// fail.
    /// @return true if the message was handed to the client for sending; false
    ///         if it was dropped because there is no broker connection right
    ///         now (logged, rate-limited).
    /// @throws MqttError only for invalid input: empty topic, wildcards in the
    ///         topic, oversized payload.
    bool publish(std::string_view topic, std::string_view payload, const PublishOptions& options = {}) {
        return doPublish(topic, payload, options);
    }

protected:
    /// Implementation of publish().
    virtual bool doPublish(std::string_view topic, std::string_view payload, const PublishOptions& options) = 0;
};

/// What a service needs to receive messages.
///
/// @code
/// auto id = subscriber->subscribe("sensors/+/temperature", [](const Message& m) { ... });
/// subscriber->unsubscribe(id);
/// @endcode
///
/// Subscriptions may be made before the client is connected and survive
/// reconnects. A handler may still receive a message already queued when its
/// subscription is removed.
class IMqttSubscriber {
public:
    virtual ~IMqttSubscriber() = default;

    /// Registers `handler` for messages matching `filter`.
    /// @param filter   Topic filter; may contain `+` and `#` wildcards.
    /// @param handler  Called on the dispatcher thread; see MessageHandler.
    /// @param qos      Maximum QoS to receive with; default: MqttConfig::qos.
    /// @return An id for unsubscribe().
    /// @throws MqttError if the filter is invalid.
    SubscriptionId subscribe(std::string_view filter, MessageHandler handler, std::optional<QoS> qos = std::nullopt) {
        return doSubscribe(filter, std::move(handler), qos);
    }

    /// Removes a subscription. Unknown ids are ignored.
    void unsubscribe(SubscriptionId id) { doUnsubscribe(id); }

protected:
    /// Implementation of subscribe().
    virtual SubscriptionId doSubscribe(std::string_view filter, MessageHandler handler, std::optional<QoS> qos) = 0;
    /// Implementation of unsubscribe().
    virtual void doUnsubscribe(SubscriptionId id) = 0;
};

/// The whole client, for the code that owns its lifecycle (main / wiring).
class IMqttClient : public IMqttPublisher, public IMqttSubscriber {
public:
    /// Message counters since start, for monitoring.
    struct Stats {
        std::uint64_t published = 0;       ///< Messages handed to the broker connection.
        std::uint64_t publishDropped = 0;  ///< Not connected, or the send failed.
        std::uint64_t received = 0;        ///< Messages received from the broker.
        std::uint64_t receiveDropped = 0;  ///< Incoming queue full (MqttConfig::incomingQueueSize).
        std::uint64_t handlerErrors = 0;   ///< Handlers that threw.
    };

    /// Connects in the background and keeps reconnecting until stop().
    /// Does not throw if the broker is unreachable.
    virtual void start() = 0;
    /// Publishes the "offline" status (if configured), disconnects cleanly and
    /// delivers messages already received. Idempotent.
    virtual void stop() = 0;

    /// True while a broker connection is up.
    virtual bool isConnected() const = 0;
    /// Waits up to `timeout` for a connection.
    /// @return true if connected.
    virtual bool waitUntilConnected(std::chrono::milliseconds timeout) = 0;
    /// Message counters.
    virtual Stats stats() const = 0;
};

}  // namespace caelitus::mqtt
