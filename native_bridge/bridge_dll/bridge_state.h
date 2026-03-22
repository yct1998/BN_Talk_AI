#pragma once

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bntalk::bridge {

struct BridgeState {
    HMODULE self = nullptr;
    bool bootstrap_complete = false;
    bool lua_bridge_registered = false;
    void *lua_state = nullptr;
    uintptr_t base_runtime = 0;
    uintptr_t addr_fn_reg_lua_iuse = 0;
    uintptr_t addr_fn_open_function_lua = 0;
    uintptr_t addr_fn_lua_console = 0;
    uintptr_t addr_fn_lua_reload = 0;
    std::string last_route_transport_error;
};

BridgeState &state();
bool bootstrap();

}  // namespace bntalk::bridge
