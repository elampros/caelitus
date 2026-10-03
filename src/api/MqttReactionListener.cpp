#include "caelitus/api/MqttReactionListener.hpp"

#include <charconv>

namespace caelitus::api {

MqttReactionListener::MqttReactionListener(std::shared_ptr<mqtt::IMqttSubscriber> mqtt,
                                           std::shared_ptr<catalog::ReactionService> reactions, std::string topicPrefix)
    : mqtt_(std::move(mqtt)),
      state_(std::make_shared<State>()) {
    if (!mqtt_ || !reactions) throw std::invalid_argument("MqttReactionListener: null dependency");
    state_->reactions = std::move(reactions);
    state_->prefix = std::move(topicPrefix);
    state_->log = log::get("api.mqtt");
    std::string& prefix = state_->prefix;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    for (const char* kind : {"like", "dislike"})
        subscriptions_.push_back(
            mqtt_->subscribe(prefix + "/+/" + kind, [state = state_](const mqtt::Message& m) { state->onMessage(m); }));
    state_->log->info("Listening for reactions on {}/<bookId>/like|dislike", prefix);
}

MqttReactionListener::~MqttReactionListener() {
    for (auto id : subscriptions_) mqtt_->unsubscribe(id);
}

void MqttReactionListener::State::onMessage(const mqtt::Message& message) {
    // <prefix>/<id>/<kind>
    std::string_view rest(message.topic);
    std::int64_t id = 0;
    std::optional<catalog::Reaction> reaction;
    if (rest.size() > prefix.size() + 1 && rest.compare(0, prefix.size(), prefix) == 0 && rest[prefix.size()] == '/') {
        rest.remove_prefix(prefix.size() + 1);
        const auto slash = rest.find('/');
        if (slash != std::string_view::npos) {
            const std::string_view idText = rest.substr(0, slash);
            const std::string_view kind = rest.substr(slash + 1);
            auto [end, ec] = std::from_chars(idText.data(), idText.data() + idText.size(), id);
            if (ec == std::errc{} && end == idText.data() + idText.size() && id > 0) {
                if (kind == "like") reaction = catalog::Reaction::Like;
                if (kind == "dislike") reaction = catalog::Reaction::Dislike;
            }
        }
    }

    if (!reaction) {
        ++rejected;
        if (auto suppressed = rejectedLog.allow())
            log->warn("Ignoring reaction on unexpected topic '{}'{}", message.topic,
                      log::suppressedSuffix(*suppressed));
        return;
    }
    if (reactions->record(catalog::BookId(id), *reaction)) {
        ++accepted;
        return;
    }
    ++ignored;
    if (auto suppressed = ignoredLog.allow())
        log->debug("Ignoring reactions for book {}: unknown or reactions disabled{}", id,
                   log::suppressedSuffix(*suppressed));
}

}  // namespace caelitus::api
