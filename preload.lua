---@diagnostic disable: undefined-global

local game = game
local gapi = gapi
local gdebug = gdebug
local TimeDuration = TimeDuration

gdebug.log_info( "BN Talk: preload" )

---@class ModBnTalk
local mod = game.mod_runtime[game.current_mod]

require( "./lua/util" )
require( "./lua/memory" )
require( "./lua/context" )
require( "./lua/router" )
require( "./lua/provider_config" )
require( "./lua/provider_local" )
require( "./lua/provider_bridge" )
require( "./main" )

table.insert( game.hooks.on_game_started, function( ... )
	return mod.on_game_started( ... )
end )

table.insert( game.hooks.on_game_load, function( ... )
	return mod.on_game_load( ... )
end )

table.insert( game.hooks.on_npc_interaction, function( params )
	return mod.on_npc_interaction( params )
end )

table.insert( game.hooks.on_dialogue_start, function( params )
	return mod.on_dialogue_start( params )
end )

table.insert( game.hooks.on_dialogue_option, function( params )
	return mod.on_dialogue_option( params )
end )

table.insert( game.hooks.on_dialogue_end, function( params )
	return mod.on_dialogue_end( params )
end )

gapi.add_on_every_x_hook( TimeDuration.from_turns( 1 ), function( ... )
	return mod.on_every_second( ... )
end )

gapi.add_on_every_x_hook( TimeDuration.from_minutes( 30 ), function( ... )
	return mod.on_every_30_minutes( ... )
end )

game.iuse_functions["BNTALK_DEBUG_MENU"] = {
	use = function( params )
		return mod.open_debug_menu( params.user, params.item, params.pos )
	end,
}

gdebug.log_info( "BN Talk: preload complete" )