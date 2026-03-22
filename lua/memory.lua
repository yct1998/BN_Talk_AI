---@diagnostic disable: undefined-global

local util = _G.BNTALK_UTIL or require( "./lua/util" )

local M = {}

function M.ensure_defaults( npc )
	if not npc then
		return
	end
	util.ensure_value( npc, "bntalk_affinity", "0" )
	util.ensure_value( npc, "bntalk_last_topic", "" )
	util.ensure_value( npc, "bntalk_last_event", "none" )
	util.ensure_value( npc, "bntalk_last_seen_turn", "0" )
	util.ensure_value( npc, "bntalk_times_spoken", "0" )
end

function M.note_interaction( npc, event_name )
	if not npc then
		return
	end
	M.ensure_defaults( npc )

	if event_name == "dialogue_start" then
		local spoken = util.get_number( npc, "bntalk_times_spoken", 0 ) + 1
		util.set_number( npc, "bntalk_times_spoken", spoken )
	end

	npc:set_value( "bntalk_last_event", tostring( event_name or "talk" ) )
	npc:set_value( "bntalk_last_seen_turn", util.current_turn_text() )
end

function M.add_affinity( npc, delta )
	if not npc then
		return
	end
	M.ensure_defaults( npc )
	local current = util.get_number( npc, "bntalk_affinity", 0 )
	local next_value = math.max( -100, math.min( 100, current + ( delta or 0 ) ) )
	util.set_number( npc, "bntalk_affinity", next_value )
end

function M.set_last_topic( npc, topic_id )
	if npc then
		npc:set_value( "bntalk_last_topic", tostring( topic_id or "" ) )
	end
end

function M.snapshot( npc )
	M.ensure_defaults( npc )
	return {
		affinity = util.get_number( npc, "bntalk_affinity", 0 ),
		last_topic = npc:get_value( "bntalk_last_topic" ),
		last_event = npc:get_value( "bntalk_last_event" ),
		last_seen_turn = npc:get_value( "bntalk_last_seen_turn" ),
		times_spoken = util.get_number( npc, "bntalk_times_spoken", 0 ),
	}
end

_G.BNTALK_MEMORY = M
return M