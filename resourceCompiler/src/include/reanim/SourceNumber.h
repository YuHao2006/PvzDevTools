#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <algorithm>

namespace openpvz::reanim {

// Source values are ordinary finite floats; no source precision grid applies.
inline float ParseFiniteFloat(std::string_view text)
{
    const auto original = text;
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    float value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(value))
        throw std::runtime_error("expected finite decimal float: '" + std::string(original) + "'");
    return value;
}

inline std::int32_t ParseDecimalInteger(std::string_view text)
{
    if (text.find('.') != std::string_view::npos)
        throw std::runtime_error("expected integer: '" + std::string(text) + "'");
    std::int32_t value = 0;
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("expected int32 integer: '" + std::string(text) + "'");
    return value;
}

// Preserve exact small decimal timestamp ratios and implicit thirds. Longer
// source decimals use a bounded continued fraction, without a thousandth grid.
inline std::pair<std::int32_t, std::int32_t> ParseTimeRatio(std::string_view text,
                                                         std::int64_t divisor = 1)
{
    (void)ParseFiniteFloat(text);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    double value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || divisor <= 0)
        throw std::runtime_error("invalid particle node time");
    value /= static_cast<double>(divisor);
    const bool negative = value < 0;
    double remaining = std::abs(value);
    std::int64_t p0 = 0, p1 = 1, q0 = 1, q1 = 0;
    for (int step = 0; step < 64; ++step) {
        const double integral = std::floor(remaining);
        if (integral > INT32_MAX) break;
        const auto a = static_cast<std::int64_t>(integral);
        if ((p1 && a > (INT32_MAX - p0) / p1) ||
            (q1 && a > (INT32_MAX - q0) / q1)) break;
        const auto p = a * p1 + p0, q = a * q1 + q0;
        p0 = p1; p1 = p; q0 = q1; q1 = q;
        const double fraction = remaining - integral;
        if (fraction <= std::numeric_limits<double>::epsilon() * std::max(1.0, remaining) ||
            (q1 && std::abs(static_cast<double>(p1) / q1 - std::abs(value)) <=
             std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(value)))) break;
        remaining = 1.0 / fraction;
    }
    if (q1 <= 0 || (value != 0 && p1 == 0))
        throw std::runtime_error("particle node time exceeds rational storage range");
    return {static_cast<std::int32_t>(negative ? -p1 : p1), static_cast<std::int32_t>(q1)};
}
}
