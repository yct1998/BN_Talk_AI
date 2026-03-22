---@diagnostic disable: undefined-global

local mod = game.mod_runtime[game.current_mod]
local storage = game.mod_storage[game.current_mod]

local util = _G.BNTALK_UTIL or require( "./lua/util" )
local memory = _G.BNTALK_MEMORY or require( "./lua/memory" )
local context_builder = _G.BNTALK_CONTEXT or require( "./lua/context" )
local config = _G.BNTALK_PROVIDER_CONFIG or require( "./lua/provider_config" )
local local_provider = _G.BNTALK_PROVIDER_LOCAL or require( "./lua/provider_local" )
local bridge_provider = _G.BNTALK_PROVIDER_BRIDGE or require( "./lua/provider_bridge" )
local tuning = _G.BNTALK_TUNING or require( "./lua/tuning" )

local DEBUG_ITEM_ID = "bntalk_debug_console"
local NPC_SCAN_RANGE = 60
local MERCY_FREEZE_SECONDS = 7
local unpack_values = table.unpack or unpack
local poll_async_jobs

local function ensure_runtime_state()
	mod.pending_jobs = mod.pending_jobs or {}
	mod.mercy_jobs = mod.mercy_jobs or {}
	mod.dialogue_queue = mod.dialogue_queue or {}
end

local function ensure_player_ready()
	local player = gapi.get_avatar()
	if not player then
		return nil
	end

	util.ensure_value( player, "bntalk_enabled", "yes" )
	util.ensure_value( player, "bntalk_debug", "yes" )
	util.ensure_debug_item( player, DEBUG_ITEM_ID )

	storage.stats = storage.stats or {
		routes = 0,
		last_topic = "",
		last_provider = "local_router",
		bridge_failures = 0,
		bridge_available = false,
		last_generated_text = "",
		async_submitted = 0,
		async_completed = 0,
		async_discarded = 0,
		last_async_request_id = "",
	}
	storage.stats.routes = storage.stats.routes or 0
	storage.stats.last_topic = storage.stats.last_topic or ""
	storage.stats.last_provider = storage.stats.last_provider or "local_router"
	storage.stats.bridge_failures = storage.stats.bridge_failures or 0
	storage.stats.bridge_available = storage.stats.bridge_available == true
	storage.stats.last_generated_text = storage.stats.last_generated_text or ""
	storage.stats.async_submitted = storage.stats.async_submitted or 0
	storage.stats.async_completed = storage.stats.async_completed or 0
	storage.stats.async_discarded = storage.stats.async_discarded or 0
	storage.stats.last_async_request_id = storage.stats.last_async_request_id or ""

	storage.bridge = storage.bridge or {
		last_error = "",
		last_provider = "local_router",
		last_mode = "local_router",
		last_generated_text = "",
	}
	storage.bridge.last_error = storage.bridge.last_error or ""
	storage.bridge.last_provider = storage.bridge.last_provider or "local_router"
	storage.bridge.last_mode = storage.bridge.last_mode or "local_router"
	storage.bridge.last_generated_text = storage.bridge.last_generated_text or ""

	ensure_runtime_state()
	return player
end

local function select_provider()
	local prefer_bridge = config.bridge and config.bridge.prefer_native_bridge == true
	if prefer_bridge and bridge_provider and bridge_provider.is_available then
		local available, status = bridge_provider.is_available()
		if available then
			return bridge_provider, "native_bridge", status
		end
		return local_provider, "local_router", status
	end

	return local_provider, "local_router", {
		available = false,
		error = "bridge_disabled",
		bridge_mode = "disabled",
		provider = "local_router",
		version = "",
	}
end

local function stash_generated_text( npc, response )
	local generated_text = response and response.bntalk and response.bntalk.generated_text or ""
	storage.stats.last_generated_text = generated_text
	storage.bridge.last_generated_text = generated_text
	if npc and generated_text ~= "" then
		npc:set_value( "bntalk_last_generated_text", tostring( generated_text ) )
	end
	return generated_text
end


local function passthrough_dialogue_topic( params, fallback )
	local candidate = nil
	if params then
		candidate = params.next_topic or params.prev
	end
	if candidate == nil or candidate == "" then
		candidate = fallback or "TALK_DONE"
	end
	return tostring( candidate )
end

local function try_call_method( obj, method_name, ... )
	if not obj then
		return false, nil
	end
	local method = obj[method_name]
	if type( method ) ~= "function" then
		return false, nil
	end
	return pcall( method, obj, ... )
end

local function find_json_value_start( text, key )
	if type( text ) ~= "string" or type( key ) ~= "string" then
		return nil
	end
	local _, value_pos = text:find( '"' .. key .. '"%s*:%s*', 1 )
	if not value_pos then
		return nil
	end
	return value_pos + 1
end

local function extract_json_string_field( text, key )
	local pos = find_json_value_start( text, key )
	if not pos or text:sub( pos, pos ) ~= '"' then
		return nil
	end
	pos = pos + 1
	local out = {}
	local escaped = false
	while pos <= #text do
		local ch = text:sub( pos, pos )
		if escaped then
			if ch == "n" then
				table.insert( out, "\n" )
			elseif ch == "r" then
				table.insert( out, "\r" )
			elseif ch == "t" then
				table.insert( out, "\t" )
			elseif ch == '"' then
				table.insert( out, '"' )
			elseif ch == "\\" then
				table.insert( out, "\\" )
			else
				table.insert( out, ch )
			end
			escaped = false
		elseif ch == "\\" then
			escaped = true
		elseif ch == '"' then
			return table.concat( out )
		else
			table.insert( out, ch )
		end
		pos = pos + 1
	end
	return nil
end

local function extract_json_bool_field( text, key )
	local pos = find_json_value_start( text, key )
	if not pos then
		return nil
	end
	if text:sub( pos, pos + 3 ) == "true" then
		return true
	end
	if text:sub( pos, pos + 4 ) == "false" then
		return false
	end
	return nil
end

local function extract_json_number_field( text, key )
	local pos = find_json_value_start( text, key )
	if not pos then
		return nil
	end
	local number_text = text:match( "^-?%d+", pos )
	if not number_text then
		return nil
	end
	return tonumber( number_text )
end

local function decode_native_route_json( raw )
	if type( raw ) ~= "string" or not raw:match( "^%s*%{" ) then
		return nil
	end
	local ready = extract_json_bool_field( raw, "ready" )
	if ready == nil then
		ready = true
	end
	return {
		provider = extract_json_string_field( raw, "provider" ) or "native_bridge",
		topic_id = extract_json_string_field( raw, "topic_id" ) or "TALK_BNTALK_NEUTRAL",
		mode = extract_json_string_field( raw, "mode" ) or "topic_route",
		reason = extract_json_string_field( raw, "reason" ) or "native_bridge",
		generated_text = extract_json_string_field( raw, "generated_text" ) or "",
		debug = extract_json_string_field( raw, "debug" ) or "",
		request_id = extract_json_string_field( raw, "request_id" ) or "",
		interaction_outcome = extract_json_string_field( raw, "interaction_outcome" ) or "",
		emotion_delta = extract_json_number_field( raw, "emotion_delta" ) or 0,
		ready = ready,
	}
end

