---@diagnostic disable: undefined-global

local config = _G.BNTALK_PROVIDER_CONFIG or require( "./lua/provider_config" )

local M = {}

local function resolve_global( name )
	if not name or name == "" then
		return nil
	end

	local value = rawget( _G, name )
	if type( value ) == "function" then
		return value
	end

	return nil
end

local function call_named_function( name, ... )
	local fn = resolve_global( name )
	if not fn then
		return nil, "missing_global:" .. tostring( name )
	end

	local ok, result = pcall( fn, ... )
	if not ok then
		return nil, tostring( result )
	end

	return result, nil
end

local function decode_flat_json_object( raw )
	if type( raw ) ~= "string" then
		return raw
	end

	local text = raw
	if not text:match( "^%s*%{" ) then
		return raw
	end

	local out = {}
	for key, quoted in text:gmatch( '"([%w_]+)"%s*:%s*"(.-)"' ) do
		local value = quoted:gsub( '\\n', '\n' ):gsub( '\\r', '\r' ):gsub( '\\t', '\t' ):gsub( '\\"', '"' ):gsub( '\\\\', '\\' )
		out[key] = value
	end
	for key, boolean_text in text:gmatch( '"([%w_]+)"%s*:%s*(true|false)' ) do
		out[key] = boolean_text == "true"
	end
	for key, number_text in text:gmatch( '"([%w_]+)"%s*:%s*(-?%d+)' ) do
		out[key] = tonumber( number_text )
	end

	if next( out ) == nil then
		return raw
	end
	return out
end

function M.status()
	local bridge = config.bridge or {}
	local status_fn = bridge.status_function or "bntalk_native_status"
	local result, err = call_named_function( status_fn )
	result = decode_flat_json_object( result )
	if not result then
		return {
			available = false,
			provider = "native_bridge",
			error = err or "unavailable",
			version = "",
			bridge_mode = "detached",
		}
	end

	if type( result ) ~= "table" then
		return {
			available = false,
			provider = "native_bridge",
			error = "invalid_status_result",
			version = "",
			bridge_mode = "detached",
		}
	end

	result.available = result.available == true
	result.provider = result.provider or "native_bridge"
	result.version = tostring( result.version or "" )
	result.bridge_mode = tostring( result.bridge_mode or "attached" )
	result.error = result.error and tostring( result.error ) or nil
	return result
end

function M.is_available()
	local status = M.status()
	return status.available == true, status
end

function M.build_request( ctx, opts )
	local bridge = config.bridge or {}
	opts = opts or {}
	return {
		model = bridge.model or "bntalk-native-bridge",
		messages = {
			{
				role = "system",
				content = "You are the BN Talk native bridge. Return a structured talk routing decision for Cataclysm BN.",
			},
			{
				role = "user",
				content = "Use the supplied context and return topic_id plus optional generated_text.",
			},
		},
		temperature = 0,
		max_tokens = bridge.max_tokens or 64,
		response_format = {
			type = "json_object",
		},
		extra_body = {
			context = ctx,
			bridge = {
				protocol = bridge.protocol_version or 1,
				allow_dynamic_text = bridge.allow_dynamic_text == true,
				preferred_dynamic_topic = bridge.dynamic_topic or "TALK_BNTALK_DYNAMIC",
				sidecar_enabled = bridge.sidecar_enabled ~= false,
				sidecar_url = bridge.sidecar_url or "http://127.0.0.1:45123/route",
				sidecar_timeout_ms = bridge.sidecar_timeout_ms or 3000,
				request_kind = tostring( opts.request_kind or "route" ),
				utterance = tostring( opts.utterance or "" ),
				request_id = tostring( opts.request_id or "" ),
				interaction_intent = tostring( opts.interaction_intent or "talk" ),
				social_skill = tonumber( opts.social_skill ) or 0,
				is_enemy = opts.is_enemy == true,
				is_following = opts.is_following == true,
			},
		},
	}
end

function M.complete( request )
	local bridge = config.bridge or {}
	local available, status = M.is_available()
	if not available then
		return nil, status and status.error or "native bridge unavailable"
	end

	local route_fn = bridge.route_function or "bntalk_native_route"
	local result, err = call_named_function( route_fn, request )
	result = decode_flat_json_object( result )
	if not result then
		return nil, err or "native route failed"
	end
	if type( result ) ~= "table" then
		return nil, "native route returned non-table result"
	end

	local topic_id = tostring( result.topic_id or "" )
	if topic_id == "" then
		topic_id = "TALK_BNTALK_NEUTRAL"
	end

	local generated_text = result.generated_text and tostring( result.generated_text ) or ""
	local provider_name = result.provider and tostring( result.provider ) or "native_bridge"
	local emotion_delta = tonumber( result.emotion_delta ) or 0
	local request_id = result.request_id and tostring( result.request_id ) or ""
	local ready = result.ready ~= false

	return {
		id = "bntalk-native-0001",
		object = "chat.completion",
		created = 0,
		model = request.model or bridge.model or "bntalk-native-bridge",
		choices = {
			{
				index = 0,
				message = {
					role = "assistant",
					content = topic_id,
				},
				finish_reason = "stop",
			},
		},
		usage = {
			prompt_tokens = #( request.messages or {} ),
			completion_tokens = 1,
			total_tokens = #( request.messages or {} ) + 1,
		},
		bntalk = {
			provider = provider_name,
			topic_id = topic_id,
			reason = result.reason and tostring( result.reason ) or "native_bridge",
			generated_text = generated_text,
			emotion_delta = emotion_delta,
			request_id = request_id,
			ready = ready,
			bridge_status = status,
			debug = result.debug and tostring( result.debug ) or "",
			mode = result.mode and tostring( result.mode ) or "topic_route",
			native = result,
		},
	}, nil
end

_G.BNTALK_PROVIDER_BRIDGE = M
return M
