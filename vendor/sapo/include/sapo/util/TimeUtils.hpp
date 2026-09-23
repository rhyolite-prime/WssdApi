//
//  Sapo Engine — duration / timestamp parsing helpers (T2.2, T2.3).
//
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sapo::util {

    using namespace std::chrono_literals;

    /**
     * @brief Parses human durations: "500ms", "30s", "10m", "2h", "1d",
     *        "1h30m", "90" (bare number = seconds).
     * @return std::nullopt when the input is malformed.
     */
    [[nodiscard]] std::optional<std::chrono::milliseconds> parseDuration(std::string_view text);

    /** Parses "in 5 minutes" / "in 30s" style one-shot delays. */
    [[nodiscard]] std::optional<std::chrono::milliseconds> parseRelativeDelay(std::string_view text);

    /// ISO-8601 (UTC) timestamp -> epoch milliseconds. Accepts date-only form.
    [[nodiscard]] std::optional<int64_t> parseIso8601(std::string_view text);

    /// epoch milliseconds -> ISO-8601 UTC string (millisecond precision).
    [[nodiscard]] std::string formatIso8601(int64_t epoch_ms);

    /// Fixed-offset time zones ("UTC", "+03:00", "-0500"); named zones are
    /// resolved by the host clock, see docs/DSL_GRAMMAR_v1.md.
    struct ZoneOffset {
        int seconds{0};
        [[nodiscard]] std::string toString() const;
    };

    /// Returns nullopt for unknown/unsupported zone identifiers.
    [[nodiscard]] std::optional<ZoneOffset> parseZoneOffset(std::string_view zone);

    struct BrokenDownTime {
        int year{1970}, month{1}, day{1}, hour{0}, minute{0}, second{0}, weekday{4}; // 0 = Sunday
    };

    [[nodiscard]] BrokenDownTime breakDown(int64_t epoch_ms, int offset_seconds);
    [[nodiscard]] int64_t compose(const BrokenDownTime &tm, int offset_seconds);

} // namespace sapo::util