local function complete_request_via_native( request )
	local native_route = rawget( _G, "bntalk_native_route" )
	if type( native_route ) ~= "function" then
		return bridge_provider.complete( request )
	end

	local ok_raw, raw_result = pcall( native_route, request )
	if not ok_raw then
		return nil, tostring( raw_result )
	end
	if type( raw_result ) ~= "string" then
		return bridge_provider.complete( request )
	end

	local decoded = decode_native_route_json( raw_result )
	if not decoded then
		return nil, "native_raw_decode_failed"
	end

	local _, status = bridge_provider.is_available()
	return {
		bntalk = {
			provider = tostring( decoded.provider or "native_bridge" ),
			topic_id = tostring( decoded.topic_id or "TALK_BNTALK_NEUTRAL" ),
			reason = tostring( decoded.reason or "native_bridge" ),
			generated_text = tostring( decoded.generated_text or "" ),
			emotion_delta = tonumber( decoded.emotion_delta ) or 0,
			request_id = tostring( decoded.request_id or "" ),
			interaction_outcome = tostring( decoded.interaction_outcome or "" ),
			ready = decoded.ready ~= false,
			bridge_status = status,
			debug = tostring( decoded.debug or "" ),
			mode = tostring( decoded.mode or "topic_route" ),
			native = decoded,
		},
	}, nil
end

local function clamp_number( value, min_value, max_value )
	if value < min_value then
		return min_value
	elseif value > max_value then
		return max_value
	end
	return value
end

local function tuned_number( value, default )
	if value == nil then
		return default
	end
	local n = tonumber( value )
	if n == nil then
		return default
	end
	return n
end

local function escape_lua_pattern( text )
	return ( tostring( text or "" ):gsub( "([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1" ) )
end


local function get_player_skill_level( player, skill_name )
	if not player or not player.get_skill_level then
		return 0
	end
	local ok_level, level = pcall( function()
		return player:get_skill_level( SkillId.new( skill_name ) )
	end )
	if ok_level and level then
		return tonumber( level ) or 0
	end
	return 0
end

local function get_social_skill( player )
	return get_player_skill_level( player, "speech" )
end

local function current_turn_number()
	local ok, turn_obj = pcall( function()
		return game.current_turn()
	end )
	if ok and turn_obj and turn_obj.get_turn then
		local ok_turn, turn = pcall( function()
			return turn_obj:get_turn()
		end )
		if ok_turn and turn then
			return tonumber( turn ) or 0
		end
	end
	return 0
end

local function roll_percent( chance )
	chance = clamp_number( math.floor( ( chance or 0 ) + 0.5 ), 0, 100 )
	if chance <= 0 then
		return false, chance
	end
	return gapi.rng( 1, 100 ) <= chance, chance
end

local function sample_percent_roll()
	return gapi.rng( 1, 100 )
end

local function count_pending_jobs()
	ensure_runtime_state()
	local count = 0
	for _ in pairs( mod.pending_jobs ) do
		count = count + 1
	end
	return count
end


local function refresh_npc_pending_state( npc )
	ensure_runtime_state()
	if not npc then
		return ""
	end
	local pending_request_id = npc:get_value( "bntalk_pending_request_id" )
	if pending_request_id ~= "" and not mod.pending_jobs[pending_request_id] then
		npc:set_value( "bntalk_pending_request_id", "" )
		return ""
	end
	return pending_request_id
end

local function collect_visible_npcs( player, range )
	local npcs = {}
	if not player or not player.get_visible_creatures then
		return npcs
	end

	local ok, creatures = pcall( function()
		return player:get_visible_creatures( range or NPC_SCAN_RANGE )
	end )
	if not ok or not creatures then
		return npcs
	end

	for _, creature in ipairs( creatures ) do
		if creature and util.safe_bool_call( creature, "is_npc" ) then
			local ok_npc, npc = pcall( function()
				return creature:as_npc()
			end )
			if ok_npc and npc then
				table.insert( npcs, npc )
			end
		end
	end

	table.sort( npcs, function( a, b )
		return util.safe_name( a ) < util.safe_name( b )
	end )
	return npcs
end

local function choose_nearby_npc( player, title, predicate )
	local npcs = {}
	for _, npc in ipairs( collect_visible_npcs( player, NPC_SCAN_RANGE ) ) do
		if not predicate or predicate( npc ) then
			table.insert( npcs, npc )
		end
	end
	if #npcs == 0 then
		gapi.add_msg( MsgType.info, "BN Talk: no nearby visible NPC found." )
		return nil
	end

	local menu = UiList.new()
	menu:title( title or "BN Talk: choose nearby NPC" )
	for index, npc in ipairs( npcs ) do
		local affinity = util.get_number( npc, "bntalk_affinity", 0 )
		local pending_request_id = refresh_npc_pending_state( npc )
		local pending_suffix = pending_request_id ~= "" and " [busy]" or ""
		menu:add( index, string.format( "%s [aff=%d]%s", util.safe_name( npc ), affinity, pending_suffix ) )
	end
	menu:add( 0, "Cancel" )

	local choice = menu:query()
	if not choice or choice < 1 then
		return nil
	end
	return npcs[choice]
end

local function find_visible_npc_by_request( player, request_id )
	if not player or not request_id or request_id == "" then
		return nil
	end
	for _, npc in ipairs( collect_visible_npcs( player, NPC_SCAN_RANGE ) ) do
		if refresh_npc_pending_state( npc ) == request_id then
			return npc
		end
	end
	return nil
end

local function query_custom_utterance( prompt_text, title_text, default_value )
	prompt_text = tostring( prompt_text or "Input what you say to the selected NPC." )
	title_text = tostring( title_text or "BN Talk" )
	default_value = tostring( default_value or "" )

	local native_prompt = rawget( _G, "bntalk_native_prompt_text" )
	if type( native_prompt ) == "function" then
		local ok_prompt, value = pcall( native_prompt, prompt_text, title_text, default_value )
		if ok_prompt and type( value ) == "string" then
			local trimmed = value:gsub( "^%s+", "" ):gsub( "%s+$", "" )
			if trimmed ~= "" then
				return trimmed, nil
			end
			return nil, nil
		end
	end

	local popup_type = rawget( _G, "string_input_popup" )
	if not popup_type or type( popup_type.new ) ~= "function" then
		return nil, "string_input_popup_unavailable"
	end

	local ok_popup, popup = pcall( popup_type.new )
	if not ok_popup or not popup then
		return nil, "string_input_popup_new_failed"
	end

	try_call_method( popup, "title", title_text )
	try_call_method( popup, "description", prompt_text )
	try_call_method( popup, "identifier", "bntalk_free_talk" )
	try_call_method( popup, "width", 72 )
	try_call_method( popup, "max_length", 280 )

	local attempts = {
		{ default_value, false, false, false },
		{ default_value, false, false },
		{ default_value, false },
		{ default_value },
		{},
	}
	for _, args in ipairs( attempts ) do
		local ok_query, value = try_call_method( popup, "query_string", unpack_values( args ) )
		if ok_query and type( value ) == "string" then
			local trimmed = value:gsub( "^%s+", "" ):gsub( "%s+$", "" )
			if trimmed ~= "" then
				return trimmed, nil
			end
			return nil, nil
		end
	end

	return nil, "query_string_failed"
end

local function utterance_has_any( text, phrases )
	if type( text ) ~= "string" or text == "" then
		return false
	end
	for _, phrase in ipairs( phrases ) do
		if text:find( phrase, 1, true ) then
			return true
		end
	end
	return false
end

local function is_extreme_insult( utterance )
	if type( utterance ) ~= "string" then
		return false
	end
	local lowered = utterance:lower()
	return utterance_has_any( lowered, {
		"操你妈",
		"操你",
		"你妈",
		"妈的",
		"傻逼",
		"傻b",
		"煞笔",
		"弱智",
		"脑残",
		"废物",
		"杂种",
		"去死",
		"滚开",
		"滚远点",
		"杀了你",
	} )
end

