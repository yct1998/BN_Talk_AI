#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

DWORD find_process_id( const wchar_t *process_name ) {
    const HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
    if( snapshot == INVALID_HANDLE_VALUE ) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof( entry );
    if( !Process32FirstW( snapshot, &entry ) ) {
        CloseHandle( snapshot );
        return 0;
    }

    do {
        if( _wcsicmp( entry.szExeFile, process_name ) == 0 ) {
            CloseHandle( snapshot );
            return entry.th32ProcessID;
        }
    } while( Process32NextW( snapshot, &entry ) );

    CloseHandle( snapshot );
    return 0;
}

std::wstring basename_from_path( const std::wstring &path ) {
    const size_t pos = path.find_last_of( L"\\/" );
    if( pos == std::wstring::npos ) {
        return path;
    }
    return path.substr( pos + 1 );
}

std::wstring directory_from_path( const std::wstring &path ) {
    const size_t pos = path.find_last_of( L"\\/" );
    if( pos == std::wstring::npos ) {
        return L".";
    }
    return path.substr( 0, pos );
}

std::wstring quote_arg( const std::wstring &arg ) {
    if( arg.find_first_of( L" \t\"" ) == std::wstring::npos ) {
        return arg;
    }
    std::wstring out = L"\"";
    for( const wchar_t ch : arg ) {
        if( ch == L'\"' ) {
            out += L"\\\"";
        } else {
            out += ch;
        }
    }
    out += L"\"";
    return out;
}

bool launch_sidecar_detached( const std::wstring &injector_dir ) {
    const std::wstring server_path = injector_dir + L"\\..\\sidecar\\server.py";
    if( GetFileAttributesW( server_path.c_str() ) == INVALID_FILE_ATTRIBUTES ) {
        return false;
    }

    const std::wstring python_exe = L"python";
    const std::wstring command =
        quote_arg( python_exe ) + L" -u " +
        quote_arg( server_path ) +
        L" --config " +
        quote_arg( injector_dir + L"\\..\\..\\config\\provider.openai.runtime" );

    std::vector<wchar_t> command_buffer( command.begin(), command.end() );
    command_buffer.push_back( L'\0' );

    STARTUPINFOW si{};
    si.cb = sizeof( si );
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        nullptr,
        command_buffer.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS,
        nullptr,
        injector_dir.c_str(),
        &si,
        &pi
    );
    if( !created ) {
        return false;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return true;
}

uintptr_t find_remote_module_base( DWORD pid, const std::wstring &dll_path ) {
    const std::wstring basename = basename_from_path( dll_path );
    for( int attempt = 0; attempt < 100; ++attempt ) {
        HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid );
        if( snapshot != INVALID_HANDLE_VALUE ) {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof( entry );
            if( Module32FirstW( snapshot, &entry ) ) {
                do {
                    if( _wcsicmp( entry.szExePath, dll_path.c_str() ) == 0 || _wcsicmp( entry.szModule, basename.c_str() ) == 0 ) {
                        CloseHandle( snapshot );
                        return reinterpret_cast<uintptr_t>( entry.modBaseAddr );
                    }
                } while( Module32NextW( snapshot, &entry ) );
            }
            CloseHandle( snapshot );
        }
        Sleep( 50 );
    }
    return 0;
}

