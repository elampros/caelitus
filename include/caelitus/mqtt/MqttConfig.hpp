#pragma once

/// @file
/// MQTT client settings (the "mqtt" configuration section).
/// @ingroup mqtt

#include "caelitus/mqtt/MqttTypes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace caelitus::mqtt {

/// An online/offline status topic, kept up to date by the broker and the client.
///
/// - On every (re)connect the client publishes `onlinePayload` (if set).
/// - On a clean stop() the client publishes `payload` itself (the broker does
///   not send the will on a normal disconnect). Once it has, a connect racing
///   with stop() no longer publishes `onlinePayload`, so the status cannot end
///   up "online" after a clean stop.
/// - Otherwise the broker publishes `payload` (the actual MQTT Last Will): at
///   once if the process dies (the OS closes the socket), or about
///   1.5 x keepAlive after the network silently drops.
///
/// With retain (the default), anyone subscribing later sees the current state.
struct LastWill {
    std::string topic;                                                 ///< Required, e.g. "caelitus/status".
    std::string payload = "offline";                                   ///< Sent when the client goes away.
    std::optional<std::string> onlinePayload = std::string("online");  ///< Sent on connect; nullopt: nothing.
    QoS qos = QoS::AtLeastOnce;                                        ///< QoS of both status messages.
    bool retain = true;                                                ///< Retain both status messages.
};

/// How to reach the broker and how the client behaves.
struct MqttConfig {
    std::string server;         ///< Broker host.
    std::uint16_t port = 1883;  ///< Broker port.
    /// Must be unique per running instance: two clients with the same id keep
    /// disconnecting each other. Required (stable) when cleanSession is false.
    std::string clientId = "caelitus";
    std::string username;                ///< Empty: anonymous.
    std::string password;                ///< Used with a username.
    std::chrono::seconds keepAlive{30};  ///< Ping interval; the broker drops the client after ~1.5x without traffic.

    /// Default QoS for publish() and subscribe(); each call may override it.
    QoS qos = QoS::AtMostOnce;
    /// Default retain flag for publish(); each call may override it.
    bool retain = false;

    /// true: the broker forgets subscriptions and queued messages on
    /// disconnect. false: a persistent session; QoS 1/2 messages sent while we
    /// were offline are delivered on reconnect.
    bool cleanSession = true;

    /// Reconnect backoff: doubles from reconnectMinDelay up to reconnectMaxDelay.
    std::chrono::seconds reconnectMinDelay{1};
    /// Upper bound of the reconnect backoff.
    std::chrono::seconds reconnectMaxDelay{30};

    /// Received messages waiting for their handlers; beyond this, new ones are
    /// dropped (and logged) instead of growing memory without bound.
    std::size_t incomingQueueSize = 10000;

    /// Online/offline status topic; none if unset.
    std::optional<LastWill> will;
};

}  // namespace caelitus::mqtt
