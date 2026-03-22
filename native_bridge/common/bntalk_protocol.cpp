#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include "bntalk_protocol.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace bntalk::bridge {
namespace {

bool contains_ci( std::string haystack, const std::string &needle ) {
    std::transform( haystack.begin(), haystack.end(), haystack.begin(), []( unsigned char c ) {
        return static_cast<char>( std::tolower( c ) );
    } );
    std::string lower_needle( needle );
    std::transform( lower_needle.begin(), lower_needle.end(), lower_needle.begin(), []( unsigned char c ) {
        return static_cast<char>( std::tolower( c ) );
    } );
    return haystack.find( lower_needle ) != std::string::npos;
}

std::string json_escape( const std::string &text ) {
    std::string out;
    out.reserve( text.size() + 8 );
    for( const char c : text ) {
        switch( c ) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::size_t skip_ws( const std::string &text, std::size_t pos ) {
    while( pos < text.size() && std::isspace( static_cast<unsigned char>( text[pos] ) ) ) {
        ++pos;
    }
    return pos;
}

bool find_value_pos( const std::string &payload, const std::string &key, std::size_t &value_pos ) {
    const std::string needle = '"' + key + '"';
    const std::size_t key_pos = payload.find( needle );
    if( key_pos == std::string::npos ) {
        return false;
    }
    const std::size_t colon_pos = payload.find( ':', key_pos + needle.size() );
    if( colon_pos == std::string::npos ) {
        return false;
    }
    value_pos = skip_ws( payload, colon_pos + 1 );
    return value_pos < payload.size();
}

std::string json_unescape( const std::string &text ) {
    std::string out;
    out.reserve( text.size() );
    bool escape = false;
    for( const char c : text ) {
        if( escape ) {
            switch( c ) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: out += c; break;
            }
            escape = false;
            continue;
        }
        if( c == '\\' ) {
            escape = true;
            continue;
        }
        out += c;
    }
    if( escape ) {
        out += '\\';
    }
    return out;
}

bool extract_json_string( const std::string &payload, const std::string &key, std::string &out ) {
    std::size_t pos = 0;
    if( !find_value_pos( payload, key, pos ) || payload[pos] != '"' ) {
        return false;
    }
    ++pos;
    std::string raw;
    bool escape = false;
    while( pos < payload.size() ) {
        const char c = payload[pos++];
        if( escape ) {
            raw += '\\';
            raw += c;
            escape = false;
            continue;
        }
        if( c == '\\' ) {
            escape = true;
            continue;
        }
        if( c == '"' ) {
            out = json_unescape( raw );
            return true;
        }
        raw += c;
    }
    return false;
}

bool extract_json_bool( const std::string &payload, const std::string &key, bool &out ) {
    std::size_t pos = 0;
    if( !find_value_pos( payload, key, pos ) ) {
        return false;
    }
    if( payload.compare( pos, 4, "true" ) == 0 ) {
        out = true;
        return true;
    }
    if( payload.compare( pos, 5, "false" ) == 0 ) {
        out = false;
        return true;
    }
    return false;
}

bool extract_json_int( const std::string &payload, const std::string &key, int &out ) {
    std::size_t pos = 0;
    if( !find_value_pos( payload, key, pos ) ) {
        return false;
    }

    std::size_t end = pos;
    if( end < payload.size() && ( payload[end] == '-' || payload[end] == '+' ) ) {
        ++end;
    }
    const std::size_t digits_begin = end;
    while( end < payload.size() && std::isdigit( static_cast<unsigned char>( payload[end] ) ) ) {
        ++end;
    }
    if( end == digits_begin ) {
        return false;
    }

    try {
        out = std::stoi( payload.substr( pos, end - pos ) );
        return true;
    } catch( ... ) {
        return false;
    }
}

std::wstring utf8_to_wide( const std::string &text ) {
    if( text.empty() ) {
        return L"";
    }
    const int size = MultiByteToWideChar( CP_UTF8, 0, text.c_str(), static_cast<int>( text.size() ), nullptr, 0 );
    if( size <= 0 ) {
        return L"";
    }
    std::wstring out( size, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, text.c_str(), static_cast<int>( text.size() ), &out[0], size );
    return out;
}

bool http_post_json( const std::string &url, const std::string &payload, int timeout_ms, std::string &response, std::string &error ) {
    const std::wstring wide_url = utf8_to_wide( url );
    if( wide_url.empty() ) {
        error = "sidecar_url_invalid";
        return false;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof( parts );
    parts.dwSchemeLength = static_cast<DWORD>( -1 );
    parts.dwHostNameLength = static_cast<DWORD>( -1 );
    parts.dwUrlPathLength = static_cast<DWORD>( -1 );
    parts.dwExtraInfoLength = static_cast<DWORD>( -1 );
    if( !WinHttpCrackUrl( wide_url.c_str(), 0, 0, &parts ) ) {
        error = "sidecar_url_crack_failed";
        return false;
    }

    const std::wstring host( parts.lpszHostName, parts.dwHostNameLength );
    std::wstring path = parts.dwUrlPathLength > 0 ? std::wstring( parts.lpszUrlPath, parts.dwUrlPathLength ) : L"/";
    if( parts.dwExtraInfoLength > 0 ) {
        path += std::wstring( parts.lpszExtraInfo, parts.dwExtraInfoLength );
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen( L"BN Talk Native Bridge/0.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    if( session == nullptr ) {
        error = "winhttp_open_failed";
        return false;
    }

    if( timeout_ms > 0 ) {
        WinHttpSetTimeouts( session, timeout_ms, timeout_ms, timeout_ms, timeout_ms );
    }

    HINTERNET connection = WinHttpConnect( session, host.c_str(), parts.nPort, 0 );
    if( connection == nullptr ) {
        error = "winhttp_connect_failed";
        WinHttpCloseHandle( session );
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"POST",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0
    );
    if( request == nullptr ) {
        error = "winhttp_open_request_failed";
        WinHttpCloseHandle( connection );
        WinHttpCloseHandle( session );
        return false;
    }

    const wchar_t *headers = L"Content-Type: application/json\r\n";
    const BOOL sent = WinHttpSendRequest(
        request,
        headers,
        static_cast<DWORD>( -1 ),
        const_cast<char *>( payload.data() ),
        static_cast<DWORD>( payload.size() ),
        static_cast<DWORD>( payload.size() ),
        0
    );
    if( !sent ) {
        error = "winhttp_send_failed";
        WinHttpCloseHandle( request );
        WinHttpCloseHandle( connection );
        WinHttpCloseHandle( session );
        return false;
    }

    if( !WinHttpReceiveResponse( request, nullptr ) ) {
        error = "winhttp_receive_failed";
        WinHttpCloseHandle( request );
        WinHttpCloseHandle( connection );
        WinHttpCloseHandle( session );
        return false;
    }

    DWORD status_code = 0;
    DWORD status_code_size = sizeof( status_code );
    WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size, WINHTTP_NO_HEADER_INDEX );

    response.clear();
    for( ;; ) {
        DWORD available = 0;
        if( !WinHttpQueryDataAvailable( request, &available ) ) {
            error = "winhttp_query_data_failed";
            break;
        }
        if( available == 0 ) {
            break;
        }

        std::vector<char> buffer( available + 1, '\0' );
        DWORD read = 0;
        if( !WinHttpReadData( request, buffer.data(), available, &read ) ) {
            error = "winhttp_read_failed";
            break;
        }
        response.append( buffer.data(), buffer.data() + read );
    }

    WinHttpCloseHandle( request );
    WinHttpCloseHandle( connection );
    WinHttpCloseHandle( session );

    if( !error.empty() ) {
        return false;
    }
    if( status_code < 200 || status_code >= 300 ) {
        error = "sidecar_http_status_" + std::to_string( status_code );
        return false;
    }
    if( response.empty() ) {
        error = "sidecar_empty_response";
        return false;
    }
    return true;
}

}  // namespace

RouteDecision route_from_context_json( const std::string &request_json ) {
    RouteDecision out;
    out.ok = true;
    out.provider = "native_context_router";

    std::string phase = "dialogue_start";
    std::string npc_name = "someone";
    std::string player_name = "you";
    std::string last_topic;
    std::string last_event;
    std::string last_seen_turn;
    std::string preferred_dynamic_topic = "TALK_BNTALK_DYNAMIC";
    std::string request_kind = "route";
    std::string utterance;
    std::string interaction_intent = "talk";
    bool following = false;
    bool friend_flag = false;
    bool enemy = false;
    bool allow_dynamic_text = false;
    bool bridge_is_enemy = false;
    bool bridge_is_following = false;
    int affinity = 0;
    int times_spoken = 0;
    int protocol = 0;
    int social_skill = 0;

    extract_json_string( request_json, "phase", phase );
    extract_json_string( request_json, "npc_name", npc_name );
    extract_json_string( request_json, "player_name", player_name );
    extract_json_string( request_json, "last_topic", last_topic );
    extract_json_string( request_json, "last_event", last_event );
    extract_json_string( request_json, "last_seen_turn", last_seen_turn );
    extract_json_string( request_json, "preferred_dynamic_topic", preferred_dynamic_topic );
    extract_json_string( request_json, "request_kind", request_kind );
    extract_json_string( request_json, "utterance", utterance );
    extract_json_string( request_json, "interaction_intent", interaction_intent );
    extract_json_bool( request_json, "following", following );
    extract_json_bool( request_json, "friend", friend_flag );
    extract_json_bool( request_json, "enemy", enemy );
    extract_json_bool( request_json, "allow_dynamic_text", allow_dynamic_text );
    extract_json_bool( request_json, "is_enemy", bridge_is_enemy );
    extract_json_bool( request_json, "is_following", bridge_is_following );
    extract_json_int( request_json, "affinity", affinity );
    extract_json_int( request_json, "times_spoken", times_spoken );
    extract_json_int( request_json, "protocol", protocol );
    extract_json_int( request_json, "social_skill", social_skill );

    if( preferred_dynamic_topic.empty() ) {
        preferred_dynamic_topic = "TALK_BNTALK_DYNAMIC";
    }
    if( interaction_intent.empty() ) {
        interaction_intent = "talk";
    }
    if( bridge_is_enemy ) {
        enemy = true;
    }
    if( bridge_is_following ) {
        following = true;
    }
    if( player_name.empty() ) {
        player_name = "you";
    }

    out.topic_id = preferred_dynamic_topic;
    out.mode = "dynamic_topic";
    out.reason = "default_dynamic";
    out.emotion_delta = 0;

    if( request_kind == "utterance_reply" || !utterance.empty() ) {
        out.topic_id = preferred_dynamic_topic;
        out.mode = "utterance_reply";
        out.reason = "utterance_reply";
        if( interaction_intent == "beg" ) {
            if( enemy ) {
                out.generated_text = npc_name + " stares at you and weighs whether to spare you for a moment.";
                out.emotion_delta = -1;
            } else {
                out.generated_text = npc_name + " looks confused, because you are not actually under attack right now.";
                out.emotion_delta = 0;
            }
        } else if( interaction_intent == "recruit" ) {
            if( enemy ) {
                out.generated_text = npc_name + " coldly rejects your invitation and has no intention of going with you.";
                out.emotion_delta = -2;
            } else if( friend_flag || affinity >= 4 || social_skill >= 4 ) {
                out.generated_text = npc_name + " listens to your invitation seriously and is clearly not completely uninterested.";
                out.emotion_delta = 1;
            } else {
                out.generated_text = npc_name + " hears your invitation, but their attitude remains cautious and reserved.";
                out.emotion_delta = 0;
            }
        } else {
            out.generated_text = npc_name + " heard you say '" + utterance + "' and gives a thoughtful reply.";
            if( friend_flag || affinity >= 4 ) {
                out.generated_text = npc_name + " smiles faintly at your words: '" + utterance + "'...  I appreciate that.";
                out.emotion_delta = 1;
            } else if( enemy || affinity <= -3 ) {
                out.generated_text = npc_name + " narrows their eyes after hearing '" + utterance + "'.  'Watch your tone.'";
                out.emotion_delta = -1;
            }
        }
    } else if( phase == "self_test" ) {
        out.reason = "self_test";
        out.generated_text = "Sidecar self-test succeeded for " + npc_name + " using protocol " + std::to_string( protocol ) + ".";
    } else if( enemy ) {
        out.topic_id = "TALK_BNTALK_ENEMY";
        out.mode = "topic_route";
        out.reason = "enemy";
        out.generated_text = npc_name + " replies with immediate hostility and treats " + player_name + " as a threat.";
    } else if( following ) {
        out.topic_id = "TALK_BNTALK_FOLLOWER";
        out.mode = "topic_route";
        out.reason = "following";
        out.generated_text = npc_name + " is already following your lead and waits for the next concrete order.";
    } else if( times_spoken <= 1 ) {
        out.topic_id = "TALK_BNTALK_FIRST_MEET";
        out.mode = "topic_route";
        out.reason = "first_meet";
        out.generated_text = npc_name + " sounds cautious, like this first real exchange still needs to earn trust.";
    } else if( affinity <= -3 ) {
        out.topic_id = "TALK_BNTALK_NEUTRAL";
        out.mode = "topic_route";
        out.reason = "low_affinity";
        out.generated_text = npc_name + " keeps the answer short and guarded, clearly not ready to open up.";
    } else if( friend_flag || affinity >= 6 ) {
        out.reason = friend_flag ? "friend_dynamic" : "high_affinity_dynamic";
        out.generated_text = npc_name + " speaks to " + player_name + " with the ease of someone who has learned to rely on you.";
    } else if( last_topic == "TALK_BNTALK_SMALLTALK" ) {
        out.reason = "smalltalk_followup";
        out.generated_text = npc_name + " smoothly continues the earlier small talk instead of resetting the mood.";
    } else if( last_event == "dialogue_end" ) {
        out.reason = "resume_after_dialogue";
        out.generated_text = npc_name + " resumes the conversation as if the last exchange is still fresh.";
    } else if( contains_ci( request_json, "enemy" ) && !contains_ci( request_json, "\"enemy\":false" ) ) {
        out.topic_id = "TALK_BNTALK_ENEMY";
        out.mode = "topic_route";
        out.reason = "enemy_fallback_match";
        out.generated_text = "I do not trust you, but I am still listening.";
    } else {
        out.generated_text = npc_name + " answers in a grounded way shaped by the current BN Talk sidecar router.";
    }

    if( !allow_dynamic_text && out.mode == "dynamic_topic" ) {
        out.generated_text.clear();
    }

    out.debug =
        "phase=" + phase +
        ";affinity=" + std::to_string( affinity ) +
        ";spoken=" + std::to_string( times_spoken ) +
        ";friend=" + std::string( friend_flag ? "true" : "false" ) +
        ";following=" + std::string( following ? "true" : "false" ) +
        ";enemy=" + std::string( enemy ? "true" : "false" ) +
        ";intent=" + interaction_intent +
        ";social_skill=" + std::to_string( social_skill ) +
        ";bridge_is_enemy=" + std::string( bridge_is_enemy ? "true" : "false" ) +
        ";bridge_is_following=" + std::string( bridge_is_following ? "true" : "false" ) +
        ";last_topic=" + last_topic +
        ";last_event=" + last_event +
        ";last_seen_turn=" + last_seen_turn;

    return out;
}

std::string make_route_json_local( const RouteDecision &decision ) {
    std::string json = "{";
    json += "\"provider\":\"" + json_escape( decision.provider ) + "\",";
    json += "\"topic_id\":\"" + json_escape( decision.topic_id ) + "\",";
    json += "\"mode\":\"" + json_escape( decision.mode ) + "\",";
    json += "\"reason\":\"" + json_escape( decision.reason ) + "\",";
    json += "\"generated_text\":\"" + json_escape( decision.generated_text ) + "\",";
    json += "\"debug\":\"" + json_escape( decision.debug ) + "\",";
    json += "\"emotion_delta\":" + std::to_string( decision.emotion_delta ) + ",";
    json += "\"request_id\":\"" + json_escape( decision.request_id ) + "\",";
    json += "\"ready\":" + std::string( decision.ready ? "true" : "false" );
    json += "}";
    return json;
}

bool parse_route_result_json( const std::string &response_json, RouteDecision &out ) {
    bool any = false;
    any = extract_json_string( response_json, "provider", out.provider ) || any;
    any = extract_json_string( response_json, "topic_id", out.topic_id ) || any;
    any = extract_json_string( response_json, "mode", out.mode ) || any;
    any = extract_json_string( response_json, "reason", out.reason ) || any;
    any = extract_json_string( response_json, "generated_text", out.generated_text ) || any;
    any = extract_json_string( response_json, "debug", out.debug ) || any;
    any = extract_json_int( response_json, "emotion_delta", out.emotion_delta ) || any;
    any = extract_json_string( response_json, "request_id", out.request_id ) || any;
    any = extract_json_bool( response_json, "ready", out.ready ) || any;
    return any;
}

std::string route_json_or_fallback( const std::string &request_json, std::string &transport_error ) {
    bool sidecar_enabled = true;
    std::string sidecar_url = "http://127.0.0.1:45123/route";
    int sidecar_timeout_ms = 3000;

    extract_json_bool( request_json, "sidecar_enabled", sidecar_enabled );
    extract_json_string( request_json, "sidecar_url", sidecar_url );
    extract_json_int( request_json, "sidecar_timeout_ms", sidecar_timeout_ms );

    if( sidecar_enabled ) {
        std::string response;
        if( http_post_json( sidecar_url, request_json, sidecar_timeout_ms, response, transport_error ) ) {
            if( !response.empty() && response.front() == '{' ) {
                RouteDecision parsed;
                if( parse_route_result_json( response, parsed ) ) {
                    return response;
                }
                transport_error = "sidecar_response_invalid";
            } else {
                transport_error = "sidecar_response_not_json_object";
            }
        }
    } else {
        transport_error = "sidecar_disabled";
    }

    RouteDecision fallback = route_from_context_json( request_json );
    fallback.provider = "native_context_router_fallback";
    if( !transport_error.empty() ) {
        if( !fallback.debug.empty() ) {
            fallback.debug += ';';
        }
        fallback.debug += "transport_error=" + transport_error;
    }
    return make_route_json_local( fallback );
}

std::string make_status_json( bool attached, const std::string &error ) {
    std::ostringstream os;
    os << "{";
    os << "\"available\":" << ( attached ? "true" : "false" ) << ",";
    os << "\"provider\":\"native_context_router\",";
    os << "\"version\":\"0.4.0\",";
    os << "\"bridge_mode\":\"sidecar_router\",";
    os << "\"error\":\"" << json_escape( error ) << "\"";
    os << "}";
    return os.str();
}

}  // namespace bntalk::bridge
