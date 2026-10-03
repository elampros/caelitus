#include "caelitus/mqtt/mosquitto/MosquittoClient.hpp"

#include <mosquitto.h>

#include <cerrno>

#include <mutex>

namespace caelitus::mqtt {

namespace {

void initLibrary() {
    static std::once_flag once;
    std::call_once(once, [] {
        mosquitto_lib_init();
        // Never cleaned up: clients may live until static destruction.
    });
}

std::string errorText(int rc) { return std::string(mosquitto_strerror(rc)) + " (" + std::to_string(rc) + ")"; }

void check(int rc, const char* what) {
    if (rc != MOSQ_ERR_SUCCESS) throw MqttError(std::string("libmosquitto: ") + what + ": " + errorText(rc));
}

}  // namespace

MosquittoClient::MosquittoClient(MqttConfig config) : MqttClientBase(std::move(config)), libLog_(log::get("mqtt.lib")) {
    initLibrary();
    const MqttConfig& c = this->config();

    mosq_ = mosquitto_new(c.clientId.empty() ? nullptr : c.clientId.c_str(), c.cleanSession, this);
    if (!mosq_)
        throw MqttError("libmosquitto: cannot create client: " +
                        errorText(errno == ENOMEM ? MOSQ_ERR_NOMEM : MOSQ_ERR_INVAL));

    try {
        check(mosquitto_int_option(mosq_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311), "protocol version");
        if (!c.username.empty())
            check(mosquitto_username_pw_set(mosq_, c.username.c_str(),
                                            c.password.empty() ? nullptr : c.password.c_str()),
                  "credentials");
        if (c.will)
            check(mosquitto_will_set(mosq_, c.will->topic.c_str(), static_cast<int>(c.will->payload.size()),
                                     c.will->payload.data(), toInt(c.will->qos), c.will->retain),
                  "last will");
        check(mosquitto_reconnect_delay_set(mosq_, static_cast<unsigned>(c.reconnectMinDelay.count()),
                                            static_cast<unsigned>(c.reconnectMaxDelay.count()), true),
              "reconnect delay");
    } catch (...) {
        mosquitto_destroy(mosq_);
        throw;
    }

    mosquitto_connect_with_flags_callback_set(mosq_, &MosquittoClient::onConnect);
    mosquitto_disconnect_callback_set(mosq_, &MosquittoClient::onDisconnect);
    mosquitto_message_callback_set(mosq_, &MosquittoClient::onMessage);
    mosquitto_log_callback_set(mosq_, &MosquittoClient::onLog);
}

MosquittoClient::~MosquittoClient() {
    stop();
    mosquitto_destroy(mosq_);
}

void MosquittoClient::start() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (started_) return;
    const MqttConfig& c = config();

    stopping_ = false;
    resetOfflineStatus();
    startDispatcher();

    // Non-blocking connect. If the broker is down, the network thread keeps
    // retrying, so this is not an error.
    const int rc = mosquitto_connect_async(mosq_, c.server.c_str(), c.port, static_cast<int>(c.keepAlive.count()));
    if (rc == MOSQ_ERR_INVAL) {
        stopDispatcher();
        throw MqttError("libmosquitto: invalid connection settings for " + brokerAddress());
    }
    if (rc != MOSQ_ERR_SUCCESS)
        log_->warn("Cannot reach broker {} yet: {}; retrying in the background", brokerAddress(), errorText(rc));

    const int loop = mosquitto_loop_start(mosq_);
    if (loop != MOSQ_ERR_SUCCESS) {
        stopDispatcher();
        throw MqttError("libmosquitto: cannot start network thread: " + errorText(loop));
    }
    started_ = true;
    log_->info("MQTT client started (broker {}, client id '{}')", brokerAddress(), c.clientId);
}

void MosquittoClient::stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!started_) return;

    stopping_ = true;
    publishOfflineStatus();             // queued ahead of DISCONNECT, so it is sent first
    mosquitto_disconnect(mosq_);        // also ends the reconnect loop
    mosquitto_loop_stop(mosq_, false);  // joins the network thread
    handleDisconnected(true, "client stopped");
    stopDispatcher();  // delivers already received messages
    started_ = false;
    log_->info("MQTT client stopped");
}

// ---- transport ---------------------------------------------------------------

MqttClientBase::SendResult MosquittoClient::transportPublish(const std::string& topic, const std::string& payload,
                                                             QoS qos, bool retain, std::string& error) {
    const int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(),
                                     toInt(qos), retain);
    if (rc == MOSQ_ERR_SUCCESS) return SendResult::Ok;
    if (rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST) return SendResult::NotConnected;
    error = errorText(rc);
    return SendResult::Failed;
}

bool MosquittoClient::transportSubscribe(const std::string& filter, QoS qos, std::string& error) {
    const int rc = mosquitto_subscribe(mosq_, nullptr, filter.c_str(), toInt(qos));
    if (rc == MOSQ_ERR_SUCCESS) return true;
    error = errorText(rc);
    return false;
}

bool MosquittoClient::transportUnsubscribe(const std::string& filter, std::string& error) {
    const int rc = mosquitto_unsubscribe(mosq_, nullptr, filter.c_str());
    if (rc == MOSQ_ERR_SUCCESS) return true;
    error = errorText(rc);
    return false;
}

// ---- callbacks (libmosquitto network thread) ---------------------------------

void MosquittoClient::onConnect(struct mosquitto*, void* self, int rc, int flags) {
    auto* client = static_cast<MosquittoClient*>(self);
    if (rc == 0) client->handleConnected((flags & 1) != 0);
    else client->handleConnectRefused(mosquitto_connack_string(rc));
}

void MosquittoClient::onDisconnect(struct mosquitto*, void* self, int rc) {
    auto* client = static_cast<MosquittoClient*>(self);
    // rc == 0: we called mosquitto_disconnect().
    client->handleDisconnected(rc == 0 || client->stopping_, errorText(rc));
}

void MosquittoClient::onMessage(struct mosquitto*, void* self, const struct mosquitto_message* msg) {
    Message m;
    m.topic = msg->topic;
    if (msg->payload && msg->payloadlen > 0)
        m.payload.assign(static_cast<const char*>(msg->payload), static_cast<std::size_t>(msg->payloadlen));
    m.qos = static_cast<QoS>(msg->qos);
    m.retained = msg->retain;
    static_cast<MosquittoClient*>(self)->handleMessage(std::move(m));
}

void MosquittoClient::onLog(struct mosquitto*, void* self, int level, const char* text) {
    // libmosquitto is chatty (every PINGREQ, every packet): keep it at trace,
    // its own errors at debug. Our own logger reports what matters.
    auto& log = static_cast<MosquittoClient*>(self)->libLog_;
    if (level & (MOSQ_LOG_ERR | MOSQ_LOG_WARNING)) log->debug("{}", text);
    else log->trace("{}", text);
}

}  // namespace caelitus::mqtt