bool inject_library( DWORD pid, const std::wstring &dll_path ) {
    const HANDLE process = OpenProcess( PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid );
    if( process == nullptr ) {
        std::wcerr << L"OpenProcess failed for pid=" << pid << L"\n";
        return false;
    }

    const SIZE_T bytes = ( dll_path.size() + 1 ) * sizeof( wchar_t );
    void *remote_memory = VirtualAllocEx( process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    if( remote_memory == nullptr ) {
        std::wcerr << L"VirtualAllocEx failed\n";
        CloseHandle( process );
        return false;
    }

    if( !WriteProcessMemory( process, remote_memory, dll_path.c_str(), bytes, nullptr ) ) {
        std::wcerr << L"WriteProcessMemory failed\n";
        VirtualFreeEx( process, remote_memory, 0, MEM_RELEASE );
        CloseHandle( process );
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW( L"kernel32.dll" );
    if( kernel32 == nullptr ) {
        std::wcerr << L"GetModuleHandleW(kernel32) failed\n";
        VirtualFreeEx( process, remote_memory, 0, MEM_RELEASE );
        CloseHandle( process );
        return false;
    }

    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>( GetProcAddress( kernel32, "LoadLibraryW" ) );
    if( load_library == nullptr ) {
        std::wcerr << L"GetProcAddress(LoadLibraryW) failed\n";
        VirtualFreeEx( process, remote_memory, 0, MEM_RELEASE );
        CloseHandle( process );
        return false;
    }

    HANDLE thread_handle = CreateRemoteThread( process, nullptr, 0, load_library, remote_memory, 0, nullptr );
    if( thread_handle == nullptr ) {
        std::wcerr << L"CreateRemoteThread failed for LoadLibraryW\n";
        VirtualFreeEx( process, remote_memory, 0, MEM_RELEASE );
        CloseHandle( process );
        return false;
    }

    WaitForSingleObject( thread_handle, 5000 );
    CloseHandle( thread_handle );
    VirtualFreeEx( process, remote_memory, 0, MEM_RELEASE );

    const uintptr_t remote_base = find_remote_module_base( pid, dll_path );
    if( remote_base == 0 ) {
        std::wcerr << L"Injected DLL not found in remote module list\n";
        CloseHandle( process );
        return false;
    }

    HMODULE local_module = LoadLibraryExW( dll_path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES );
    if( local_module == nullptr ) {
        std::wcerr << L"LoadLibraryExW failed for local export resolution\n";
        CloseHandle( process );
        return false;
    }

    FARPROC bootstrap_local = GetProcAddress( local_module, "bntalk_bridge_bootstrap" );
    if( bootstrap_local == nullptr ) {
        std::wcerr << L"GetProcAddress(bntalk_bridge_bootstrap) failed\n";
        FreeLibrary( local_module );
        CloseHandle( process );
        return false;
    }

    const uintptr_t offset = reinterpret_cast<uintptr_t>( bootstrap_local ) - reinterpret_cast<uintptr_t>( local_module );
    const auto bootstrap_remote = reinterpret_cast<LPTHREAD_START_ROUTINE>( remote_base + offset );
    HANDLE bootstrap_thread = CreateRemoteThread( process, nullptr, 0, bootstrap_remote, nullptr, 0, nullptr );
    if( bootstrap_thread == nullptr ) {
        std::wcerr << L"CreateRemoteThread failed for bntalk_bridge_bootstrap\n";
        FreeLibrary( local_module );
        CloseHandle( process );
        return false;
    }

    WaitForSingleObject( bootstrap_thread, 5000 );
    CloseHandle( bootstrap_thread );
    FreeLibrary( local_module );
    CloseHandle( process );
    return true;
}

}  // namespace

int wmain( int argc, wchar_t **argv ) {
    const wchar_t *process_name = L"cataclysm-bn-tiles.exe";

    wchar_t exe_path_buffer[MAX_PATH] = {};
    const DWORD exe_path_len = GetModuleFileNameW( nullptr, exe_path_buffer, MAX_PATH );
    if( exe_path_len == 0 || exe_path_len >= MAX_PATH ) {
        std::wcerr << L"GetModuleFileNameW failed for injector path\n";
        return 1;
    }

    const std::wstring injector_dir = directory_from_path( exe_path_buffer );
    launch_sidecar_detached( injector_dir );
    const std::wstring default_dll_path = injector_dir + L"\\bntalk_bridge_lua.dll";
    wchar_t dll_path_buffer[MAX_PATH] = {};
    wcsncpy_s( dll_path_buffer, default_dll_path.c_str(), _TRUNCATE );

    if( argc >= 2 ) {
        wcsncpy_s( dll_path_buffer, argv[1], _TRUNCATE );
    }
    if( argc >= 3 ) {
        process_name = argv[2];
    }

    wchar_t full_dll_path[MAX_PATH] = {};
    const DWORD full_len = GetFullPathNameW( dll_path_buffer, MAX_PATH, full_dll_path, nullptr );
    if( full_len == 0 || full_len >= MAX_PATH ) {
        std::wcerr << L"GetFullPathNameW failed for DLL path\n";
        return 1;
    }
    if( GetFileAttributesW( full_dll_path ) == INVALID_FILE_ATTRIBUTES ) {
        std::wcerr << L"DLL not found: " << full_dll_path << L"\n";
        return 1;
    }

    const DWORD pid = find_process_id( process_name );
    if( pid == 0 ) {
        std::wcerr << L"Process not found: " << process_name << L"\n";
        return 2;
    }

    if( !inject_library( pid, full_dll_path ) ) {
        std::wcerr << L"Injection failed\n";
        return 3;
    }

    std::wcout << L"Injected " << full_dll_path << L" into pid=" << pid << L"\n";
    return 0;
}
