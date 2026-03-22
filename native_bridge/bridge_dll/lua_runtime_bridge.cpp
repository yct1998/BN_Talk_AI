#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../common/bntalk_protocol.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
struct lua_State;
typedef ptrdiff_t lua_Integer;
typedef int ( __cdecl *lua_CFunction )( lua_State *L );

int lua_gettop( lua_State *L );
void lua_settop( lua_State *L, int idx );
void lua_createtable( lua_State *L, int narr, int nrec );
void lua_pushboolean( lua_State *L, int b );
const char *lua_pushstring( lua_State *L, const char *s );
void lua_pushinteger( lua_State *L, lua_Integer n );
void lua_pushnil( lua_State *L );
void lua_pushcclosure( lua_State *L, lua_CFunction fn, int n );
int lua_type( lua_State *L, int idx );
const char *lua_typename( lua_State *L, int tp );
int lua_getglobal( lua_State *L, const char *name );
void lua_getfield( lua_State *L, int idx, const char *k );
void lua_setglobal( lua_State *L, const char *name );
void lua_setfield( lua_State *L, int idx, const char *k );
int lua_toboolean( lua_State *L, int idx );
const char *lua_tolstring( lua_State *L, int idx, size_t *len );
lua_Integer lua_tointegerx( lua_State *L, int idx, int *isnum );
double lua_tonumberx( lua_State *L, int idx, int *isnum );
int lua_isinteger( lua_State *L, int idx );
}

namespace {
constexpr int LUA_REGISTRYINDEX = -1001000;
constexpr int LUA_TBOOLEAN = 1;
constexpr int LUA_TNUMBER = 3;
constexpr int LUA_TSTRING = 4;
constexpr int LUA_TTABLE = 5;
constexpr int LUA_TFUNCTION = 6;

inline void lua_newtable( lua_State *L ) {
    lua_createtable( L, 0, 0 );
}

inline void lua_pushcfunction( lua_State *L, lua_CFunction fn ) {
    lua_pushcclosure( L, fn, 0 );
}

inline void lua_pop( lua_State *L, int n ) {
    lua_settop( L, -n - 1 );
}

inline const char *lua_tostring( lua_State *L, int idx ) {
    return lua_tolstring( L, idx, nullptr );
}

inline lua_Integer lua_tointeger( lua_State *L, int idx ) {
    return lua_tointegerx( L, idx, nullptr );
}

inline double lua_tonumber( lua_State *L, int idx ) {
    return lua_tonumberx( L, idx, nullptr );
}

inline int lua_istable( lua_State *L, int idx ) {
    return lua_type( L, idx ) == LUA_TTABLE;
}

inline int lua_isstring( lua_State *L, int idx ) {
    const int type = lua_type( L, idx );
    return type == LUA_TSTRING || type == LUA_TNUMBER;
}

inline int lua_isnumber( lua_State *L, int idx ) {
    return lua_type( L, idx ) == LUA_TNUMBER;
}

inline int lua_isboolean( lua_State *L, int idx ) {
    return lua_type( L, idx ) == LUA_TBOOLEAN;
}
}  // namespace

