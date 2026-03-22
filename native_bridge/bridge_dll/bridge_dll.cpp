#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "bridge_exports.h"
#include "bridge_state.h"
#include "../common/bntalk_protocol.h"

namespace bntalk::bridge::lua_runtime {
std::string validate_state_json( void *raw_state );
std::string register_bridge_json( void *raw_state );
}

namespace bntalk::bridge {
namespace {

BridgeState g_state;
std::string g_last_status_json;
std::string g_last_route_json;
std::string g_title_status_json;
std::atomic<bool> g_pipe_running( false );
HANDLE g_pipe_thread = nullptr;
HANDLE g_attach_thread = nullptr;

constexpr const wchar_t *kPipeName = LR"(\\.\pipe\bntalk_bridge_test)";
constexpr const wchar_t *kLogPath = L"E:\\stuff\\gamere\\CBN\\bn\\userdata\\mods\\ab\\bntalk\\native_bridge\\build\\bridge_runtime.log";

constexpr uintptr_t kPreferredImageBase = 0x140000000ull;
constexpr uintptr_t kVaStringCataluaCpp = 0x1431CDE00ull;
constexpr uintptr_t kVaStringRegLua = 0x1431CE300ull;
constexpr uintptr_t kVaStringOpenFunctionLua = 0x1437FEBF0ull;
constexpr uintptr_t kVaStringLuaReload = 0x14331AA60ull;

struct TextSectionView {
    const std::uint8_t *begin = nullptr;
    std::size_t size = 0;
};

void append_log( const std::string &line ) {
    HANDLE file = CreateFileW( kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
    if( file == INVALID_HANDLE_VALUE ) {
        return;
    }

    const std::string text = line + "\r\n";
    DWORD written = 0;
    WriteFile( file, text.c_str(), static_cast<DWORD>( text.size() ), &written, nullptr );
    CloseHandle( file );
}

void log_last_error( const std::string &prefix ) {
    append_log( prefix + " GetLastError=" + std::to_string( static_cast<unsigned long>( GetLastError() ) ) );
}

std::string hex_addr( uintptr_t value ) {
    char buffer[32] = {};
    wsprintfA( buffer, "0x%p", reinterpret_cast<void *>( value ) );
    return std::string( buffer );
}

bool query_named_section( const char *name, TextSectionView &out ) {
    auto *base = reinterpret_cast<std::uint8_t *>( GetModuleHandleW( nullptr ) );
    if( base == nullptr ) {
        return false;
    }

    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>( base );
    if( dos->e_magic != IMAGE_DOS_SIGNATURE ) {
        return false;
    }

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>( base + dos->e_lfanew );
    if( nt->Signature != IMAGE_NT_SIGNATURE ) {
        return false;
    }

    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION( nt );
    for( unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i ) {
        if( std::memcmp( section[i].Name, name, std::strlen( name ) ) == 0 ) {
            out.begin = base + section[i].VirtualAddress;
            out.size = section[i].Misc.VirtualSize;
            return true;
        }
    }

    return false;
}

bool query_text_section( TextSectionView &out ) {
    return query_named_section( ".text", out );
}

bool is_readable_pointer_range( uintptr_t address, std::size_t min_size ) {
    if( address < 0x10000ull || address > 0x00007FFFFFFFFFFFull ) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if( VirtualQuery( reinterpret_cast<void *>( address ), &mbi, sizeof( mbi ) ) == 0 ) {
        return false;
    }
    if( mbi.State != MEM_COMMIT ) {
        return false;
    }
    if( ( mbi.Protect & PAGE_NOACCESS ) != 0 || ( mbi.Protect & PAGE_GUARD ) != 0 ) {
        return false;
    }

    const uintptr_t region_base = reinterpret_cast<uintptr_t>( mbi.BaseAddress );
    const uintptr_t region_end = region_base + mbi.RegionSize;
    return address + min_size <= region_end;
}

bool looks_like_lua_state( uintptr_t address ) {
    if( !is_readable_pointer_range( address, 0x20 ) ) {
        return false;
    }

    const auto *ptr = reinterpret_cast<const std::uint8_t *>( address );
    const std::uint8_t tt = ptr[8];
    const std::uint8_t status = ptr[10];
    const std::uint8_t ncalls = ptr[11];

    if( tt != 8 ) {
        return false;
    }
    if( status > 8 ) {
        return false;
    }
    if( ncalls > 96 ) {
        return false;
    }

    return true;
}

bool scan_for_loader_lua_state() {
    auto &current = state();
    current.lua_state = nullptr;

    const char *section_names[] = { ".data", ".rdata" };
    int hit_count = 0;

    for( const char *section_name : section_names ) {
        TextSectionView section;
        if( !query_named_section( section_name, section ) ) {
            continue;
        }

        for( std::size_t i = 0; i + sizeof( uintptr_t ) <= section.size; i += sizeof( uintptr_t ) ) {
            uintptr_t object_ptr = 0;
            std::memcpy( &object_ptr, section.begin + i, sizeof( object_ptr ) );
            if( !is_readable_pointer_range( object_ptr, sizeof( uintptr_t ) * 2 ) ) {
                continue;
            }

            uintptr_t lua_a = 0;
            uintptr_t lua_b = 0;
            std::memcpy( &lua_a, reinterpret_cast<const void *>( object_ptr ), sizeof( lua_a ) );
            std::memcpy( &lua_b, reinterpret_cast<const void *>( object_ptr + sizeof( uintptr_t ) ), sizeof( lua_b ) );
            if( lua_a == 0 || lua_a != lua_b ) {
                continue;
            }
            if( !looks_like_lua_state( lua_a ) ) {
                continue;
            }

            ++hit_count;
            append_log(
                std::string( "lua candidate section=" ) + section_name +
                " holder=" + hex_addr( reinterpret_cast<uintptr_t>( section.begin + i ) ) +
                " object=" + hex_addr( object_ptr ) +
                " lua=" + hex_addr( lua_a )
            );
            if( current.lua_state == nullptr ) {
                current.lua_state = reinterpret_cast<void *>( lua_a );
            }
            if( hit_count >= 8 ) {
                append_log( "lua candidate scan truncated at 8 hits" );
                return current.lua_state != nullptr;
            }
        }
    }

    append_log( std::string( "lua candidate count=" ) + std::to_string( hit_count ) );
    return current.lua_state != nullptr;
}

bool try_attach_lua_bridge( const char *reason ) {
    auto &current = state();
    append_log( std::string( "try_attach_lua_bridge: reason=" ) + ( reason ? reason : "<null>" ) );

    if( current.lua_state == nullptr ) {
        const bool found = scan_for_loader_lua_state();
        append_log( std::string( "try_attach_lua_bridge: scan=" ) + ( found ? "true" : "false" ) +
                    " lua_state=" + hex_addr( reinterpret_cast<uintptr_t>( current.lua_state ) ) );
    }

    if( current.lua_state == nullptr ) {
        return false;
    }
    if( current.lua_bridge_registered ) {
        return true;
    }

    const std::string register_result = bntalk::bridge::lua_runtime::register_bridge_json( current.lua_state );
    append_log( "try_attach_lua_bridge: register_result=" + register_result );
    if( register_result.find( "\"ok\":true" ) != std::string::npos ) {
        current.lua_bridge_registered = true;
    }
    return current.lua_bridge_registered;
}

DWORD WINAPI attach_monitor_thread( LPVOID ) {
    append_log( "attach_monitor_thread: starting" );
    for( int i = 0; i < 240 && !state().lua_bridge_registered; ++i ) {
        if( try_attach_lua_bridge( "attach_monitor_thread" ) ) {
            break;
        }
        Sleep( 500 );
    }
    append_log( std::string( "attach_monitor_thread: exiting registered=" ) +
                ( state().lua_bridge_registered ? "true" : "false" ) );
    return 0;
}

uintptr_t current_exe_base() {
    return reinterpret_cast<uintptr_t>( GetModuleHandleW( nullptr ) );
}

uintptr_t translate_preferred_va( uintptr_t preferred_va ) {
    return current_exe_base() + ( preferred_va - kPreferredImageBase );
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
        return std::string();
    }
    const int size = WideCharToMultiByte( CP_UTF8, 0, text.c_str(), static_cast<int>( text.size() ), nullptr, 0, nullptr, nullptr );
    if( size <= 0 ) {
        return std::string();
    }
    std::string out( size, '\0' );
    WideCharToMultiByte( CP_UTF8, 0, text.c_str(), static_cast<int>( text.size() ), &out[0], size, nullptr, nullptr );
    return out;
}


std::string collect_rip_relative_xrefs( uintptr_t target ) {
    TextSectionView text;
    if( !query_text_section( text ) ) {
        return "no_text_section";
    }

    std::string result;
    int hit_count = 0;
    for( std::size_t i = 0; i + 7 <= text.size && hit_count < 8; ++i ) {
        const std::uint8_t rex = text.begin[i];
        const std::uint8_t opcode = text.begin[i + 1];
        const std::uint8_t modrm = text.begin[i + 2];

        const bool is_rex = rex == 0x48 || rex == 0x4C;
        const bool is_rip_relative = ( modrm & 0xC7 ) == 0x05;
        const bool is_candidate_opcode = opcode == 0x8D || opcode == 0x8B;
        if( !is_rex || !is_candidate_opcode || !is_rip_relative ) {
            continue;
        }

        std::int32_t disp = 0;
        std::memcpy( &disp, text.begin + i + 3, sizeof( disp ) );
        const uintptr_t resolved = reinterpret_cast<uintptr_t>( text.begin + i + 7 ) + static_cast<std::intptr_t>( disp );
        if( resolved != target ) {
            continue;
        }

        if( !result.empty() ) {
            result += ';';
        }
        result += hex_addr( reinterpret_cast<uintptr_t>( text.begin + i ) );
        ++hit_count;
    }

    if( result.empty() ) {
        return "none";
    }
    return result;
}

void capture_runtime_anchors() {
    auto &current = state();
    current.base_runtime = current_exe_base();
    current.addr_fn_lua_console = translate_preferred_va( kVaStringCataluaCpp );
    current.addr_fn_reg_lua_iuse = translate_preferred_va( kVaStringRegLua );
    current.addr_fn_open_function_lua = translate_preferred_va( kVaStringOpenFunctionLua );
    current.addr_fn_lua_reload = translate_preferred_va( kVaStringLuaReload );

    append_log( "runtime base=" + hex_addr( current.base_runtime ) );
    append_log( "anchor string catalua.cpp=" + hex_addr( current.addr_fn_lua_console ) );
    append_log( "anchor string reg_lua_=" + hex_addr( current.addr_fn_reg_lua_iuse ) );
    append_log( "anchor string open_function_lua=" + hex_addr( current.addr_fn_open_function_lua ) );
    append_log( "anchor string lua_reload=" + hex_addr( current.addr_fn_lua_reload ) );
    append_log( "xref reg_lua_=" + collect_rip_relative_xrefs( current.addr_fn_reg_lua_iuse ) );
    append_log( "xref open_function_lua=" + collect_rip_relative_xrefs( current.addr_fn_open_function_lua ) );
    append_log( "xref lua_reload=" + collect_rip_relative_xrefs( current.addr_fn_lua_reload ) );
    const bool found_lua = scan_for_loader_lua_state();
    append_log( std::string( "scan_for_loader_lua_state=" ) + ( found_lua ? "true" : "false" ) +
                " current.lua_state=" + hex_addr( reinterpret_cast<uintptr_t>( current.lua_state ) ) );
}

struct WindowSearchState {
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

BOOL CALLBACK enum_windows_proc( HWND hwnd, LPARAM lparam ) {
    auto *state = reinterpret_cast<WindowSearchState *>( lparam );
    DWORD window_pid = 0;
    GetWindowThreadProcessId( hwnd, &window_pid );
    if( window_pid != state->pid ) {
        return TRUE;
    }
    if( GetWindow( hwnd, GW_OWNER ) != nullptr ) {
        return TRUE;
    }
    if( !IsWindowVisible( hwnd ) ) {
        return TRUE;
    }
    state->hwnd = hwnd;
    return FALSE;
}

HWND find_bn_window() {
    WindowSearchState state;
    state.pid = GetCurrentProcessId();
    EnumWindows( enum_windows_proc, reinterpret_cast<LPARAM>( &state ) );
    return state.hwnd;
}

std::string get_window_title() {
    HWND hwnd = find_bn_window();
    if( hwnd == nullptr ) {
        return std::string();
    }
    wchar_t buffer[512] = {};
    const int len = GetWindowTextW( hwnd, buffer, 511 );
    if( len <= 0 ) {
        return std::string();
    }
    return wide_to_utf8( std::wstring( buffer, buffer + len ) );
}

bool set_window_title_text( const std::string &title ) {
    HWND hwnd = find_bn_window();
    if( hwnd == nullptr ) {
        append_log( "set_window_title_text: no BN window found" );
        return false;
    }
    const std::wstring wide_title = utf8_to_wide( title );
    if( wide_title.empty() && !title.empty() ) {
        append_log( "set_window_title_text: utf8_to_wide failed" );
        return false;
    }
    const bool ok = SetWindowTextW( hwnd, wide_title.c_str() ) != FALSE;
    if( !ok ) {
        log_last_error( "SetWindowTextW failed" );
    }
    return ok;
}

std::string status_json_text() {
    try_attach_lua_bridge( "status_json_text" );
    auto &current = state();
    const bool has_lua_state = current.lua_state != nullptr;
    const bool available = current.lua_bridge_registered;

    std::string bridge_mode = "detached";
    std::string error;
    if( current.bootstrap_complete ) {
        bridge_mode = available ? "lua_registered" : "bootstrap_only";
    }
    if( !current.bootstrap_complete ) {
        error = "bootstrap_not_complete";
    } else if( !has_lua_state ) {
        error = "lua_state_not_found";
    } else if( !current.lua_bridge_registered ) {
        error = "lua_bridge_not_registered";
    }

    g_last_status_json = "{";
    g_last_status_json += "\"available\":" + std::string( available ? "true" : "false" ) + ",";
    g_last_status_json += "\"provider\":\"native_runtime_bridge\",";
    g_last_status_json += "\"version\":\"0.3.0\",";
    g_last_status_json += "\"bridge_mode\":\"" + json_escape( bridge_mode ) + "\",";
    g_last_status_json += "\"bootstrap_complete\":" + std::string( current.bootstrap_complete ? "true" : "false" ) + ",";
    g_last_status_json += "\"lua_bridge_registered\":" + std::string( current.lua_bridge_registered ? "true" : "false" ) + ",";
    g_last_status_json += "\"has_lua_state\":" + std::string( has_lua_state ? "true" : "false" ) + ",";
    g_last_status_json += "\"runtime_base\":\"" + json_escape( hex_addr( current.base_runtime ) ) + "\",";
    g_last_status_json += "\"lua_state\":\"" + json_escape( hex_addr( reinterpret_cast<uintptr_t>( current.lua_state ) ) ) + "\",";
    g_last_status_json += "\"error\":\"" + json_escape( error ) + "\"";
    g_last_status_json += "}";
    return g_last_status_json;
}

std::string route_json_text( const char *request_json ) {
    const std::string payload = request_json ? request_json : "{}";
    state().last_route_transport_error.clear();
    g_last_route_json = route_json_or_fallback( payload, state().last_route_transport_error );
    return g_last_route_json;
}

std::string title_status_json_text() {
    const std::string title = get_window_title();
    g_title_status_json = "{";
    g_title_status_json += "\"ok\":";
    g_title_status_json += title.empty() ? "false" : "true";
    g_title_status_json += ",\"window_title\":\"" + json_escape( title ) + "\"";
    g_title_status_json += "}";
    return g_title_status_json;
}

std::string lua_probe_json_text() {
    auto &current = state();
    std::string json = "{";
    json += "\"runtime_base\":\"" + json_escape( hex_addr( current.base_runtime ) ) + "\",";
    json += "\"lua_bridge_registered\":" + std::string( current.lua_bridge_registered ? "true" : "false" ) + ",";
    json += "\"lua_state\":\"" + json_escape( hex_addr( reinterpret_cast<uintptr_t>( current.lua_state ) ) ) + "\",";
    json += "\"reg_lua_anchor\":\"" + json_escape( hex_addr( current.addr_fn_reg_lua_iuse ) ) + "\",";
    json += "\"reg_lua_xrefs\":\"" + json_escape( collect_rip_relative_xrefs( current.addr_fn_reg_lua_iuse ) ) + "\",";
    json += "\"open_function_lua_anchor\":\"" + json_escape( hex_addr( current.addr_fn_open_function_lua ) ) + "\",";
    json += "\"open_function_lua_xrefs\":\"" + json_escape( collect_rip_relative_xrefs( current.addr_fn_open_function_lua ) ) + "\",";
    json += "\"lua_reload_anchor\":\"" + json_escape( hex_addr( current.addr_fn_lua_reload ) ) + "\",";
    json += "\"lua_reload_xrefs\":\"" + json_escape( collect_rip_relative_xrefs( current.addr_fn_lua_reload ) ) + "\"";
    json += "}";
    return json;
}

std::string handle_command( const std::string &command ) {
    append_log( "handle_command: " + command );
    if( command == "PING" ) {
        return "PONG";
    }
    if( command == "STATUS" ) {
        try_attach_lua_bridge( "command_STATUS" );
        return status_json_text();
    }
    if( command == "LUA_PROBE" ) {
        return lua_probe_json_text();
    }
    if( command == "LUA_VALIDATE" ) {
        try_attach_lua_bridge( "command_LUA_VALIDATE" );
        return bntalk::bridge::lua_runtime::validate_state_json( state().lua_state );
    }
    if( command == "LUA_REGISTER" ) {
        try_attach_lua_bridge( "command_LUA_REGISTER_pre" );
        const std::string result = bntalk::bridge::lua_runtime::register_bridge_json( state().lua_state );
        if( result.find( "\"ok\":true" ) != std::string::npos ) {
            state().lua_bridge_registered = true;
        }
        return result;
    }
    if( command.rfind( "ROUTE ", 0 ) == 0 ) {
        return route_json_text( command.c_str() + 6 );
    }
    if( command == "TITLE" ) {
        return title_status_json_text();
    }
    if( command.rfind( "TITLE ", 0 ) == 0 ) {
        const bool ok = set_window_title_text( command.substr( 6 ) );
        return ok ? std::string( "TITLE_SET" ) : std::string( "TITLE_SET_FAILED" );
    }
    if( command.rfind( "TITLE_ROUTE ", 0 ) == 0 ) {
        const std::string route_json = route_json_text( command.c_str() + 12 );
        const bool ok = set_window_title_text( std::string( "BNTalk Route: " ) + g_last_route_json );
        return ok ? route_json : std::string( "TITLE_ROUTE_FAILED " ) + route_json;
    }
    return std::string( "UNKNOWN_COMMAND" );
}

DWORD WINAPI pipe_server_thread( LPVOID ) {
    append_log( "pipe_server_thread: starting" );
    g_pipe_running = true;
    while( g_pipe_running ) {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr
        );
        if( pipe == INVALID_HANDLE_VALUE ) {
            log_last_error( "CreateNamedPipeW failed" );
            break;
        }

        append_log( "pipe_server_thread: pipe created, waiting for client" );
        const BOOL connected = ConnectNamedPipe( pipe, nullptr ) ? TRUE : ( GetLastError() == ERROR_PIPE_CONNECTED );
        if( connected ) {
            append_log( "pipe_server_thread: client connected" );
            char buffer[4096] = {};
            DWORD read = 0;
            if( ReadFile( pipe, buffer, sizeof( buffer ) - 1, &read, nullptr ) ) {
                const std::string command( buffer, buffer + read );
                const std::string response = handle_command( command );
                DWORD written = 0;
                WriteFile( pipe, response.c_str(), static_cast<DWORD>( response.size() ), &written, nullptr );
            } else {
                log_last_error( "ReadFile(pipe) failed" );
            }
        } else {
            log_last_error( "ConnectNamedPipe failed" );
        }

        FlushFileBuffers( pipe );
        DisconnectNamedPipe( pipe );
        CloseHandle( pipe );
    }
    append_log( "pipe_server_thread: exiting" );
    return 0;
}

}  // namespace

BridgeState &state() {
    return g_state;
}

bool bootstrap() {
    auto &current = state();
    current.bootstrap_complete = true;
    current.lua_bridge_registered = false;
    capture_runtime_anchors();
    if( !try_attach_lua_bridge( "bootstrap" ) ) {
        append_log( "bootstrap: lua bridge not ready yet; attach monitor will keep retrying" );
    }
    append_log( "bootstrap: process_id=" + std::to_string( static_cast<unsigned long>( GetCurrentProcessId() ) ) );
    if( g_pipe_thread == nullptr ) {
        g_pipe_thread = CreateThread( nullptr, 0, pipe_server_thread, nullptr, 0, nullptr );
        if( g_pipe_thread == nullptr ) {
            log_last_error( "CreateThread(pipe_server_thread) failed" );
        } else {
            append_log( "bootstrap: pipe thread created" );
        }
    } else {
        append_log( "bootstrap: pipe thread already exists" );
    }
    if( g_attach_thread == nullptr && !current.lua_bridge_registered ) {
        g_attach_thread = CreateThread( nullptr, 0, attach_monitor_thread, nullptr, 0, nullptr );
        if( g_attach_thread == nullptr ) {
            log_last_error( "CreateThread(attach_monitor_thread) failed" );
        } else {
            append_log( "bootstrap: attach monitor thread created" );
        }
    }
    return true;
}

const char *status_json() {
    g_last_status_json = status_json_text();
    return g_last_status_json.c_str();
}

const char *route_json( const char *request_json ) {
    g_last_route_json = route_json_text( request_json );
    return g_last_route_json.c_str();
}

}  // namespace bntalk::bridge

