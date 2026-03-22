# BN Talk native-bridge next-stage findings

## Confirmed facts

- Runtime bridge injection works through [`bntalk_injector.exe`](./build/bntalk_injector.exe).
- In-process named-pipe communication works through [`bntalk_bridge_boot.dll`](./build/bntalk_bridge_boot.dll) and [`bntalk_bridge_client.exe`](./build/bntalk_bridge_client.exe).
- The current runtime probe discovered a stable Lua-state candidate:
  - `lua_state = 0x00000216C4B05888`
  - candidate holder in `.data`
  - candidate object pointer chain also discovered in the log.

## Source-backed architecture facts

From the BN source tree under [`../../../../../../project/code/trae/Cataclysm-BN-main/src`](../../../../../../project/code/trae/Cataclysm-BN-main/src):

- [`DynamicDataLoader`](../../../../../../project/code/trae/Cataclysm-BN-main/src/init.h:56) owns [`lua`](../../../../../../project/code/trae/Cataclysm-BN-main/src/init.h:70).
- [`DynamicDataLoader::get_instance()`](../../../../../../project/code/trae/Cataclysm-BN-main/src/init.cpp:128) returns a static singleton.
- [`loader.lua = cata::make_wrapped_state()`](../../../../../../project/code/trae/Cataclysm-BN-main/src/init.cpp:862) creates the wrapped runtime state.
- [`cata::lua_state`](../../../../../../project/code/trae/Cataclysm-BN-main/src/catalua_impl.h:18) contains [`sol::state lua`](../../../../../../project/code/trae/Cataclysm-BN-main/src/catalua_impl.h:19).
- [`make_lua_state()`](../../../../../../project/code/trae/Cataclysm-BN-main/src/catalua_impl.cpp:16) calls [`register_searcher( lua.lua_state() )`](../../../../../../project/code/trae/Cataclysm-BN-main/src/catalua_impl.cpp:29) and [`reg_all_bindings( lua )`](../../../../../../project/code/trae/Cataclysm-BN-main/src/catalua_impl.cpp:31).
- BN itself accesses the runtime Lua through [`DynamicDataLoader::get_instance().lua->lua`](../../../../../../project/code/trae/Cataclysm-BN-main/src/catalua_mapgen.cpp:13).

## What the current prototype already does

The injected DLL now performs:

- executable base discovery
- string anchor translation for `catalua.cpp`, `reg_lua_`, `open_function_lua`, and `lua_reload`
- RIP-relative xref search on `.text`
- object-like scan across `.data` and `.rdata`
- heuristic detection of a likely `lua_State *`

Relevant implementation is in [`bridge_dll.cpp`](./bridge_dll/bridge_dll.cpp).

## Current blocker

We can now locate a plausible Lua-state pointer, but we have **not yet validated** that it is safe to call Lua C API directly from the injected DLL.

The next concrete engineering step is:

1. add a **read-only Lua validation probe** against the discovered `lua_State *`
2. test minimal operations such as:
   - `lua_gettop`
   - `lua_getglobal( L, "game" )`
   - `lua_getglobal( L, "package" )`
3. only after safe validation, add:
   - `lua_pushcfunction`
   - `lua_setglobal( L, "bntalk_native_status" )`
   - `lua_setglobal( L, "bntalk_native_route" )`

## Why no final registration yet

The project is past the feasibility phase, but still in the **runtime validation** phase.

Direct registration before validating stack/thread safety would risk:

- corrupting the game Lua stack
- crashing BN during dialogue hooks
- producing false positives about bridge availability

## Current user-facing status

- Pure Lua fallback path in [`../main.lua`](../main.lua) is stable.
- Native pipe bridge is stable.
- Native-to-Lua registration is not finished yet.

## Planned next implementation step

The next code change should add a `LUA_VALIDATE` command to the pipe bridge client / DLL pair that attempts read-only interaction with the discovered `lua_State *` and returns a JSON result.
