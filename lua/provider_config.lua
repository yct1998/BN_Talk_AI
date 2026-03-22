---@diagnostic disable: undefined-global

local M = {
	bridge = {
		prefer_native_bridge = true,
		protocol_version = 1,
		status_function = "bntalk_native_status",
		route_function = "bntalk_native_route",
		dynamic_topic = "TALK_BNTALK_DYNAMIC",
		allow_dynamic_text = true,
		display_generated_text_in_log = true,
		max_tokens = 96,
		sidecar_enabled = true,
		sidecar_url = "http://127.0.0.1:45123/route",
		sidecar_timeout_ms = 30000,
	},
	api = {
		type = "openai_chat_completion",
		base_url = "http://127.0.0.1:11434/v1",
		api_key = "",
		model = "bntalk-placeholder",
		request = {
			model = "bntalk-placeholder",
			temperature = 0.2,
			max_tokens = 64,
			response_format = {
				type = "json_object",
			},
		},
	},
}

_G.BNTALK_PROVIDER_CONFIG = M
return M