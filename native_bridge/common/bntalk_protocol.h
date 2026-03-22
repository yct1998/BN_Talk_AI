#pragma once

#include <string>

namespace bntalk::bridge {

struct RouteDecision {
    bool ok = false;
    std::string provider = "native_bridge";
    std::string topic_id = "TALK_BNTALK_NEUTRAL";
    std::string mode = "topic_route";
    std::string reason = "default";
    std::string generated_text;
    std::string debug;
    int emotion_delta = 0;
    std::string request_id;
    bool ready = true;
};

RouteDecision route_from_context_json( const std::string &request_json );
bool parse_route_result_json( const std::string &response_json, RouteDecision &out );
std::string route_json_or_fallback( const std::string &request_json, std::string &transport_error );
std::string make_status_json( bool attached, const std::string &error );

}  // namespace bntalk::bridge