namespace bntalk::bridge::lua_runtime {
namespace {

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

std::string make_result_json( bool ok, const std::string &details ) {
    std::string json = "{";
    json += "\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"details\":\"" + json_escape( details ) + "\"";
    json += "}";
    return json;
}

int absolute_index( lua_State *L, int index ) {
    if( index > 0 || index <= LUA_REGISTRYINDEX ) {
        return index;
    }
    return lua_gettop( L ) + index + 1;
}

bool table_get_string_field( lua_State *L, int table_index, const char *field, std::string &out ) {
    table_index = absolute_index( L, table_index );
    lua_getfield( L, table_index, field );
    const bool ok = lua_isstring( L, -1 );
    if( ok ) {
        out = lua_tostring( L, -1 );
    }
    lua_pop( L, 1 );
    return ok;
}

bool table_get_bool_field( lua_State *L, int table_index, const char *field, bool &out ) {
    table_index = absolute_index( L, table_index );
    lua_getfield( L, table_index, field );
    const bool ok = lua_isboolean( L, -1 ) || lua_isnumber( L, -1 );
    if( ok ) {
        out = lua_toboolean( L, -1 ) != 0;
    }
    lua_pop( L, 1 );
    return ok;
}

bool table_get_int_field( lua_State *L, int table_index, const char *field, int &out ) {
    table_index = absolute_index( L, table_index );
    lua_getfield( L, table_index, field );
    bool ok = false;
    if( lua_isinteger( L, -1 ) ) {
        out = static_cast<int>( lua_tointeger( L, -1 ) );
        ok = true;
    } else if( lua_isnumber( L, -1 ) ) {
        out = static_cast<int>( lua_tonumber( L, -1 ) );
        ok = true;
    }
    lua_pop( L, 1 );
    return ok;
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

std::string wide_to_utf8( const std::wstring &text ) {
    if( text.empty() ) {
        return "";
    }
    const int size = WideCharToMultiByte( CP_UTF8, 0, text.c_str(), static_cast<int>( text.size() ), nullptr, 0, nullptr, nullptr );
    if( size <= 0 ) {
        return "";
    }
    std::string out( size, '\0' );
    WideCharToMultiByte( CP_UTF8, 0, text.c_str(), static_cast<int>( text.size() ), &out[0], size, nullptr, nullptr );
    return out;
}

std::wstring ps_single_quote( const std::wstring &text ) {
    std::wstring out;
    out.reserve( text.size() + 8 );
    for( const wchar_t c : text ) {
        if( c == L'\'' ) {
            out += L"''";
        } else {
            out += c;
        }
    }
    return out;
}

std::string read_utf8_file( const std::wstring &path ) {
    std::ifstream file( path, std::ios::binary );
    if( !file ) {
        return "";
    }
    std::string data( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    if( data.size() >= 3 &&
        static_cast<unsigned char>( data[0] ) == 0xEF &&
        static_cast<unsigned char>( data[1] ) == 0xBB &&
        static_cast<unsigned char>( data[2] ) == 0xBF ) {
        data.erase( 0, 3 );
    }
    return data;
}

bool prompt_text_with_powershell( const std::string &prompt, const std::string &title,
                                  const std::string &default_value, std::string &out ) {
    wchar_t temp_dir[MAX_PATH] = {};
    if( GetTempPathW( MAX_PATH, temp_dir ) == 0 ) {
        return false;
    }

    wchar_t temp_file[MAX_PATH] = {};
    if( GetTempFileNameW( temp_dir, L"bnt", 0, temp_file ) == 0 ) {
        return false;
    }

    const std::wstring prompt_w = ps_single_quote( utf8_to_wide( prompt ) );
    const std::wstring title_w = ps_single_quote( utf8_to_wide( title ) );
    const std::wstring default_w = ps_single_quote( utf8_to_wide( default_value ) );
    const std::wstring file_w = ps_single_quote( temp_file );

    std::wstring command =
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -STA -WindowStyle Hidden -Command "
        L"\"Add-Type -AssemblyName Microsoft.VisualBasic; "
        L"$enc = New-Object System.Text.UTF8Encoding($false); "
        L"$r = [Microsoft.VisualBasic.Interaction]::InputBox('" + prompt_w + L"','" + title_w + L"','" + default_w + L"'); "
        L"[System.IO.File]::WriteAllText('" + file_w + L"',$r,$enc)\"";

    std::vector<wchar_t> command_buffer( command.begin(), command.end() );
    command_buffer.push_back( L'\0' );

    STARTUPINFOW si{};
    si.cb = sizeof( si );
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW( nullptr, command_buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi );
    if( !created ) {
        DeleteFileW( temp_file );
        return false;
    }

    WaitForSingleObject( pi.hProcess, INFINITE );

    DWORD exit_code = 0;
    GetExitCodeProcess( pi.hProcess, &exit_code );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );

    out = read_utf8_file( temp_file );
    DeleteFileW( temp_file );
    return exit_code == 0;
}

void append_json_string_field( std::string &json, bool &first, const char *key, const std::string &value ) {
    if( !first ) {
        json += ',';
    }
    first = false;
    json += '"';
    json += key;
    json += "\":\"";
    json += json_escape( value );
    json += '"';
}

void append_json_bool_field( std::string &json, bool &first, const char *key, bool value ) {
    if( !first ) {
        json += ',';
    }
    first = false;
    json += '"';
    json += key;
    json += "\":";
    json += value ? "true" : "false";
}

void append_json_int_field( std::string &json, bool &first, const char *key, int value ) {
    if( !first ) {
        json += ',';
    }
    first = false;
    json += '"';
    json += key;
    json += "\":";
    json += std::to_string( value );
}

int l_bntalk_native_prompt_text( lua_State *L ) {
    std::string prompt = "Input what you say to the selected NPC.";
    std::string title = "BN Talk";
    std::string default_value;

    if( lua_isstring( L, 1 ) ) {
        prompt = lua_tostring( L, 1 );
    }
    if( lua_isstring( L, 2 ) ) {
        title = lua_tostring( L, 2 );
    }
    if( lua_isstring( L, 3 ) ) {
        default_value = lua_tostring( L, 3 );
    }

    std::string result;
    if( !prompt_text_with_powershell( prompt, title, default_value, result ) ) {
        lua_pushnil( L );
        return 1;
    }

    lua_pushstring( L, result.c_str() );
    return 1;
}

int l_bntalk_native_status( lua_State *L ) {
    lua_newtable( L );

    lua_pushboolean( L, 1 );
    lua_setfield( L, -2, "available" );

    lua_pushstring( L, "native_runtime_bridge" );
    lua_setfield( L, -2, "provider" );

    lua_pushstring( L, "0.3.0" );
    lua_setfield( L, -2, "version" );

    lua_pushstring( L, "lua_registered" );
    lua_setfield( L, -2, "bridge_mode" );

    lua_pushboolean( L, 1 );
    lua_setfield( L, -2, "bootstrap_complete" );

    lua_pushboolean( L, 1 );
    lua_setfield( L, -2, "lua_bridge_registered" );

    lua_pushboolean( L, 1 );
    lua_setfield( L, -2, "has_lua_state" );

    lua_pushstring( L, "" );
    lua_setfield( L, -2, "error" );

    return 1;
}

int l_bntalk_native_route( lua_State *L ) {
    const int request_index = 1;

    std::string phase = "dialogue_start";
    std::string npc_name = "someone";
    std::string player_name = "you";
    std::string last_topic;
    std::string last_event;
    std::string last_seen_turn;
    bool following = false;
    bool friend_flag = false;
    bool enemy = false;
    bool allow_dynamic_text = false;
    bool sidecar_enabled = true;
    int affinity = 0;
    int times_spoken = 0;
    int protocol = 0;
    int sidecar_timeout_ms = 3000;
    std::string preferred_dynamic_topic = "TALK_BNTALK_DYNAMIC";
    std::string sidecar_url = "http://127.0.0.1:45123/route";
    std::string request_kind = "route";
    std::string utterance;
    std::string request_id;
    std::string interaction_intent = "talk";
    std::string planned_intent;
    std::string planned_outcome;
    std::string planned_summary;
    std::string planned_debug;
    std::string planned_tone;
    int planned_forgive_chance = 0;
    int planned_mercy_chance = 0;
    int planned_recruit_chance = 0;
    int planned_forgive_roll = 0;
    int planned_mercy_roll = 0;
    int planned_recruit_roll = 0;
    int social_skill = 0;
    bool is_enemy = false;
    bool is_following = false;

    if( lua_istable( L, request_index ) ) {
        lua_getfield( L, request_index, "extra_body" );
        if( lua_istable( L, -1 ) ) {
            lua_getfield( L, -1, "context" );
            if( lua_istable( L, -1 ) ) {
                table_get_string_field( L, -1, "phase", phase );
                table_get_string_field( L, -1, "npc_name", npc_name );
                table_get_string_field( L, -1, "player_name", player_name );
                table_get_string_field( L, -1, "last_topic", last_topic );
                table_get_string_field( L, -1, "last_event", last_event );
                table_get_string_field( L, -1, "last_seen_turn", last_seen_turn );
                table_get_bool_field( L, -1, "following", following );
                table_get_bool_field( L, -1, "friend", friend_flag );
                table_get_bool_field( L, -1, "enemy", enemy );
                table_get_int_field( L, -1, "affinity", affinity );
                table_get_int_field( L, -1, "times_spoken", times_spoken );
                table_get_string_field( L, -1, "planned_intent", planned_intent );
                table_get_string_field( L, -1, "planned_outcome", planned_outcome );
                table_get_string_field( L, -1, "planned_summary", planned_summary );
                table_get_string_field( L, -1, "planned_debug", planned_debug );
                table_get_string_field( L, -1, "planned_tone", planned_tone );
                table_get_int_field( L, -1, "planned_forgive_chance", planned_forgive_chance );
                table_get_int_field( L, -1, "planned_mercy_chance", planned_mercy_chance );
                table_get_int_field( L, -1, "planned_recruit_chance", planned_recruit_chance );
                table_get_int_field( L, -1, "planned_forgive_roll", planned_forgive_roll );
                table_get_int_field( L, -1, "planned_mercy_roll", planned_mercy_roll );
                table_get_int_field( L, -1, "planned_recruit_roll", planned_recruit_roll );
            }
            lua_pop( L, 1 );

            lua_getfield( L, -1, "bridge" );
            if( lua_istable( L, -1 ) ) {
                table_get_bool_field( L, -1, "allow_dynamic_text", allow_dynamic_text );
                table_get_string_field( L, -1, "preferred_dynamic_topic", preferred_dynamic_topic );
                table_get_int_field( L, -1, "protocol", protocol );
                table_get_bool_field( L, -1, "sidecar_enabled", sidecar_enabled );
                table_get_string_field( L, -1, "sidecar_url", sidecar_url );
                table_get_int_field( L, -1, "sidecar_timeout_ms", sidecar_timeout_ms );
                table_get_string_field( L, -1, "request_kind", request_kind );
                table_get_string_field( L, -1, "utterance", utterance );
                table_get_string_field( L, -1, "request_id", request_id );
                table_get_string_field( L, -1, "interaction_intent", interaction_intent );
                table_get_int_field( L, -1, "social_skill", social_skill );
                table_get_bool_field( L, -1, "is_enemy", is_enemy );
                table_get_bool_field( L, -1, "is_following", is_following );
            }
            lua_pop( L, 1 );
        }
        lua_pop( L, 1 );
    }

    if( preferred_dynamic_topic.empty() ) {
        preferred_dynamic_topic = "TALK_BNTALK_DYNAMIC";
    }
    if( sidecar_url.empty() ) {
        sidecar_url = "http://127.0.0.1:45123/route";
    }
    if( interaction_intent.empty() ) {
        interaction_intent = "talk";
    }

    std::string request_json = "{";
    bool first = true;
    append_json_string_field( request_json, first, "phase", phase );
    append_json_string_field( request_json, first, "npc_name", npc_name );
    append_json_string_field( request_json, first, "player_name", player_name );
    append_json_string_field( request_json, first, "last_topic", last_topic );
    append_json_string_field( request_json, first, "last_event", last_event );
    append_json_string_field( request_json, first, "last_seen_turn", last_seen_turn );
    append_json_bool_field( request_json, first, "following", following );
    append_json_bool_field( request_json, first, "friend", friend_flag );
    append_json_bool_field( request_json, first, "enemy", enemy );
    append_json_int_field( request_json, first, "affinity", affinity );
    append_json_int_field( request_json, first, "times_spoken", times_spoken );
    append_json_string_field( request_json, first, "planned_intent", planned_intent );
    append_json_string_field( request_json, first, "planned_outcome", planned_outcome );
    append_json_string_field( request_json, first, "planned_summary", planned_summary );
    append_json_string_field( request_json, first, "planned_debug", planned_debug );
    append_json_string_field( request_json, first, "planned_tone", planned_tone );
    append_json_int_field( request_json, first, "planned_forgive_chance", planned_forgive_chance );
    append_json_int_field( request_json, first, "planned_mercy_chance", planned_mercy_chance );
    append_json_int_field( request_json, first, "planned_recruit_chance", planned_recruit_chance );
    append_json_int_field( request_json, first, "planned_forgive_roll", planned_forgive_roll );
    append_json_int_field( request_json, first, "planned_mercy_roll", planned_mercy_roll );
    append_json_int_field( request_json, first, "planned_recruit_roll", planned_recruit_roll );
    append_json_int_field( request_json, first, "protocol", protocol );
    append_json_bool_field( request_json, first, "allow_dynamic_text", allow_dynamic_text );
    append_json_string_field( request_json, first, "preferred_dynamic_topic", preferred_dynamic_topic );
    append_json_bool_field( request_json, first, "sidecar_enabled", sidecar_enabled );
    append_json_string_field( request_json, first, "sidecar_url", sidecar_url );
    append_json_int_field( request_json, first, "sidecar_timeout_ms", sidecar_timeout_ms );
    append_json_string_field( request_json, first, "request_kind", request_kind );
    append_json_string_field( request_json, first, "utterance", utterance );
    append_json_string_field( request_json, first, "request_id", request_id );
    append_json_string_field( request_json, first, "interaction_intent", interaction_intent );
    append_json_int_field( request_json, first, "social_skill", social_skill );
    append_json_bool_field( request_json, first, "is_enemy", is_enemy );
    append_json_bool_field( request_json, first, "is_following", is_following );
    request_json += '}';

    std::string transport_error;
    const std::string route_json = bntalk::bridge::route_json_or_fallback( request_json, transport_error );
    lua_pushstring( L, route_json.c_str() );
    return 1;
}

bool try_validate_state( lua_State *L, std::string &details ) {
    if( L == nullptr ) {
        details = "lua_state=null";
        return false;
    }

    const int top_before = lua_gettop( L );

    lua_getglobal( L, "game" );
    const int game_type = lua_type( L, -1 );
    const char *game_type_name = lua_typename( L, game_type );
    lua_pop( L, 1 );

    lua_getglobal( L, "package" );
    const int package_type = lua_type( L, -1 );
    const char *package_type_name = lua_typename( L, package_type );
    lua_pop( L, 1 );

    lua_getglobal( L, "gdebug" );
    const int gdebug_type = lua_type( L, -1 );
    const char *gdebug_type_name = lua_typename( L, gdebug_type );
    lua_pop( L, 1 );

    const int top_after = lua_gettop( L );
    lua_settop( L, top_before );

    details = "top_before=" + std::to_string( top_before ) +
              ";top_after=" + std::to_string( top_after ) +
              ";game=" + std::string( game_type_name ? game_type_name : "<null>" ) +
              ";package=" + std::string( package_type_name ? package_type_name : "<null>" ) +
              ";gdebug=" + std::string( gdebug_type_name ? gdebug_type_name : "<null>" );
    return true;
}

bool try_register_bridge( lua_State *L, std::string &details ) {
    if( L == nullptr ) {
        details = "lua_state=null";
        return false;
    }

    const int top_before = lua_gettop( L );

    lua_pushcfunction( L, l_bntalk_native_status );
    lua_setglobal( L, "bntalk_native_status" );

    lua_pushcfunction( L, l_bntalk_native_route );
    lua_setglobal( L, "bntalk_native_route" );

    lua_pushcfunction( L, l_bntalk_native_prompt_text );
    lua_setglobal( L, "bntalk_native_prompt_text" );

    lua_getglobal( L, "bntalk_native_status" );
    const bool status_registered = lua_type( L, -1 ) == LUA_TFUNCTION;
    lua_pop( L, 1 );

    lua_getglobal( L, "bntalk_native_route" );
    const bool route_registered = lua_type( L, -1 ) == LUA_TFUNCTION;
    lua_pop( L, 1 );

    lua_settop( L, top_before );

    details = "status_registered=" + std::string( status_registered ? "true" : "false" ) +
              ";route_registered=" + std::string( route_registered ? "true" : "false" );
    return status_registered && route_registered;
}

LONG run_with_seh( void *raw_state, std::string &details, bool do_register ) {
    __try {
        if( do_register ) {
            return try_register_bridge( static_cast<lua_State *>( raw_state ), details ) ? 1 : 0;
        }
        return try_validate_state( static_cast<lua_State *>( raw_state ), details ) ? 1 : 0;
    } __except( EXCEPTION_EXECUTE_HANDLER ) {
        details = do_register ? "seh_exception_during_register_bridge" : "seh_exception_during_validate_state";
        return -1;
    }
}

}  // namespace

std::string validate_state_json( void *raw_state ) {
    std::string details;
    const LONG result = run_with_seh( raw_state, details, false );
    return make_result_json( result == 1, details );
}

std::string register_bridge_json( void *raw_state ) {
    std::string details;
    const LONG result = run_with_seh( raw_state, details, true );
    return make_result_json( result == 1, details );
}

}  // namespace bntalk::bridge::lua_runtime
