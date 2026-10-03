#pragma once

/// @file
/// Civil time zones, for "what day is it in Athens".
/// @ingroup core

#include "caelitus/core/DateTime.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace caelitus {

/// A civil time zone: a standard UTC offset plus, optionally, the EU daylight
/// saving rule.
///
/// The EU rule is +1h from the last Sunday of March 01:00 UTC to the last
/// Sunday of October 01:00 UTC. It has applied to every EU zone since 1996, so
/// no tz database is needed. Used to decide which day a like belongs to.
class TimeZone {
public:
    /// UTC, no daylight saving.
    static TimeZone utc();

    /// Looks a zone up by name.
    /// @param name  "UTC" or a European zone such as "Europe/Athens",
    ///              "Europe/Berlin", "Europe/London".
    /// @throws std::invalid_argument for other names.
    static TimeZone named(const std::string& name);

    /// The name it was created with.
    const std::string& name() const noexcept { return name_; }

    /// The UTC offset in effect at `ts` (e.g. +180 minutes for Athens in summer).
    std::chrono::minutes offsetAt(Timestamp ts) const;
    /// The local calendar date at `ts`.
    Date localDate(Timestamp ts) const;
    /// The local calendar date and time of day at `ts`.
    DateTimeParts toLocal(Timestamp ts) const;

    /// The UTC instant of a local date and time.
    ///
    /// On the day clocks go back, a local time between 03:00 and 04:00
    /// (Athens) happens twice; the first occurrence is returned. On the day
    /// clocks go forward, local times in the skipped hour do not exist.
    /// @return std::nullopt for a skipped local time.
    std::optional<Timestamp> toUtc(const DateTimeParts& local) const;

private:
    TimeZone(std::string name, std::chrono::minutes standardOffset, bool euDst)
        : name_(std::move(name)),
          standardOffset_(standardOffset),
          euDst_(euDst) {}

    std::string name_;
    std::chrono::minutes standardOffset_;
    bool euDst_;
};

}  // namespace caelitus
