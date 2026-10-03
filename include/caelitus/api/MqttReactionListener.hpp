#pragma once

/// @file
/// Likes and dislikes arriving over MQTT.
/// @ingroup api

#include "caelitus/catalog/service/ReactionService.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/log/Throttle.hpp"
#include "caelitus/mqtt/IMqttClient.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace caelitus::api {

/// Inbound MQTT adapter for likes and dislikes.
///
/// Subscribes to "<prefix>/+/like" and "<prefix>/+/dislike" and turns each
/// message (payload ignored) into a catalog::ReactionService::record() call:
///
/// @verbatim
/// mosquitto_pub -t catalog/in/books/42/like -n
/// @endverbatim
///
/// Malformed topics, and books that do not accept reactions, are logged
/// (throttled) and ignored. Unsubscribes when destroyed; a message already
/// queued may still be handled afterwards, which is safe (the handler shares
/// ownership of what it uses).
class MqttReactionListener {
public:
    /// Subscribes at once.
    /// @param mqtt         Where the messages come from.
    /// @param reactions    Counts them.
    /// @param topicPrefix  Topic prefix before "/<bookId>/like" (config
    ///                     "catalog.reactions.topicPrefix").
    MqttReactionListener(std::shared_ptr<mqtt::IMqttSubscriber> mqtt,
                         std::shared_ptr<catalog::ReactionService> reactions,
                         std::string topicPrefix = "catalog/in/books");
    /// Unsubscribes.
    ~MqttReactionListener();

    MqttReactionListener(const MqttReactionListener&) = delete;
    MqttReactionListener& operator=(const MqttReactionListener&) = delete;

    /// Messages counted.
    std::uint64_t accepted() const noexcept { return state_->accepted.load(); }
    /// Messages with a malformed topic (book id not a positive number).
    std::uint64_t rejected() const noexcept { return state_->rejected.load(); }
    /// Messages for books that do not accept reactions (or do not exist).
    std::uint64_t ignored() const noexcept { return state_->ignored.load(); }

private:
    struct State {
        std::shared_ptr<catalog::ReactionService> reactions;
        std::string prefix;
        std::atomic<std::uint64_t> accepted{0}, rejected{0}, ignored{0};
        log::Logger log;
        log::LogThrottle rejectedLog, ignoredLog;

        void onMessage(const mqtt::Message& message);
    };

    std::shared_ptr<mqtt::IMqttSubscriber> mqtt_;
    std::shared_ptr<State> state_;
    std::vector<mqtt::SubscriptionId> subscriptions_;
};

}  // namespace caelitus::api
