#pragma once

#include "ignis/core/result.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace ignis {

// Parses JSON with a hard nesting-depth cap so hostile input cannot overflow
// the recursive descent parser's stack. Returns failure instead of throwing.
inline Result<nlohmann::json> parse_json_bounded(std::string_view text, int max_depth = 64) {
    const nlohmann::json::parser_callback_t cb =
        [max_depth](int depth, nlohmann::json::parse_event_t event, nlohmann::json&) {
            if (event == nlohmann::json::parse_event_t::object_start ||
                event == nlohmann::json::parse_event_t::array_start) {
                if (depth > max_depth)
                    throw std::runtime_error("json nesting deeper than " +
                                             std::to_string(max_depth) + " levels");
            }
            return true;
        };
    try {
        return Result<nlohmann::json>::success(nlohmann::json::parse(text, cb));
    } catch (const std::exception& e) {
        return Result<nlohmann::json>::failure(std::string("json parse error: ") + e.what());
    }
}

} // namespace ignis
