---@diagnostic disable: undefined-global

local M = {}

local function value_or_default( value, default )
	if value == nil or value == "" then
		return default
	end
	return value
end

function M.ensure_value( obj, key, default )
	if not obj or not key then
		return
	end
	local current = obj:get_value( key )
	if current == "" then
		obj:set_value( key, tostring( default ) )
	end
end

function M.get_number( obj, key, default )
	default = default or 0
	if not obj or not key then
		return default
	end
	return tonumber( value_or_default( obj:get_value( key ), default ) ) or default
end

function M.set_number( obj, key, value )
	if obj and key then
		obj:set_value( key, tostring( value ) )
	end
end

function M.get_bool( obj, key, default )
	default = default == true
	if not obj or not key then
		return default
	end
	local raw = obj:get_value( key )
	if raw == "" then
		return default
	end
	return raw == "yes" or raw == "true" or raw == "1"
end

function M.set_bool( obj, key, value )
	if obj and key then
		obj:set_value( key, value and "yes" or "no" )
	end
end

function M.is_enabled( player )
	return M.get_bool( player, "bntalk_enabled", true )
end

function M.is_debug( player )
	return M.get_bool( player, "bntalk_debug", true )
end

function M.current_turn_text()
	local ok, turn_obj = pcall( function()
		return game.current_turn()
	end )
	if ok and turn_obj and turn_obj.get_turn then
		local ok_turn, turn = pcall( function()
			return turn_obj:get_turn()
		end )
		if ok_turn and turn then
			return tostring( turn )
		end
	end
	return "0"
end

function M.safe_bool_call( obj, method_name, ... )
	if not obj or not obj[method_name] then
		return false
	end
	local args = { ... }
	local ok, result = pcall( function()
		return obj[method_name]( obj, table.unpack( args ) )
	end )
	if ok then
		return result and true or false
	end
	return false
end

function M.safe_name( obj )
	if not obj then
		return "unknown"
	end
	local ok, name = pcall( function()
		return obj:disp_name( false, true )
	end )
	if ok and name then
		return tostring( name )
	end
	return "unknown"
end

function M.safe_add_item_with_id( player, item_id )
	if not player or not item_id then
		return false
	end

	if player.has_item_with_id then
		local ok_has, has_item = pcall( function()
			return player:has_item_with_id( ItypeId.new( item_id ), false )
		end )
		if ok_has and has_item then
			return true
		end
	end

	if player.add_item_with_id then
		local ok_add = pcall( function()
			player:add_item_with_id( ItypeId.new( item_id ), 1 )
		end )
		return ok_add
	end

	return false
end

function M.ensure_debug_item( player, item_id )
	return M.safe_add_item_with_id( player, item_id )
end

function M.context_summary( ctx )
	return string.format(
		"npc=%s affinity=%d following=%s friend=%s enemy=%s times_spoken=%d last_topic=%s phase=%s",
		tostring( ctx.npc_name ),
		tonumber( ctx.affinity ) or 0,
		tostring( ctx.following ),
		tostring( ctx.friend ),
		tostring( ctx.enemy ),
		tonumber( ctx.times_spoken ) or 0,
		tostring( ctx.last_topic or "" ),
		tostring( ctx.phase or "" )
	)
end

_G.BNTALK_UTIL = M
return M