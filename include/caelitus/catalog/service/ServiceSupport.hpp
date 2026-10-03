#pragma once

/// @file
/// Helpers shared by the catalog services: clock, paging, events.
/// @ingroup catalog

#include "caelitus/catalog/domain/Query.hpp"
#include "caelitus/core/DateTime.hpp"
#include "caelitus/core/DomainErrors.hpp"
#include "caelitus/mqtt/IMqttClient.hpp"

#include <functional>
#include <memory>
#include <string>

namespace caelitus::catalog {

/// Where services get "now" from; injected so tests control it.
using Clock = std::function<Timestamp()>;
/// The real clock (nowUtc()).
inline Clock systemClock() {
    return [] { return nowUtc(); };
}

/// Checks a requested page.
/// @throws ValidationError for "page" (< 1) or "pageSize" (outside 1..Page::kMaxSize).
inline Page validatePage(const Page& page) {
    if (page.number < 1) throw ValidationError("page", "page number starts at 1");
    if (page.size < 1 || page.size > Page::kMaxSize)
        throw ValidationError("pageSize", "must be between 1 and " + std::to_string(Page::kMaxSize));
    return page;
}

/// Publishes catalog events over MQTT after a commit.
///
/// | Topic                           | Payload                    | When |
/// |---------------------------------|----------------------------|------|
/// | `catalog/books/<id>/created`    | the title                  | BookService::create() |
/// | `catalog/books/<id>/updated`    | the title                  | BookService::update() |
/// | `catalog/books/<id>/deleted`    | empty                      | BookService::remove() |
/// | `catalog/books/<id>/reviews`    | "<rating> <reviewer name>" | ReviewService::add() |
///
/// Best effort, with the client's default QoS and retain flag (config.json:
/// QoS 0, not retained): a missing broker never fails a service
/// call. Optional: without a publisher, events are skipped.
class CatalogEvents {
public:
    /// @param publisher  Where events go; may be null.
    explicit CatalogEvents(std::shared_ptr<mqtt::IMqttPublisher> publisher) : publisher_(std::move(publisher)) {}

    /// Publishes `payload` to "catalog/books/<id>/<event>".
    void bookEvent(BookId id, const std::string& event, const std::string& payload) const {
        if (publisher_) publisher_->publish("catalog/books/" + std::to_string(id.value) + "/" + event, payload);
    }

private:
    std::shared_ptr<mqtt::IMqttPublisher> publisher_;
};

}  // namespace caelitus::catalog
