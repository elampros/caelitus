/// @file
/// mqtt::MqttClientBase: subscriptions, message dispatch, the online/offline
/// status messages, reconnect handling and statistics.
/// @ingroup mqtt

#include "caelitus/mqtt/MqttClientBase.hpp"

#include "caelitus/mqtt/Topic.hpp"

#include <algorithm>

namespace caelitus::mqtt {

namespace {

constexpr std::size_t kMaxPayload = 268'435'455;  // MQTT protocol limit
constexpr std::size_t kPreviewLength = 120;

// Payload excerpt for trace logs.
std::string preview(std::string_view payload) {
    std::string out(payload.substr(0, kPreviewLength));
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    if (payload.size() > kPreviewLength) out += "...";
    return out;
}

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

MqttClientBase::MqttClientBase(MqttConfig config) : log_(log::get("mqtt")), config_(std::move(config)) {
    if (config_.server.empty()) throw MqttError("MQTT: server is not configured");
    if (!config_.cleanSession && config_.clientId.empty())
        throw MqttError("MQTT: a persistent session (cleanSession=false) needs a clientId");
    if (config_.will) {
        validateTopicName(config_.will->topic);
        if (config_.will->payload.size() > kMaxPayload) throw MqttError("MQTT: last will payload too large");
    }
    disconnectedSince_ = std::chrono::steady_clock::now();
}

MqttClientBase::~MqttClientBase() { stopDispatcher(); }

std::string MqttClientBase::brokerAddress() const { return config_.server + ":" + std::to_string(config_.port); }

bool MqttClientBase::waitUntilConnected(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    return connectedCv_.wait_for(lock, timeout, [this] { return connected_.load(); });
}

IMqttClient::Stats MqttClientBase::stats() const {
    return {published_.load(), publishDropped_.load(), received_.load(), receiveDropped_.load(), handlerErrors_.load()};
}

// ---- publish -----------------------------------------------------------------

bool MqttClientBase::doPublish(std::string_view topic, std::string_view payload, const PublishOptions& options) {
    validateTopicName(topic);
    if (payload.size() > kMaxPayload)
        throw MqttError("Payload of " + std::to_string(payload.size()) + " bytes exceeds the MQTT limit");

    const QoS qos = options.qos.value_or(config_.qos);
    const bool retain = options.retain.value_or(config_.retain);
    const std::string topicStr(topic);

    std::string error;
    const SendResult result = transportPublish(topicStr, std::string(payload), qos, retain, error);
    switch (result) {
        case SendResult::Ok:
            ++published_;
            if (log_->should_log(spdlog::level::trace))
                log_->trace("-> {} [{} bytes, qos {}{}] {}", topicStr, payload.size(), toInt(qos),
                            retain ? ", retain" : "", preview(payload));
            return true;
        case SendResult::NotConnected:
            ++publishDropped_;
            if (auto suppressed = notConnectedLog_.allow())
                log_->warn("Not connected to broker; message to '{}' dropped{}", topicStr,
                           log::suppressedSuffix(*suppressed));
            return false;
        case SendResult::Failed:
            ++publishDropped_;
            if (auto suppressed = sendFailedLog_.allow())
                log_->error("Publishing to '{}' failed: {}{}", topicStr, error, log::suppressedSuffix(*suppressed));
            return false;
    }
    return false;
}

// ---- subscriptions -----------------------------------------------------------

std::optional<QoS> MqttClientBase::effectiveQos(const std::string& filter) const {
    std::optional<QoS> best;
    for (const auto& s : subs_)
        if (s.filter == filter && (!best || toInt(s.qos) > toInt(*best))) best = s.qos;
    return best;
}

bool MqttClientBase::sendSubscribe(const std::string& filter, QoS qos, bool logSuccess) {
    std::string error;
    if (!transportSubscribe(filter, qos, error)) {
        log_->error("Subscribing to '{}' failed: {} (will retry on reconnect)", filter, error);
        return false;
    }
    if (logSuccess) log_->info("Subscribed to '{}' (qos {})", filter, toInt(qos));
    else log_->debug("Subscribed to '{}' (qos {})", filter, toInt(qos));
    return true;
}

SubscriptionId MqttClientBase::doSubscribe(std::string_view filter, MessageHandler handler, std::optional<QoS> qos) {
    validateTopicFilter(filter);
    if (!handler) throw MqttError("subscribe: handler is empty");

    const std::string filterStr(filter);
    const QoS requested = qos.value_or(config_.qos);
    SubscriptionId id;
    bool needsSend;
    QoS sendQos;
    {
        std::lock_guard<std::mutex> lock(subsMutex_);
        const std::optional<QoS> before = effectiveQos(filterStr);
        id = nextId_++;
        subs_.push_back({id, filterStr, requested, std::make_shared<MessageHandler>(std::move(handler))});
        sendQos = *effectiveQos(filterStr);
        needsSend = !before || toInt(sendQos) > toInt(*before);  // new filter or higher QoS
    }

    if (!needsSend) {
        log_->debug("Added handler for already subscribed '{}'", filterStr);
    } else if (connected_) {
        sendSubscribe(filterStr, sendQos, true);
    } else {
        log_->debug("Will subscribe to '{}' once connected", filterStr);
    }
    return id;
}

void MqttClientBase::doUnsubscribe(SubscriptionId id) {
    std::string filter;
    bool lastForFilter;
    {
        std::lock_guard<std::mutex> lock(subsMutex_);
        auto it = std::find_if(subs_.begin(), subs_.end(), [id](const Subscription& s) { return s.id == id; });
        if (it == subs_.end()) return;
        filter = it->filter;
        subs_.erase(it);
        lastForFilter = !effectiveQos(filter);
    }
    if (!lastForFilter) return;

    std::string error;
    if (!connected_) {
        log_->debug("Removed subscription to '{}' while disconnected", filter);
    } else if (transportUnsubscribe(filter, error)) {
        log_->info("Unsubscribed from '{}'", filter);
    } else {
        log_->error("Unsubscribing from '{}' failed: {}", filter, error);
    }
}

// ---- connection events -------------------------------------------------------

void MqttClientBase::handleConnected(bool sessionPresent) {
    bool reconnect;
    double downtime;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        reconnect = everConnected_;
        everConnected_ = true;
        downtime = secondsSince(disconnectedSince_);
        connected_ = true;
    }
    connectedCv_.notify_all();
    notConnectedLog_.reset();
    stillDisconnectedLog_.reset();
    refusedLog_.reset();