local function is_threatening_utterance( utterance )
	if type( utterance ) ~= "string" then
		return false
	end
	local lowered = utterance:lower()
	return utterance_has_any( lowered, {
		"杀了你",
		"弄死你",
		"宰了你",
		"打死你",
		"你死定了",
		"别想活",
		"kill you",
		"i'll kill you",
	} )
end

local function is_pleading_utterance( utterance )
	if type( utterance ) ~= "string" then
		return false
	end
	local lowered = utterance:lower()
	return utterance_has_any( lowered, {
		"求你",
		"原谅我",
		"别杀我",
		"饶了我",
		"放过我",
		"我错了",
		"forgive me",
		"please don't kill me",
		"spare me",
	} )
end

local function analyze_utterance_style( utterance )
	local style = {
		abusive = is_extreme_insult( utterance ),
		threatening = is_threatening_utterance( utterance ),
		pleading = is_pleading_utterance( utterance ),
		label = "plain",
	}
	if style.abusive and style.threatening then
		style.label = "abusive_threat"
	elseif style.threatening then
		style.label = "threat"
	elseif style.abusive then
		style.label = "abuse"
	elseif style.pleading then
		style.label = "pleading"
	end
	return style
end

local function compute_insult_hostility_chance( player, npc, utterance, emotion_delta )
	if not npc then
		return 0
	end
	if not is_extreme_insult( utterance ) and ( tonumber( emotion_delta ) or 0 ) > -2 then
		return 0
	end
	local insult_tuning = tuning.insult or {}
	local speech = get_social_skill( player )
	local affinity = util.get_number( npc, "bntalk_affinity", 0 )
	local emotion_penalty = math.max( 0, -( tonumber( emotion_delta ) or 0 ) )
	local hostility = tuned_number( insult_tuning.base, 35 )
		+ emotion_penalty * tuned_number( insult_tuning.emotion_weight, 10 )
		- speech * tuned_number( insult_tuning.speech_weight, 2 )
		- math.floor( math.max( 0, affinity ) / tuned_number( insult_tuning.positive_affinity_divisor, 6 ) )
	if is_extreme_insult( utterance ) then
		hostility = hostility + tuned_number( insult_tuning.extreme_bonus, 10 )
	end
	return clamp_number( hostility, tuned_number( insult_tuning.minimum, 20 ), tuned_number( insult_tuning.maximum, 85 ) )
end

local function try_force_npc_hostile( player, npc, utterance, emotion_delta )
	if not npc or util.safe_bool_call( npc, "is_enemy" ) then
		return false, 0
	end
	local hostile_chance = compute_insult_hostility_chance( player, npc, utterance, emotion_delta )
	local hostile_roll = false
	if hostile_chance > 0 then
		hostile_roll, hostile_chance = roll_percent( hostile_chance )
	end
	if not hostile_roll then
		return false, hostile_chance
	end
	local ok_angry = select( 1, try_call_method( npc, "make_angry" ) )
	if util.safe_bool_call( npc, "is_enemy" ) then
		return true, hostile_chance
	end
	return ok_angry == true, hostile_chance
end

local function get_npc_attitude_text( npc )
	if not npc then
		return "unknown"
	end
	local ok, attitude = try_call_method( npc, "get_attitude" )
	if ok and attitude ~= nil and attitude ~= "" then
		return tostring( attitude )
	end
	return "unknown"
end

local function get_npc_attitude_number( npc )
	return tonumber( get_npc_attitude_text( npc ) ) or -1
end

