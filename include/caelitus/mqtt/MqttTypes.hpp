#pragma once

/// @file
/// Messages, quality of service, handlers and errors.
/// @ingroup mqtt

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace caelitus::mqtt {

/// MQTT delivery guarantee.
enum class QoS : int {
    AtMostOnce = 0,   ///< Fire and forget; may be lost.
    AtLeastOnce = 1,  ///< Acknowledged; may be delivered twice.
    ExactlyOnce = 2,  ///< Four-way handshake; delivered once.
};

/// The numeric QoS (0, 1 or 2).
inline int toInt(QoS q) noexcept { return static_cast<int>(q); }

/// A received message.
struct Message {
    std::string topic;          ///< The topic it was published to (never a filter).
    std::string payload;        ///< Raw bytes; usually UTF-8 text or JSON.
    QoS qos = QoS::AtMostOnce;  ///< QoS it was delivered with.
    bool retained = false;      ///< Delivered from the broker's retained store (sent before we subscribed).
};

/// Per-message overrides; anything left unset uses the MqttConfig default.
struct PublishOptions {
    std::optional<QoS> qos;      ///< Delivery guarantee.
    std::optional<bool> retain;  ///< Ask the broker to keep it for late subscribers.

    /// Options with just the QoS set.
    static PublishOptions withQos(QoS q) { return {q, std::nullopt}; }
    /// Options with just the retain flag set. Publishing an empty retained
    /// payload deletes the topic's retained message.
    static PublishOptions retained(bool r = true) { return {std::nullopt, r}; }
};

/// Receives messages of a subscription.
///
/// Runs on the client's dispatcher thread, one message at a time, in arrival
/// order. It must not block for long: while it runs, later messages wait.
/// Exceptions are caught and logged.
using MessageHandler = std::function<void(const Message&)>;

/// Identifies a subscription, for IMqttSubscriber::unsubscribe().
using SubscriptionId = std::uint64_t;

/// Invalid use of the API: bad topic, oversized payload, bad configuration.
///
/// Not thrown for connectivity problems; see IMqttPublisher::publish().
class MqttError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace caelitus::mqtt
