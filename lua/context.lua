---@diagnostic disable: undefined-global

local util = _G.BNTALK_UTIL or require( "./lua/util" )
local memory = _G.BNTALK_MEMORY or require( "./lua/memory" )

local M = {}

function M.build( npc, player, phase )
	local mem = memory.snapshot( npc )
	return {
		phase = phase or "dialogue_start",
		npc_name = util.safe_name( npc ),
		player_name = util.safe_name( player ),
		affinity = mem.affinity,
		last_topic = mem.last_topic,
		last_event = mem.last_event,
		times_spoken = mem.times_spoken,
		last_seen_turn = mem.last_seen_turn,
		following = util.safe_bool_call( npc, "is_following" ),
		friend = util.safe_bool_call( npc, "is_friend" ),
		enemy = util.safe_bool_call( npc, "is_enemy" ),
	}
end

_G.BNTALK_CONTEXT = M
return M