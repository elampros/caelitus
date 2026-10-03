#pragma once

/// @file
/// MQTT 3.1.1 topic rules, independent of any client library.
/// @ingroup mqtt

#include <string_view>

namespace caelitus::mqtt {

/// Checks a topic to publish to: non-empty, at most 65535 bytes, valid UTF-8,
/// no wildcards, no NUL.
/// @throws MqttError naming the problem.
void validateTopicName(std::string_view topic);

/// Checks a subscription filter: like a topic name, but `+` may fill a whole
/// level and `#` may be the whole last level. "$share/<group>/<filter>" is
/// accepted.
/// @throws MqttError naming the problem.
void validateTopicFilter(std::string_view filter);

/// Whether `topic` matches `filter`.
///
/// Wildcards at the first level do not match topics starting with `$` (e.g.
/// "$SYS/..."), as the specification requires.
/// @code
/// topicMatches("catalog/in/books/+/like", "catalog/in/books/42/like");  // true
/// topicMatches("sensors/#", "sensors");                                  // true
/// @endcode
bool topicMatches(std::string_view filter, std::string_view topic) noexcept;

}  // namespace caelitus::mqtt