extern "C" __declspec(dllexport) DWORD WINAPI bntalk_bridge_bootstrap( LPVOID ) {
    bntalk::bridge::append_log( "bntalk_bridge_bootstrap: enter" );
    bntalk::bridge::bootstrap();
    bntalk::bridge::append_log( "bntalk_bridge_bootstrap: leave" );
    return 0;
}

extern "C" __declspec(dllexport) const char *bntalk_bridge_status_json() {
    return bntalk::bridge::status_json();
}

extern "C" __declspec(dllexport) const char *bntalk_bridge_route_json( const char *request_json ) {
    return bntalk::bridge::route_json( request_json );
}

extern "C" __declspec(dllexport) const char *bntalk_native_status() {
    return bntalk::bridge::status_json();
}

extern "C" __declspec(dllexport) const char *bntalk_native_route( const char *request_json ) {
    return bntalk::bridge::route_json( request_json );
}

BOOL APIENTRY DllMain( HMODULE module, DWORD reason, LPVOID ) {
    if( reason == DLL_PROCESS_ATTACH ) {
        DisableThreadLibraryCalls( module );
        bntalk::bridge::state().self = module;
    } else if( reason == DLL_PROCESS_DETACH ) {
        bntalk::bridge::g_pipe_running = false;
    }
    return TRUE;
}
