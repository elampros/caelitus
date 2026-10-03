#pragma once

/// @file
/// The library-independent part of an MQTT client.
/// @ingroup mqtt

#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"
#include "caelitus/mqtt/IMqttClient.hpp"
#include "caelitus/mqtt/MqttConfig.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace caelitus::mqtt {

/// The library-independent part of an MQTT client.
///
/// Implements validation, publish defaults, the subscription registry
/// (re-subscribed on every connect), routing of received messages to handlers
/// on a dedicated dispatcher thread, the online/offline status (LastWill),
/// statistics and rate-limited logging. A concrete client (MosquittoClient)
/// only moves bytes and reports connection events through the handle*()
/// methods.
///
/// Why a dispatcher thread: handlers never run on the network thread, so a
/// slow handler delays other messages but never the connection's keep-alive.
///
/// Logging ("mqtt" logger):
/// | Level | What |
/// |-------|------|
/// | info  | connect, reconnect (with downtime), clean disconnect, subscribe |
/// | warn  | connection lost, still disconnected (once a minute), dropped messages, full incoming queue |
/// | error | broker refused the connection, a handler threw, a send failed |
/// | trace | every published / received message |
///
/// Repeating warnings and errors are throttled to one line per minute with a
/// count of what was suppressed.
class MqttClientBase : public IMqttClient {
public:
    ~MqttClientBase() override;

    bool isConnected() const override { return connected_.load(); }
    bool waitUntilConnected(std::chrono::milliseconds timeout) override;
    Stats stats() const override;

protected:
    /// Outcome of a transportPublish().
    enum class SendResult {
        Ok,            ///< Queued for sending.
        NotConnected,  ///< No connection right now; the message is dropped.
        Failed         ///< Any other failure; `error` says why.
    };

    /// Validates the configuration.
    /// @throws MqttError for a missing server, an invalid will topic, an
    ///         oversized will payload, or a persistent session without a client id.
    explicit MqttClientBase(MqttConfig config);

    /// @name Transport, implemented by the concrete client
    /// Called with none of this class's locks held, except
    /// transportPublish() for the online/offline status (see
    /// publishOfflineStatus()), so implementations must not call back into
    /// this class.
    /// @{

    /// Sends one message.
    virtual SendResult transportPublish(const std::string& topic, const std::string& payload, QoS qos, bool retain,
                                        std::string& error) = 0;
    /// Sends a SUBSCRIBE. Only called while connected.
    /// @return false (with `error` set) on failure.
    virtual bool transportSubscribe(const std::string& filter, QoS qos, std::string& error) = 0;
    /// Sends an UNSUBSCRIBE. Only called while connected.
    /// @return false (with `error` set) on failure.
    virtual bool transportUnsubscribe(const std::string& filter, std::string& error) = 0;
    /// @}

    /// @name Events reported by the concrete client (from any thread)
    /// @{

    /// The broker accepted the connection: publishes the online status and
    /// re-sends every subscription.
    void handleConnected(bool sessionPresent);
    /// The broker refused the connection (bad credentials, ...); logged, throttled.
    void handleConnectRefused(const std::string& reason);
    /// The connection ended; `requested` is true for our own disconnect.
    void handleDisconnected(bool requested, const std::string& reason);
    /// A message arrived; queued for the dispatcher (dropped if the queue is full).
    void handleMessage(Message message);
    /// @}

    /// Starts the dispatcher thread, which delivers received messages to
    /// handlers. Start it before connecting.
    void startDispatcher();
    /// Delivers what is already queued, then joins the dispatcher thread.
    void stopDispatcher();

    /// With a configured last will: publishes its offline payload.
    ///
    /// Call from stop() while still connected, before disconnecting. Once it
    /// has run, a connect that races with stop() no longer publishes the
    /// online payload, so the retained status can never end up "online" after
    /// a clean stop.
    void publishOfflineStatus();
    /// Re-enables the online status after a stop; call from start().
    void resetOfflineStatus();

    /// The configuration given to the constructor.
    const MqttConfig& config() const noexcept { return config_; }
    /// "host:port", for log messages.
    std::string brokerAddress() const;

    log::Logger log_;  ///< The "mqtt" logger.

private:
    struct Subscription {
        SubscriptionId id;
        std::string filter;
        QoS qos;
        std::shared_ptr<MessageHandler> handler;
    };

    bool doPublish(std::string_view topic, std::string_view payload, const PublishOptions& options) override;
    SubscriptionId doSubscribe(std::string_view filter, MessageHandler handler, std::optional<QoS> qos) override;
    void doUnsubscribe(SubscriptionId id) override;

    // Highest QoS requested for `filter`, or nullopt if nobody subscribes to it.
    std::optional<QoS> effectiveQos(const std::string& filter) const;  // requires subsMutex_
    bool sendSubscribe(const std::string& filter, QoS qos, bool logSuccess);

    void dispatchLoop();
    void dispatch(const Message& message);
    void checkStillDisconnected();

    MqttConfig config_;

    // connection state
    std::atomic<bool> connected_{false};
    mutable std::mutex stateMutex_;
    std::condition_variable connectedCv_;
    bool everConnected_ = false;
    std::chrono::steady_clock::time_point disconnectedSince_;

    // Orders the online/offline status publishes (see publishOfflineStatus()).
    // Held around transportPublish(), which never calls back into this class.
    std::mutex statusMutex_;
    bool offlineSent_ = false;

    // subscriptions
    mutable std::mutex subsMutex_;
    std::vector<Subscription> subs_;
    SubscriptionId nextId_ = 1;

    // dispatcher
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Message> queue_;
    bool dispatcherRunning_ = false;
    std::thread dispatcher_;

    // statistics
    std::atomic<std::uint64_t> published_{0}, publishDropped_{0}, received_{0}, receiveDropped_{0}, handlerErrors_{0};

    // log throttles
    log::LogThrottle notConnectedLog_, sendFailedLog_, refusedLog_, stillDisconnectedLog_, queueFullLog_,
        handlerErrorLog_, unroutedLog_;
};

}  // namespace caelitus::mqtt