    if (reconnect)
        log_->info("Reconnected to {} after {:.1f}s (session present: {})", brokerAddress(), downtime,
                   sessionPresent ? "yes" : "no");
    else
        log_->info("Connected to {} as '{}' (clean session: {}, session present: {}{})", brokerAddress(),
                   config_.clientId, config_.cleanSession ? "yes" : "no", sessionPresent ? "yes" : "no",
                   config_.will ? ", last will on '" + config_.will->topic + "'" : std::string());

    if (config_.will && config_.will->onlinePayload) {
        const LastWill& w = *config_.will;
        std::lock_guard<std::mutex> lock(statusMutex_);
        std::string error;
        if (offlineSent_) log_->debug("Not publishing online status: the client is stopping");
        else if (transportPublish(w.topic, *w.onlinePayload, w.qos, w.retain, error) == SendResult::Ok)
            log_->debug("Published online status '{}' to '{}'", *w.onlinePayload, w.topic);
        else
            log_->error("Publishing online status to '{}' failed: {}", w.topic,
                        error.empty() ? "not connected" : error);
    }

    // Always re-send subscriptions: with a clean session the broker forgot
    // them, and re-subscribing on a persistent session is harmless.
    std::vector<std::pair<std::string, QoS>> filters;
    {
        std::lock_guard<std::mutex> lock(subsMutex_);
        for (const auto& s : subs_)
            if (std::none_of(filters.begin(), filters.end(), [&](const auto& f) { return f.first == s.filter; }))
                filters.emplace_back(s.filter, *effectiveQos(s.filter));
    }
    if (filters.empty()) return;
    std::string names;
    std::size_t ok = 0;
    for (const auto& [filter, qos] : filters) {
        if (sendSubscribe(filter, qos, false)) ++ok;
        if (!names.empty()) names += ", ";
        names += filter;
    }
    log_->info("{} {} of {} subscriptions: {}", reconnect ? "Restored" : "Sent", ok, filters.size(), names);
}

void MqttClientBase::handleConnectRefused(const std::string& reason) {
    if (auto suppressed = refusedLog_.allow())
        log_->error("Broker {} refused the connection: {}{}", brokerAddress(), reason,
                    log::suppressedSuffix(*suppressed));
}

void MqttClientBase::handleDisconnected(bool requested, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!connected_) return;
        connected_ = false;
        disconnectedSince_ = std::chrono::steady_clock::now();
    }
    if (requested) log_->debug("Disconnected from {}", brokerAddress());
    else log_->warn("Connection to {} lost: {}; reconnecting in the background", brokerAddress(), reason);
}

