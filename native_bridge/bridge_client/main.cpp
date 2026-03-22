#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <iostream>
#include <string>

namespace {

constexpr const wchar_t *kPipeName = LR"(\\.\pipe\bntalk_bridge_test)";

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

bool send_command( const std::string &command, std::string &response ) {
    const HANDLE pipe = CreateFileW(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    if( pipe == INVALID_HANDLE_VALUE ) {
        std::cerr << "CreateFileW failed, GetLastError=" << GetLastError() << "\n";
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState( pipe, &mode, nullptr, nullptr );

    DWORD written = 0;
    if( !WriteFile( pipe, command.c_str(), static_cast<DWORD>( command.size() ), &written, nullptr ) ) {
        std::cerr << "WriteFile failed, GetLastError=" << GetLastError() << "\n";
        CloseHandle( pipe );
        return false;
    }

    char buffer[4096] = {};
    DWORD read = 0;
    if( !ReadFile( pipe, buffer, sizeof( buffer ) - 1, &read, nullptr ) ) {
        std::cerr << "ReadFile failed, GetLastError=" << GetLastError() << "\n";
        CloseHandle( pipe );
        return false;
    }

    response.assign( buffer, buffer + read );
    CloseHandle( pipe );
    return true;
}

std::string build_command( int argc, wchar_t **argv ) {
    if( argc <= 1 ) {
        return "PING";
    }

    std::wstring cmd = argv[1];
    if( cmd == L"ping" ) {
        return "PING";
    }
    if( cmd == L"status" ) {
        return "STATUS";
    }
    if( cmd == L"lua_probe" ) {
        return "LUA_PROBE";
    }
    if( cmd == L"lua_validate" ) {
        return "LUA_VALIDATE";
    }
    if( cmd == L"lua_register" ) {
        return "LUA_REGISTER";
    }
    if( cmd == L"title" ) {
        if( argc >= 3 ) {
            std::wstring wide_text = argv[2];
            return "TITLE " + wide_to_utf8( wide_text );
        }
        return "TITLE BN Talk Bridge Test";
    }
    if( cmd == L"route" ) {
        if( argc >= 3 ) {
            std::wstring payload = argv[2];
            return "ROUTE " + wide_to_utf8( payload );
        }
        return "ROUTE {\"affinity\":4,\"following\":false,\"enemy\":false}";
    }
    if( cmd == L"title_route" ) {
        if( argc >= 3 ) {
            std::wstring payload = argv[2];
            return "TITLE_ROUTE " + wide_to_utf8( payload );
        }
        return "TITLE_ROUTE {\"affinity\":4,\"following\":false,\"enemy\":false}";
    }

    std::string passthrough;
    for( int i = 1; i < argc; ++i ) {
        if( i > 1 ) {
            passthrough += ' ';
        }
        std::wstring piece = argv[i];
        passthrough += wide_to_utf8( piece );
    }
    return passthrough;
}

}  // namespace

int wmain( int argc, wchar_t **argv ) {
    std::string response;
    const std::string command = build_command( argc, argv );
    if( !send_command( command, response ) ) {
        std::cerr << "Failed to reach injected BN Talk bridge pipe." << "\n";
        return 1;
    }

    std::cout << response << "\n";
    return 0;
}
