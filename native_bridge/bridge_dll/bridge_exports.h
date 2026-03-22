#pragma once

#ifdef _WIN32
extern "C" __declspec(dllexport) const char *bntalk_bridge_status_json();
extern "C" __declspec(dllexport) const char *bntalk_bridge_route_json( const char *request_json );
extern "C" __declspec(dllexport) const char *bntalk_native_status();
extern "C" __declspec(dllexport) const char *bntalk_native_route( const char *request_json );
#endif
