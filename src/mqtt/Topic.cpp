#include "caelitus/mqtt/Topic.hpp"

#include "caelitus/mqtt/MqttTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace caelitus::mqtt {

namespace {

constexpr std::size_t kMaxTopicLength = 65535;
constexpr std::string_view kSharePrefix = "$share/";

bool isValidUtf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra;
        std::uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= s.size()) return false;  // truncated sequence
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong encodings, surrogates and out-of-range code points.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            return false;
        i += extra + 1;
    }
    return true;
}

void validateCommon(std::string_view text, const char* what) {
    if (text.empty()) throw MqttError(std::string(what) + " must not be empty");
    if (text.size() > kMaxTopicLength)
        throw MqttError(std::string(what) + " longer than " + std::to_string(kMaxTopicLength) + " bytes");
    if (text.find('\0') != std::string_view::npos) throw MqttError(std::string(what) + " contains a NUL character");
    if (!isValidUtf8(text)) throw MqttError(std::string(what) + " is not valid UTF-8");
}

// Splits on '/' and calls f(level, isLast) for each level.
template <typename F>
void forEachLevel(std::string_view s, F&& f) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = s.find('/', start);
        if (slash == std::string_view::npos) {
            f(s.substr(start), true);
            return;
        }
        f(s.substr(start, slash - start), false);
        start = slash + 1;
    }
}

}  // namespace

void validateTopicName(std::string_view topic) {
    validateCommon(topic, "Topic");
    if (topic.find_first_of("+#") != std::string_view::npos)
        throw MqttError("Topic '" + std::string(topic) + "' must not contain wildcards (+ or #)");
}

void validateTopicFilter(std::string_view filter) {
    validateCommon(filter, "Topic filter");
    std::string_view body = filter;
    if (filter.compare(0, kSharePrefix.size(), kSharePrefix) == 0) {
        const std::size_t groupEnd = filter.find('/', kSharePrefix.size());
        const std::string_view group = filter.substr(kSharePrefix.size(), groupEnd - kSharePrefix.size());
        if (groupEnd == std::string_view::npos || group.empty() ||
            group.find_first_of("+#") != std::string_view::npos || groupEnd + 1 >= filter.size())
            throw MqttError("Invalid shared subscription '" + std::string(filter) +
                            "' (expected $share/<group>/<filter>)");
        body = filter.substr(groupEnd + 1);
    }

    forEachLevel(body, [&](std::string_view level, bool last) {
        const bool hasWildcard = level.find_first_of("+#") != std::string_view::npos;
        if (!hasWildcard) return;
        if (level == "+") return;
        if (level == "#" && last) return;
        throw MqttError("Invalid topic filter '" + std::string(filter) +
                        "': '+' must fill a whole level and '#' must be the whole last level");
    });
}

bool topicMatches(std::string_view filter, std::string_view topic) noexcept {
    if (filter.compare(0, kSharePrefix.size(), kSharePrefix) == 0) {
        const std::size_t groupEnd = filter.find('/', kSharePrefix.size());
        if (groupEnd == std::string_view::npos) return false;
        filter = filter.substr(groupEnd + 1);
    }
    if (!topic.empty() && topic[0] == '$' && !filter.empty() && (filter[0] == '+' || filter[0] == '#')) return false;

    std::size_t f = 0, t = 0;
    for (;;) {
        const std::size_t fEnd = std::min(filter.find('/', f), filter.size());
        const std::string_view fLevel = filter.substr(f, fEnd - f);
        if (fLevel == "#") return true;  // matches the parent level too ("a/#" matches "a")

        const std::size_t tEnd = std::min(topic.find('/', t), topic.size());
        const std::string_view tLevel = topic.substr(t, tEnd - t);
        if (fLevel != "+" && fLevel != tLevel) return false;

        const bool fDone = fEnd == filter.size();
        const bool tDone = tEnd == topic.size();
        if (fDone || tDone) {
            if (fDone && tDone) return true;
            // "a/#" matches "a": filter has exactly "/#" left
            return tDone && filter.substr(fEnd) == "/#";
        }
        f = fEnd + 1;
        t = tEnd + 1;
    }
}

}  // namespace caelitus::mqtt
