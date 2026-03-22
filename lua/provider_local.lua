---@diagnostic disable: undefined-global

local util = _G.BNTALK_UTIL or require( "./lua/util" )
local config = _G.BNTALK_PROVIDER_CONFIG or require( "./lua/provider_config" )
local router = _G.BNTALK_ROUTER or require( "./lua/router" )

local M = {}

function M.build_request( ctx )
	return {
		model = config.api.model,
		messages = {
			{
				role = "system",
				content = "You are bntalk-local-v1, a dialogue router that returns the best talk topic for an NPC.",
			},
			{
				role = "user",
				content = util.context_summary( ctx ),
			},
		},
		temperature = config.api.request.temperature,
		max_tokens = config.api.request.max_tokens,
		response_format = config.api.request.response_format,
		extra_body = {
			context = ctx,
		},
	}
end

function M.complete( request )
	local ctx = request and request.extra_body and request.extra_body.context or {}
	local route = router.select( ctx )

	return {
		id = "bntalk-local-0001",
		object = "chat.completion",
		created = 0,
		model = request.model or config.api.model,
		choices = {
			{
				index = 0,
				message = {
					role = "assistant",
					content = route.topic_id,
				},
				finish_reason = "stop",
			},
		},
		usage = {
			prompt_tokens = #( request.messages or {} ),
			completion_tokens = 1,
			total_tokens = #( request.messages or {} ) + 1,
		},
		bntalk = route,
	}
end

_G.BNTALK_PROVIDER_LOCAL = M
return M