local function try_force_npc_peaceful( npc, prefer_ally )
	if not npc then
		return false, "npc_missing"
	end
	local steps = {}
	local function note( label, ok )
		table.insert( steps, string.format( "%s=%s", label, ok and "ok" or "fail" ) )
	end
	local function resolved()
		return util.safe_bool_call( npc, "is_enemy" ) ~= true
	end

	table.insert( steps, "attitude_before=" .. get_npc_attitude_text( npc ) )
	table.insert( steps, "enemy_before=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )

	if resolved() then
		table.insert( steps, "already_non_hostile" )
		return true, table.concat( steps, ";" )
	end

	local primary = prefer_ally and "make_ally" or "make_friendly"
	local secondary = prefer_ally and "make_friendly" or "make_ally"

	local ok_primary = select( 1, try_call_method( npc, primary ) )
	note( primary, ok_primary )
	if resolved() then
		table.insert( steps, "attitude_after=" .. get_npc_attitude_text( npc ) )
		table.insert( steps, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
		return true, table.concat( steps, ";" )
	end

	local ok_secondary = select( 1, try_call_method( npc, secondary ) )
	note( secondary, ok_secondary )
	if resolved() then
		table.insert( steps, "attitude_after=" .. get_npc_attitude_text( npc ) )
		table.insert( steps, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
		return true, table.concat( steps, ";" )
	end

	local attitude_targets = prefer_ally
		and {
			{ label = "NPCATT_FOLLOW", value = 3 },
			{ label = "NPCATT_TALK", value = 1 },
			{ label = "NPCATT_NULL", value = 0 },
		}
		or {
			{ label = "NPCATT_TALK", value = 1 },
			{ label = "NPCATT_NULL", value = 0 },
			{ label = "NPCATT_FOLLOW", value = 3 },
		}
	for _, attitude in ipairs( attitude_targets ) do
		local ok_set = select( 1, try_call_method( npc, "set_attitude", attitude.value ) )
		note( "set_attitude(" .. attitude.label .. "=" .. tostring( attitude.value ) .. ")", ok_set )
		if resolved() then
			table.insert( steps, "attitude_after=" .. get_npc_attitude_text( npc ) )
			table.insert( steps, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
			return true, table.concat( steps, ";" )
		end
	end

	table.insert( steps, "attitude_after=" .. get_npc_attitude_text( npc ) )
	table.insert( steps, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
	return false, table.concat( steps, ";" )
end

local function is_recruit_state( npc )
	if not npc then
		return false
	end
	if util.safe_bool_call( npc, "is_following" ) then
		return true
	end
	return get_npc_attitude_number( npc ) == 3
end

local function try_force_npc_recruited( npc )
	if not npc then
		return false, "npc_missing"
	end
	local steps = {}
	local function note( label, ok )
		table.insert( steps, string.format( "%s=%s", label, ok and "ok" or "fail" ) )
	end

	table.insert( steps, "attitude_before=" .. get_npc_attitude_text( npc ) )
	table.insert( steps, "following_before=" .. tostring( util.safe_bool_call( npc, "is_following" ) ) )
	table.insert( steps, "enemy_before=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )

	if is_recruit_state( npc ) then
		table.insert( steps, "already_recruited" )
		return true, table.concat( steps, ";" )
	end

	local ok_ally = select( 1, try_call_method( npc, "make_ally" ) )
	note( "make_ally", ok_ally )
	if is_recruit_state( npc ) then
		table.insert( steps, "attitude_after=" .. get_npc_attitude_text( npc ) )
		table.insert( steps, "following_after=" .. tostring( util.safe_bool_call( npc, "is_following" ) ) )
		table.insert( steps, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
		return true, table.concat( steps, ";" )
	end

	local ok_set_follow = select( 1, try_call_method( npc, "set_attitude", 3 ) )
	note( "set_attitude(NPCATT_FOLLOW=3)", ok_set_follow )
	if is_recruit_state( npc ) then
		table.insert( steps, "attitude_after=" .. get_npc_attitude_text( npc ) )
		table.insert( steps, "following_after=" .. tostring( util.safe_bool_call( npc, "is_following" ) ) )
		table.insert( steps, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
		return true, table.concat( steps, ";" )
	end

	local ok_friendly = select( 1, try_call_method( npc, "make_friendly" ) )
	note( "make_friendly", ok_friendly )
	local ok_set_talk = select( 1, try_call_method( npc, "set_attitude", 1 ) )
	note( "set_attitude(NPCATT_TALK=1)", ok_set_talk )

	table.insert( steps, "attitude_after=" .. get_npc_attitude_text( npc ) )
	table.insert( steps, "following_after=" .. tostring( util.safe_bool_call( npc, "is_following" ) ) )
	table.insert( steps, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
	return is_recruit_state( npc ), table.concat( steps, ";" )
end

local function clear_temporary_mercy( npc )
	if npc then
		npc:set_value( "bntalk_mercy_until_turn", "" )
		npc:set_value( "bntalk_mercy_active", "" )
	end
end

local function drop_mercy_job_for_npc( npc )
	ensure_runtime_state()
	for index = #mod.mercy_jobs, 1, -1 do
		if mod.mercy_jobs[index].npc_ref == npc then
			table.remove( mod.mercy_jobs, index )
		end
	end
end

local function queue_temporary_mercy( npc, duration_turns )
	ensure_runtime_state()
	local mercy_until_turn = current_turn_number() + ( duration_turns or 60 )
	clear_temporary_mercy( npc )
	drop_mercy_job_for_npc( npc )
	npc:set_value( "bntalk_mercy_until_turn", tostring( mercy_until_turn ) )
	npc:set_value( "bntalk_mercy_active", "yes" )
	table.insert( mod.mercy_jobs, {
		npc_ref = npc,
		npc_name = util.safe_name( npc ),
		until_turn = mercy_until_turn,
	} )
end

local function apply_mercy_pause( npc )
	if not npc then
		return false
	end
	local ok_effect_id, effect_id = pcall( function()
		return EffectTypeId.new( "stunned" )
	end )
	if not ok_effect_id or not effect_id then
		return false
	end
	local ok_duration, duration = pcall( function()
		return TimeDuration.from_seconds( MERCY_FREEZE_SECONDS )
	end )
	if not ok_duration or not duration then
		return false
	end
	local ok_apply = select( 1, try_call_method( npc, "add_effect", effect_id, duration ) )
	return ok_apply == true
end

local function process_mercy_jobs()
	ensure_runtime_state()
	local now_turn = current_turn_number()
	for index = #mod.mercy_jobs, 1, -1 do
		local job = mod.mercy_jobs[index]
		local npc = job.npc_ref
		local remove_job = false
		if not npc then
			remove_job = true
		elseif util.safe_bool_call( npc, "is_enemy" ) then
			clear_temporary_mercy( npc )
			remove_job = true
		elseif now_turn >= ( job.until_turn or 0 ) then
			clear_temporary_mercy( npc )
			local was_enemy = util.safe_bool_call( npc, "is_enemy" )
			try_call_method( npc, "make_angry" )
			if not was_enemy and util.safe_bool_call( npc, "is_enemy" ) then
				gapi.add_msg( MsgType.bad, string.format( "%s stops tolerating you and turns hostile again.", util.safe_name( npc ) ) )
			end
			remove_job = true
		end
		if remove_job then
			table.remove( mod.mercy_jobs, index )
		end
	end
end

local function compute_beg_chances( player, npc )
	local speech = get_social_skill( player )
	local affinity = util.get_number( npc, "bntalk_affinity", 0 )
	local beg_tuning = tuning.beg or {}
	local forgive_chance = clamp_number(
		tuned_number( beg_tuning.forgive_base, 2 )
		+ speech * tuned_number( beg_tuning.forgive_speech_weight, 3 )
		+ math.max( 0, affinity ) * tuned_number( beg_tuning.forgive_affinity_weight, 1 ),
		tuned_number( beg_tuning.forgive_minimum, 1 ),
		tuned_number( beg_tuning.forgive_maximum, 35 )
	)
	local mercy_chance = clamp_number(
		tuned_number( beg_tuning.mercy_base, 15 )
		+ speech * tuned_number( beg_tuning.mercy_speech_weight, 5 )
		+ math.max( 0, affinity ) * tuned_number( beg_tuning.mercy_affinity_weight, 1 ),
		tuned_number( beg_tuning.mercy_minimum, 5 ),
		tuned_number( beg_tuning.mercy_maximum, 85 )
	)
	return forgive_chance, mercy_chance
end

local function plan_beg_outcome( player, npc, utterance )
	if not npc or not util.safe_bool_call( npc, "is_enemy" ) then
		return {
			outcome = "invalid",
			forgive_chance = 0,
			mercy_chance = 0,
			forgive_roll_value = 0,
			mercy_roll_value = 0,
			debug = "npc_not_hostile",
			prompt_summary = "The NPC is not currently hostile. If you return an interaction_outcome, it should be invalid or fail.",
			tone_label = "plain",
		}
	end
	local style = analyze_utterance_style( utterance )
	local speech = get_social_skill( player )
	local affinity = util.get_number( npc, "bntalk_affinity", 0 )
	local debug_parts = {
		string.format( "speech=%d", speech ),
		string.format( "affinity=%d", affinity ),
		"attitude_before=" .. get_npc_attitude_text( npc ),
		"utterance_style=" .. tostring( style.label ),
	}
	local forgive_chance, mercy_chance = compute_beg_chances( player, npc )
	local forgive_roll_value = sample_percent_roll()
	local mercy_roll_value = sample_percent_roll()
	table.insert( debug_parts, "forgive_roll_value=" .. tostring( forgive_roll_value ) )
	table.insert( debug_parts, "mercy_roll_value=" .. tostring( mercy_roll_value ) )
	return {
		outcome = "pending",
		forgive_chance = forgive_chance,
		mercy_chance = mercy_chance,
		forgive_roll_value = forgive_roll_value,
		mercy_roll_value = mercy_roll_value,
		debug = table.concat( debug_parts, ";" ),
		prompt_summary = "Use the provided beg roll values and base chances as the baseline dice result. Judge whether the utterance tone, wording, and context should shift the effective chance up or down, then return interaction_outcome as forgive, mercy, fail, or invalid.",
		tone_label = style.label,
	}
end

local function fallback_beg_outcome_from_plan( plan )
	if not plan then
		return "fail"
	end
	local outcome = tostring( plan.outcome or "" )
	if outcome == "invalid" then
		return "invalid"
	end
	local forgive_roll_value = tonumber( plan.forgive_roll_value ) or 101
	local mercy_roll_value = tonumber( plan.mercy_roll_value ) or 101
	local forgive_chance = tonumber( plan.forgive_chance ) or 0
	local mercy_chance = tonumber( plan.mercy_chance ) or 0
	if forgive_roll_value <= forgive_chance then
		return "forgive"
	end
	if mercy_roll_value <= mercy_chance then
		return "mercy"
	end
	return "fail"
end

local function apply_beg_outcome( player, npc, plan, interaction_outcome )
	plan = plan or plan_beg_outcome( player, npc )
	local debug_parts = {}
	if plan.debug and plan.debug ~= "" then
		table.insert( debug_parts, tostring( plan.debug ) )
	end
	local forgive_chance = tonumber( plan.forgive_chance ) or 0
	local mercy_chance = tonumber( plan.mercy_chance ) or 0
	local resolved_outcome = tostring( interaction_outcome or "" )
	if resolved_outcome == "" or resolved_outcome == "pending" then
		resolved_outcome = fallback_beg_outcome_from_plan( plan )
		table.insert( debug_parts, "fallback_outcome=" .. resolved_outcome )
	else
		table.insert( debug_parts, "ai_outcome=" .. resolved_outcome )
	end
	if resolved_outcome == "invalid" then
		return "invalid", forgive_chance, mercy_chance, table.concat( debug_parts, ";" )
	end
	if not npc or ( resolved_outcome ~= "fail" and not util.safe_bool_call( npc, "is_enemy" ) ) then
		if not npc then
			table.insert( debug_parts, "npc_missing_at_apply" )
		else
			table.insert( debug_parts, "npc_already_non_hostile_at_apply" )
		end
		return resolved_outcome == "fail" and "fail" or "invalid", forgive_chance, mercy_chance, table.concat( debug_parts, ";" )
	end
	if resolved_outcome == "forgive" then
		clear_temporary_mercy( npc )
		drop_mercy_job_for_npc( npc )
		local peaceful, transition_debug = try_force_npc_peaceful( npc, true )
		table.insert( debug_parts, "forgive_transition=" .. transition_debug )
		if peaceful then
			memory.add_affinity( npc, 2 )
			return "forgive", forgive_chance, mercy_chance, table.concat( debug_parts, ";" )
		end
	elseif resolved_outcome == "mercy" then
		local peaceful, transition_debug = try_force_npc_peaceful( npc, false )
		table.insert( debug_parts, "mercy_transition=" .. transition_debug )
		if peaceful then
			queue_temporary_mercy( npc, 60 )
			return "mercy", forgive_chance, mercy_chance, table.concat( debug_parts, ";" )
		end
	else
		table.insert( debug_parts, "resolved_fail" )
	end
	if npc then
		table.insert( debug_parts, "attitude_after=" .. get_npc_attitude_text( npc ) )
		table.insert( debug_parts, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
	end
	return "fail", forgive_chance, mercy_chance, table.concat( debug_parts, ";" )
end

local function compute_recruit_chance( player, npc )
	local speech = get_social_skill( player )
	local affinity = util.get_number( npc, "bntalk_affinity", 0 )
	local recruit_tuning = tuning.recruit or {}
	return clamp_number(
		tuned_number( recruit_tuning.base, 3 )
		+ speech * tuned_number( recruit_tuning.speech_weight, 4 )
		+ math.max( 0, affinity ) * tuned_number( recruit_tuning.affinity_weight, 2 ),
		tuned_number( recruit_tuning.minimum, 1 ),
		tuned_number( recruit_tuning.maximum, 45 )
	)
end

local function plan_recruit_outcome( player, npc, utterance )
	if not npc or util.safe_bool_call( npc, "is_enemy" ) then
		return {
			outcome = "invalid",
			recruit_chance = 0,
			recruit_roll_value = 0,
			debug = "npc_hostile",
			prompt_summary = "The NPC is hostile, so recruitment cannot succeed right now. If you return interaction_outcome, it should be invalid or fail.",
			tone_label = "plain",
		}
	end
	local style = analyze_utterance_style( utterance )
	local speech = get_social_skill( player )
	local affinity = util.get_number( npc, "bntalk_affinity", 0 )
	local debug_parts = {
		string.format( "speech=%d", speech ),
		string.format( "affinity=%d", affinity ),
		"attitude_before=" .. get_npc_attitude_text( npc ),
		"following_before=" .. tostring( util.safe_bool_call( npc, "is_following" ) ),
		"utterance_style=" .. tostring( style.label ),
	}
	local recruit_chance = compute_recruit_chance( player, npc )
	local recruit_roll_value = sample_percent_roll()
	table.insert( debug_parts, "recruit_roll_value=" .. tostring( recruit_roll_value ) )
	return {
		outcome = "pending",
		recruit_chance = recruit_chance,
		recruit_roll_value = recruit_roll_value,
		debug = table.concat( debug_parts, ";" ),
		prompt_summary = "Use the provided recruit roll value and base chance as the baseline dice result. Judge whether the utterance tone, wording, and context should shift the effective chance up or down, then return interaction_outcome as success, fail, or invalid.",
		tone_label = style.label,
	}
end

local function fallback_recruit_outcome_from_plan( plan )
	if not plan then
		return "fail"
	end
	local outcome = tostring( plan.outcome or "" )
	if outcome == "invalid" then
		return "invalid"
	end
	local recruit_roll_value = tonumber( plan.recruit_roll_value ) or 101
	local recruit_chance = tonumber( plan.recruit_chance ) or 0
	if recruit_roll_value <= recruit_chance then
		return "success"
	end
	return "fail"
end

local function apply_recruit_outcome( player, npc, plan, interaction_outcome )
	plan = plan or plan_recruit_outcome( player, npc )
	local debug_parts = {}
	if plan.debug and plan.debug ~= "" then
		table.insert( debug_parts, tostring( plan.debug ) )
	end
	local recruit_chance = tonumber( plan.recruit_chance ) or 0
	local resolved_outcome = tostring( interaction_outcome or "" )
	if resolved_outcome == "" or resolved_outcome == "pending" then
		resolved_outcome = fallback_recruit_outcome_from_plan( plan )
		table.insert( debug_parts, "fallback_outcome=" .. resolved_outcome )
	else
		table.insert( debug_parts, "ai_outcome=" .. resolved_outcome )
	end
	if resolved_outcome == "invalid" then
		return false, recruit_chance, table.concat( debug_parts, ";" )
	end
	if resolved_outcome ~= "success" then
		if npc then
			table.insert( debug_parts, "attitude_after=" .. get_npc_attitude_text( npc ) )
			table.insert( debug_parts, "following_after=" .. tostring( util.safe_bool_call( npc, "is_following" ) ) )
			table.insert( debug_parts, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
		end
		return false, recruit_chance, table.concat( debug_parts, ";" )
	end
	if not npc or util.safe_bool_call( npc, "is_enemy" ) then
		if not npc then
			table.insert( debug_parts, "npc_missing_at_apply" )
		else
			table.insert( debug_parts, "npc_hostile_at_apply" )
		end
		return false, recruit_chance, table.concat( debug_parts, ";" )
	end
	local recruited, transition_debug = try_force_npc_recruited( npc )
		table.insert( debug_parts, "recruit_transition=" .. transition_debug )
	if recruited then
		memory.add_affinity( npc, 1 )
		return true, recruit_chance, table.concat( debug_parts, ";" )
	end
	if npc then
		table.insert( debug_parts, "attitude_after=" .. get_npc_attitude_text( npc ) )
		table.insert( debug_parts, "following_after=" .. tostring( util.safe_bool_call( npc, "is_following" ) ) )
		table.insert( debug_parts, "enemy_after=" .. tostring( util.safe_bool_call( npc, "is_enemy" ) ) )
	end
	return false, recruit_chance, table.concat( debug_parts, ";" )
end

local function queue_async_utterance( player, npc, utterance, phase_name, intent )
	ensure_runtime_state()
	poll_async_jobs()
	phase_name = tostring( phase_name or "free_talk_submit" )
	intent = tostring( intent or "talk" )
	local existing_request_id = refresh_npc_pending_state( npc )
	if existing_request_id ~= "" then
		gapi.add_msg( MsgType.info, string.format( "BN Talk: %s already has a pending AI reply.", util.safe_name( npc ) ) )
		return false
	end

	local provider_impl, provider_name, bridge_status = select_provider()
	if provider_impl ~= bridge_provider or provider_name ~= "native_bridge" then
		local err = bridge_status and tostring( bridge_status.error or "native_bridge_required" ) or "native_bridge_required"
		gapi.add_msg( MsgType.info, "BN Talk async talk requires the injected native bridge. error=" .. err )
		return false
	end

	memory.note_interaction( npc, phase_name )
	util.set_number( npc, "bntalk_times_spoken", util.get_number( npc, "bntalk_times_spoken", 0 ) + 1 )
	npc:set_value( "bntalk_last_player_utterance", tostring( utterance ) )

	local ctx = context_builder.build( npc, player, phase_name )
	local plan = nil
	if intent == "beg" then
		plan = plan_beg_outcome( player, npc, utterance )
	elseif intent == "recruit" then
		plan = plan_recruit_outcome( player, npc, utterance )
	end
	if plan then
		ctx.planned_intent = intent
		ctx.planned_outcome = tostring( plan.outcome or "" )
		ctx.planned_summary = tostring( plan.prompt_summary or "" )
		ctx.planned_debug = tostring( plan.debug or "" )
		ctx.planned_tone = tostring( plan.tone_label or "plain" )
		if plan.forgive_chance ~= nil then
			ctx.planned_forgive_chance = tonumber( plan.forgive_chance ) or 0
		end
		if plan.mercy_chance ~= nil then
			ctx.planned_mercy_chance = tonumber( plan.mercy_chance ) or 0
		end
		if plan.forgive_roll_value ~= nil then
			ctx.planned_forgive_roll = tonumber( plan.forgive_roll_value ) or 0
		end
		if plan.mercy_roll_value ~= nil then
			ctx.planned_mercy_roll = tonumber( plan.mercy_roll_value ) or 0
		end
		if plan.recruit_chance ~= nil then
			ctx.planned_recruit_chance = tonumber( plan.recruit_chance ) or 0
		end
		if plan.recruit_roll_value ~= nil then
			ctx.planned_recruit_roll = tonumber( plan.recruit_roll_value ) or 0
		end
	end
	local request = provider_impl.build_request( ctx, {
		request_kind = "enqueue_utterance",
		utterance = utterance,
		interaction_intent = intent,
		social_skill = get_social_skill( player ),
		is_enemy = util.safe_bool_call( npc, "is_enemy" ),
		is_following = util.safe_bool_call( npc, "is_following" ),
	} )
	local response, provider_error = complete_request_via_native( request )
	if not response then
		storage.stats.bridge_failures = ( storage.stats.bridge_failures or 0 ) + 1
		storage.bridge.last_error = tostring( provider_error or "async_enqueue_failed" )
		gapi.add_msg( MsgType.info, "BN Talk async enqueue failed: " .. tostring( provider_error or "unknown" ) )
		return false
	end

	local request_id = response.bntalk and tostring( response.bntalk.request_id or "" ) or ""
	if request_id == "" then
		gapi.add_msg( MsgType.info, "BN Talk async enqueue failed: sidecar returned no request_id." )
		return false
	end

	npc:set_value( "bntalk_pending_request_id", request_id )
	mod.pending_jobs[request_id] = {
		request_id = request_id,
		npc_name = util.safe_name( npc ),
		npc_ref = npc,
		utterance = utterance,
		intent = intent,
		phase = phase_name,
		snapshot = ctx,
		plan = plan,
	}

	storage.stats.async_submitted = ( storage.stats.async_submitted or 0 ) + 1
	storage.stats.last_async_request_id = request_id
	storage.bridge.last_error = ""
	storage.bridge.last_provider = provider_name
	storage.bridge.last_mode = response.bntalk and tostring( response.bntalk.mode or "background_pending" ) or "background_pending"

	gapi.add_msg( MsgType.info, string.format( "[bntalk] You to %s: %s", util.safe_name( npc ), utterance ) )
	poll_async_jobs()
	if util.is_debug( player ) then
		gapi.add_msg( MsgType.info, string.format( "[bntalk] queued request_id=%s pending=%d", request_id, count_pending_jobs() ) )
	end
	return true
end

local function resolve_job_npc( player, request_id, job )
	local npc = job and job.npc_ref or nil
	if npc then
		local ok_pending, pending_request_id = pcall( function()
			return npc:get_value( "bntalk_pending_request_id" )
		end )
		if ok_pending and ( pending_request_id == "" or pending_request_id == request_id ) then
			return npc
		end
	end
	return find_visible_npc_by_request( player, request_id )
end

local function deliver_async_result( player, request_id, job, response )
	local result = response and response.bntalk or {}
	local generated_text = tostring( result.generated_text or "" )
	local provider_name = tostring( result.provider or "native_bridge" )
	local emotion_delta = tonumber( result.emotion_delta ) or 0

	storage.bridge.last_provider = provider_name
	storage.bridge.last_mode = tostring( result.mode or "background_ready" )
	storage.bridge.last_error = ""

	local npc = resolve_job_npc( player, request_id, job )
	local speaker_name = npc and util.safe_name( npc ) or tostring( job.npc_name or "unknown" )
	local final_text = generated_text

	storage.stats.last_generated_text = generated_text
	storage.bridge.last_generated_text = generated_text

	if npc then
		local was_enemy = util.safe_bool_call( npc, "is_enemy" )
		npc:set_value( "bntalk_pending_request_id", "" )
		npc:set_value( "bntalk_last_player_utterance", tostring( job.utterance or "" ) )
		memory.note_interaction( npc, tostring( job.intent or "free_talk_reply" ) )
		if result.topic_id and tostring( result.topic_id ) ~= "" then
			memory.set_last_topic( npc, tostring( result.topic_id ) )
		end
		if emotion_delta ~= 0 then
			memory.add_affinity( npc, emotion_delta )
			if util.is_debug( player ) then
				gapi.add_msg( MsgType.info, string.format( "[bntalk] affinity %s %+d -> %d", util.safe_name( npc ), emotion_delta, util.get_number( npc, "bntalk_affinity", 0 ) ) )
			end
		end

		local interaction_outcome = tostring( result.interaction_outcome or "" )
		if job.intent == "beg" then
			local ai_text = generated_text
			local beg_outcome, forgive_chance, mercy_chance, beg_debug = apply_beg_outcome( player, npc, job.plan, interaction_outcome )
			if beg_outcome == "forgive" then
				final_text = string.format( "%s\n\n[result] lowers their weapon and decides to let you live.", ai_text ~= "" and ai_text or "……" )
				gapi.add_msg( MsgType.good, string.format( "%s lowers their weapon and decides to let you live.", util.safe_name( npc ) ) )
			elseif beg_outcome == "mercy" then
				local paused = apply_mercy_pause( npc )
				final_text = string.format( "%s\n\n[result] gives you one minute to get lost.", ai_text ~= "" and ai_text or "……" )
				if paused then
					gapi.add_msg( MsgType.warning, string.format( "%s gives you one minute to get lost and freezes for %d seconds.", util.safe_name( npc ), MERCY_FREEZE_SECONDS ) )
				else
					gapi.add_msg( MsgType.warning, string.format( "%s gives you one minute to get lost.", util.safe_name( npc ) ) )
				end
			elseif beg_outcome == "fail" then
				final_text = string.format( "%s\n\n[result] refuses your plea and keeps attacking.", ai_text ~= "" and ai_text or "……" )
				gapi.add_msg( MsgType.bad, string.format( "%s refuses your plea and keeps attacking.", util.safe_name( npc ) ) )
				try_call_method( npc, "make_angry" )
			end
			if util.is_debug( player ) then
				gapi.add_msg( MsgType.info, string.format( "[bntalk] beg chances forgive=%d mercy=%d", forgive_chance, mercy_chance ) )
				if beg_debug and beg_debug ~= "" then
					gapi.add_msg( MsgType.info, string.format( "[bntalk] beg state %s", beg_debug ) )
				end
			end
		elseif job.intent == "recruit" then
			local ai_text = generated_text
			local recruit_success, recruit_chance, recruit_debug = apply_recruit_outcome( player, npc, job.plan, interaction_outcome )
			if recruit_success then
				final_text = string.format( "%s\n\n[result] agrees to join you.", ai_text ~= "" and ai_text or "……" )
				gapi.add_msg( MsgType.good, string.format( "%s agrees to join you.", util.safe_name( npc ) ) )
			else
				final_text = string.format( "%s\n\n[result] refuses to join you.", ai_text ~= "" and ai_text or "……" )
				gapi.add_msg( MsgType.info, string.format( "%s refuses to join you.", util.safe_name( npc ) ) )
			end
			if util.is_debug( player ) then
				gapi.add_msg( MsgType.info, string.format( "[bntalk] recruit chance=%d", recruit_chance ) )
				if recruit_debug and recruit_debug ~= "" then
					gapi.add_msg( MsgType.info, string.format( "[bntalk] recruit state %s", recruit_debug ) )
				end
			end
		else
			local turned_hostile, hostile_chance = try_force_npc_hostile( player, npc, tostring( job.utterance or "" ), emotion_delta )
			if turned_hostile and not was_enemy then
				npc:set_value( "bntalk_hostile_due_to_abuse", "yes" )
				gapi.add_msg( MsgType.bad, string.format( "[bntalk] %s turns hostile after extreme verbal abuse.", util.safe_name( npc ) ) )
			elseif util.is_debug( player ) and hostile_chance > 0 then
				gapi.add_msg( MsgType.info, string.format( "[bntalk] hostility chance=%d resisted", hostile_chance ) )
			end
		end

		npc:set_value( "bntalk_last_reply_text", tostring( final_text or "" ) )
		if final_text ~= "" then
			gapi.add_msg( MsgType.warning, string.format( "%s: %s", speaker_name, final_text ) )
		end
		storage.stats.async_completed = ( storage.stats.async_completed or 0 ) + 1
	else
		if final_text ~= "" then
			gapi.add_msg( MsgType.warning, string.format( "%s: %s", speaker_name, final_text ) )
		end
		storage.stats.async_discarded = ( storage.stats.async_discarded or 0 ) + 1
		if util.is_debug( player ) then
			gapi.add_msg( MsgType.info, string.format( "[bntalk] delivered log-only reply for %s because live NPC handle was unavailable; affinity update skipped.", speaker_name ) )
		end
	end
end

poll_async_jobs = function()
	ensure_runtime_state()
	local player = gapi.get_avatar()
	if not player or not next( mod.pending_jobs ) or not bridge_provider or not bridge_provider.build_request or not bridge_provider.complete then
		return
	end

	local completed_request_ids = {}
	for request_id, job in pairs( mod.pending_jobs ) do
		local snapshot = job.snapshot or {
			phase = "free_talk_poll",
			npc_name = tostring( job.npc_name or "someone" ),
			player_name = util.safe_name( player ),
			affinity = 0,
			last_topic = storage.stats and storage.stats.last_topic or "",
			last_event = "free_talk_submit",
			times_spoken = 0,
			last_seen_turn = util.current_turn_text(),
			following = false,
			friend = false,
			enemy = false,
		}
		snapshot.phase = "free_talk_poll"
		snapshot.player_name = util.safe_name( player )
		snapshot.last_seen_turn = util.current_turn_text()

		local request = bridge_provider.build_request( snapshot, {
			request_kind = "poll_utterance",
			request_id = request_id,
		} )
		local response, err = complete_request_via_native( request )
		local result = response and response.bntalk or nil
		local result_request_id = result and tostring( result.request_id or "" ) or ""
		if result and result.ready == true and result_request_id == request_id then
			deliver_async_result( player, request_id, job, response )
			table.insert( completed_request_ids, request_id )
		elseif result and result.ready == true then
			job.poll_failures = ( job.poll_failures or 0 ) + 1
			if util.is_debug( player ) and job.poll_failures == 1 then
				gapi.add_msg( MsgType.info, string.format( "[bntalk] poll returned mismatched request_id for %s: %s", request_id, result_request_id ) )
			end
		elseif not response then
			job.poll_failures = ( job.poll_failures or 0 ) + 1
			if util.is_debug( player ) and job.poll_failures == 1 then
				gapi.add_msg( MsgType.info, string.format( "[bntalk] poll failed for %s: %s", request_id, tostring( err or "unknown" ) ) )
			end
		end
	end

	for _, request_id in ipairs( completed_request_ids ) do
		mod.pending_jobs[request_id] = nil
	end
end

local function start_async_talk( player )
	poll_async_jobs()
	local npc = choose_nearby_npc( player )
	if not npc then
		return
	end

	local utterance, input_error = query_custom_utterance()
	if not utterance then
		if input_error and input_error ~= "" then
			gapi.add_msg( MsgType.info, "BN Talk input failed: " .. tostring( input_error ) )
		end
		return
	end

	queue_async_utterance( player, npc, utterance, "free_talk_submit", "talk" )
end

local function start_beg_talk( player )
	poll_async_jobs()
	local npc = choose_nearby_npc( player, "BN Talk: beg hostile NPC", function( candidate )
		return util.safe_bool_call( candidate, "is_enemy" )
	end )
	if not npc then
		return
	end

	local utterance, input_error = query_custom_utterance( "Beg for mercy from the hostile NPC.", "BN Talk: Beg", "Please don't kill me." )
	if not utterance then
		if input_error and input_error ~= "" then
			gapi.add_msg( MsgType.info, "BN Talk input failed: " .. tostring( input_error ) )
		end
		return
	end

	queue_async_utterance( player, npc, utterance, "beg_submit", "beg" )
end

local function start_recruit_talk( player )
	poll_async_jobs()
	local npc = choose_nearby_npc( player, "BN Talk: invite NPC to join", function( candidate )
		return not util.safe_bool_call( candidate, "is_enemy" ) and not util.safe_bool_call( candidate, "is_following" )
	end )
	if not npc then
		return
	end

	local utterance, input_error = query_custom_utterance( "Ask the NPC to come with you.", "BN Talk: Recruit", "Come with me. We can survive together." )
	if not utterance then
		if input_error and input_error ~= "" then
			gapi.add_msg( MsgType.info, "BN Talk input failed: " .. tostring( input_error ) )
		end
		return
	end

	queue_async_utterance( player, npc, utterance, "recruit_submit", "recruit" )
end

mod.on_game_started = function()
	local player = ensure_player_ready()
	if not player then
		return
	end

	gapi.add_msg( MsgType.good, "BN Talk has been initialized. You have obtained the BN Talk Console, which can be used to switch on/off and debug." )
end

mod.on_game_load = function()
	ensure_player_ready()
end

mod.on_npc_interaction = function( params )
	local player = ensure_player_ready()
	if not player or not util.is_enabled( player ) then
		return
	end

	local npc = params and params.npc
	if not npc then
		return
	end

	memory.note_interaction( npc, "npc_interaction" )
end

mod.on_dialogue_start = function( params )
	local passthrough_topic = passthrough_dialogue_topic( params, "TALK_DONE" )
	local player = ensure_player_ready()
	if not player or not util.is_enabled( player ) then
		return passthrough_topic
	end

	local npc = params and params.npc
	if npc then
		memory.note_interaction( npc, "dialogue_start" )
	end
	return passthrough_topic
end

mod.on_dialogue_option = function( params )
	local passthrough_topic = passthrough_dialogue_topic( params, "TALK_DONE" )
	local player = gapi.get_avatar()
	if not player or not util.is_enabled( player ) then
		return passthrough_topic
	end

	local next_topic = passthrough_topic
	if util.is_debug( player ) and next_topic then
		gapi.add_msg( MsgType.info, "[bntalk] option -> " .. next_topic )
	end

	return next_topic
end

mod.on_dialogue_end = function( params )
	local player = gapi.get_avatar()
	if not player or not util.is_enabled( player ) then
		return
	end

	local npc = params and params.npc
	if not npc then
		return
	end

	memory.note_interaction( npc, "dialogue_end" )
end

mod.on_every_second = function()
	local player = ensure_player_ready()
	process_mercy_jobs()
	if player and util.is_enabled( player ) then
		poll_async_jobs()
	end
	return true
end

mod.on_every_30_minutes = function()
	local player = gapi.get_avatar()
	if player then
		storage.stats = storage.stats or {}
		storage.stats.pulses = ( storage.stats.pulses or 0 ) + 1
	end
	return true
end

local function run_bridge_self_test( who )
	local status = bridge_provider and bridge_provider.status and bridge_provider.status() or {
		available = false,
		error = "bridge_module_missing",
	}
	gapi.add_msg( MsgType.info, "BN Talk self-test bridge available=" .. tostring( status.available == true ) )
	if status.error and status.error ~= "" then
		gapi.add_msg( MsgType.info, "BN Talk self-test status error=" .. tostring( status.error ) )
	end
	if not bridge_provider or not bridge_provider.build_request or not bridge_provider.complete then
		gapi.add_msg( MsgType.info, "BN Talk self-test failed: bridge provider module missing" )
		return
	end

	local ctx = {
		phase = "self_test",
		npc_name = "bridge_test_npc",
		player_name = util.safe_name( who ),
		affinity = 4,
		last_topic = storage.stats and storage.stats.last_topic or "",
		last_event = "self_test",
		times_spoken = 2,
		last_seen_turn = util.current_turn_text(),
		following = false,
		friend = true,
		enemy = false,
	}
	local request = bridge_provider.build_request( ctx )
	local response, err = bridge_provider.complete( request )
	if not response then
		gapi.add_msg( MsgType.info, "BN Talk self-test complete() failed: " .. tostring( err or "unknown" ) )
		return
	end

	local route = response.bntalk and tostring( response.bntalk.topic_id or "" ) or ""
	local provider_name = response.bntalk and tostring( response.bntalk.provider or "" ) or ""
	local generated_text = response.bntalk and tostring( response.bntalk.generated_text or "" ) or ""
	local emotion_delta = response.bntalk and tonumber( response.bntalk.emotion_delta ) or 0
	gapi.add_msg( MsgType.info, string.format( "BN Talk self-test provider=%s route=%s emotion=%+d", provider_name, route, emotion_delta ) )
	if generated_text ~= "" then
		gapi.add_msg( MsgType.info, "BN Talk self-test generated=" .. generated_text )
	end
end

mod.open_debug_menu = function( who, item, pos )
	if not who or not who:is_avatar() then
		return 0
	end

	ensure_player_ready()
	if util.is_enabled( who ) then
		poll_async_jobs()
	end
	local enabled = util.is_enabled( who )
	local debug = util.is_debug( who )

	local menu = UiList.new()
	menu:title( "BN Talk Console" )
	menu:add( 2, "Talk to nearby NPC (async)" )
	menu:add( 9, "Beg hostile NPC (async)" )
	menu:add( 10, "Invite nearby NPC to join (async)" )
	menu:add( 0, string.format( "Toggle BN Talk [%s]", enabled and "ON" or "OFF" ) )
	menu:add( 1, string.format( "Toggle debug lines [%s]", debug and "ON" or "OFF" ) )
	menu:add( 3, "Show bridge status" )
	menu:add( 4, "Show pending async jobs" )
	menu:add( 5, "Show last generated line" )
	menu:add( 6, "Show Lua package/loadlib probe" )
	menu:add( 7, "Run native bridge self-test" )
	menu:add( 8, "Show quick test hint" )
	menu:add( 11, "Close" )

	local choice = menu:query()
	if choice == 0 then
		util.set_bool( who, "bntalk_enabled", not enabled )
		gapi.add_msg( MsgType.good, "BN Talk is now " .. ( not enabled and "ON" or "OFF" ) )
	elseif choice == 1 then
		util.set_bool( who, "bntalk_debug", not debug )
		gapi.add_msg( MsgType.good, "BN Talk debug is now " .. ( not debug and "ON" or "OFF" ) )
	elseif choice == 2 then
		start_async_talk( who )
	elseif choice == 3 then
		local status = bridge_provider and bridge_provider.status and bridge_provider.status() or {
			available = false,
			error = "bridge_module_missing",
			version = "",
			bridge_mode = "detached",
		}
		gapi.add_msg( MsgType.info, string.format( "BN Talk bridge available=%s mode=%s version=%s", tostring( status.available == true ), tostring( status.bridge_mode or "detached" ), tostring( status.version or "" ) ) )
		if status.error and status.error ~= "" then
			gapi.add_msg( MsgType.info, "BN Talk bridge error: " .. tostring( status.error ) )
		end
	elseif choice == 4 then
		gapi.add_msg( MsgType.info, string.format( "BN Talk pending jobs=%d submitted=%d completed=%d discarded=%d", count_pending_jobs(), storage.stats.async_submitted or 0, storage.stats.async_completed or 0, storage.stats.async_discarded or 0 ) )
	elseif choice == 5 then
		local last_line = storage.bridge and storage.bridge.last_generated_text or ""
		if last_line == "" then
			last_line = "No generated text captured yet. Use the async BN Talk option after injecting the bridge DLL."
		end
		gapi.add_msg( MsgType.info, last_line )
	elseif choice == 6 then
		local pkg = rawget( _G, "package" )
		local loadlib_type = pkg and type( pkg.loadlib ) or "nil"
		local cpath_value = pkg and tostring( pkg.cpath or "" ) or ""
		local searchers_type = pkg and type( pkg.searchers or pkg.loaders ) or "nil"
		gapi.add_msg( MsgType.info, "BN Talk package=" .. tostring( type( pkg ) ) .. " loadlib=" .. tostring( loadlib_type ) .. " searchers=" .. tostring( searchers_type ) )
		if cpath_value ~= "" then
			gapi.add_msg( MsgType.info, "BN Talk package.cpath=" .. cpath_value )
		end
	elseif choice == 7 then
		run_bridge_self_test( who )
	elseif choice == 8 then
		local beg_tuning = tuning.beg or {}
		local recruit_tuning = tuning.recruit or {}
		local insult_tuning = tuning.insult or {}
		gapi.add_msg( MsgType.info, "Open BN Talk Console, choose 'Talk to nearby NPC (async)', select an NPC, type your line, then keep playing while the reply is generated in the background." )
		gapi.add_msg( MsgType.info, "If the NPC is still nearby when the sidecar finishes, BN Talk prints the reply to the message log and applies emotion_delta to affinity." )
		gapi.add_msg( MsgType.info, string.format( "[bntalk] beg forgive=%d+speech*%d+aff*%d mercy=%d+speech*%d+aff*%d", tuned_number( beg_tuning.forgive_base, 2 ), tuned_number( beg_tuning.forgive_speech_weight, 3 ), tuned_number( beg_tuning.forgive_affinity_weight, 1 ), tuned_number( beg_tuning.mercy_base, 15 ), tuned_number( beg_tuning.mercy_speech_weight, 5 ), tuned_number( beg_tuning.mercy_affinity_weight, 1 ) ) )
		gapi.add_msg( MsgType.info, string.format( "[bntalk] recruit=%d+speech*%d+aff*%d insult=%d+emo*%d-speech*%d", tuned_number( recruit_tuning.base, 3 ), tuned_number( recruit_tuning.speech_weight, 4 ), tuned_number( recruit_tuning.affinity_weight, 2 ), tuned_number( insult_tuning.base, 35 ), tuned_number( insult_tuning.emotion_weight, 10 ), tuned_number( insult_tuning.speech_weight, 2 ) ) )
	elseif choice == 9 then
		start_beg_talk( who )
	elseif choice == 10 then
		start_recruit_talk( who )
	end

	return 0
end