void MqttClientBase::publishOfflineStatus() {
    if (!config_.will) return;
    std::lock_guard<std::mutex> lock(statusMutex_);
    offlineSent_ = true;
    if (!connected_) return;
    const LastWill& w = *config_.will;
    std::string error;
    if (transportPublish(w.topic, w.payload, w.qos, w.retain, error) == SendResult::Ok)
        log_->debug("Published offline status '{}' to '{}'", w.payload, w.topic);
    else log_->warn("Publishing offline status to '{}' failed: {}", w.topic, error.empty() ? "not connected" : error);
}

void MqttClientBase::resetOfflineStatus() {
    std::lock_guard<std::mutex> lock(statusMutex_);
    offlineSent_ = false;
}

void MqttClientBase::checkStillDisconnected() {
    double downtime;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (connected_) return;
        downtime = secondsSince(disconnectedSince_);
    }
    if (downtime < 60) return;
    if (stillDisconnectedLog_.allow())
        log_->warn("Still not connected to {} after {:.0f}s; {} outgoing messages dropped so far", brokerAddress(),
                   downtime, publishDropped_.load());
}

// ---- incoming messages -------------------------------------------------------

void MqttClientBase::handleMessage(Message message) {
    ++received_;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queue_.size() < config_.incomingQueueSize) {
            queue_.push_back(std::move(message));
            queueCv_.notify_one();
            return;
        }
    }
    ++receiveDropped_;
    if (auto suppressed = queueFullLog_.allow())
        log_->warn("Incoming queue full ({} messages): message on '{}' dropped; handlers are too slow{}",
                   config_.incomingQueueSize, message.topic, log::suppressedSuffix(*suppressed));
}

void MqttClientBase::startDispatcher() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (dispatcherRunning_) return;
    dispatcherRunning_ = true;
    dispatcher_ = std::thread([this] { dispatchLoop(); });
}

void MqttClientBase::stopDispatcher() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!dispatcherRunning_) return;
        dispatcherRunning_ = false;
    }
    queueCv_.notify_all();
    if (dispatcher_.joinable()) dispatcher_.join();
}

void MqttClientBase::dispatchLoop() {
    for (;;) {
        Message message;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            // Wake up periodically to report a long disconnection.
            queueCv_.wait_for(lock, std::chrono::seconds(5), [this] { return !queue_.empty() || !dispatcherRunning_; });
            if (queue_.empty()) {
                if (!dispatcherRunning_) return;  // stopped and drained
                lock.unlock();
                checkStillDisconnected();
                continue;
            }
            message = std::move(queue_.front());
            queue_.pop_front();
        }
        dispatch(message);
    }
}

void MqttClientBase::dispatch(const Message& message) {
    if (log_->should_log(spdlog::level::trace))
        log_->trace("<- {} [{} bytes, qos {}{}] {}", message.topic, message.payload.size(), toInt(message.qos),
                    message.retained ? ", retained" : "", preview(message.payload));

    std::vector<std::pair<std::string, std::shared_ptr<MessageHandler>>> targets;
    {
        std::lock_guard<std::mutex> lock(subsMutex_);
        for (const auto& s : subs_)
            if (topicMatches(s.filter, message.topic)) targets.emplace_back(s.filter, s.handler);
    }

    if (targets.empty()) {
        // Normal for a moment after unsubscribe, or with a persistent session
        // that still holds old subscriptions.
        if (auto suppressed = unroutedLog_.allow())
            log_->debug("No handler for message on '{}'{}", message.topic, log::suppressedSuffix(*suppressed));
        return;
    }

    for (const auto& [filter, handler] : targets) {
        try {
            (*handler)(message);
        } catch (const std::exception& e) {
            ++handlerErrors_;
            if (auto suppressed = handlerErrorLog_.allow())
                log_->error("Handler for '{}' failed on message from '{}': {}{}", filter, message.topic, e.what(),
                            log::suppressedSuffix(*suppressed));
        } catch (...) {
            ++handlerErrors_;
            if (auto suppressed = handlerErrorLog_.allow())
                log_->error("Handler for '{}' failed on message from '{}' with a non-standard exception{}", filter,
                            message.topic, log::suppressedSuffix(*suppressed));
        }
    }
}

}  // namespace caelitus::mqtt
