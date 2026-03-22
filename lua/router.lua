---@diagnostic disable: undefined-global

local M = {}

function M.select( ctx )
	local topic_id = "TALK_BNTALK_NEUTRAL"
	local reason = "default_neutral"

	if ctx.enemy then
		topic_id = "TALK_BNTALK_ENEMY"
		reason = "enemy"
	elseif ctx.following then
		topic_id = "TALK_BNTALK_FOLLOWER"
		reason = "following"
	elseif ( ctx.affinity or 0 ) >= 3 then
		topic_id = "TALK_BNTALK_FRIENDLY"
		reason = "affinity"
	elseif ( ctx.times_spoken or 0 ) <= 1 then
		topic_id = "TALK_BNTALK_FIRST_MEET"
		reason = "first_meet"
	end

	return {
		topic_id = topic_id,
		reason = reason,
		tags = {
			phase = ctx.phase,
			affinity = ctx.affinity,
			times_spoken = ctx.times_spoken,
		},
	}
end

_G.BNTALK_ROUTER = M
return M