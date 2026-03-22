---@diagnostic disable: undefined-global

local T = {
	insult = {
		base = 35,
		emotion_weight = 10,
		speech_weight = 1,
		positive_affinity_divisor = 6,
		extreme_bonus = 10,
		minimum = 20,
		maximum = 85,
	},
	beg = {
		forgive_base = 10,
		forgive_speech_weight = 2,
		forgive_affinity_weight = 1,
		forgive_minimum = 5,
		forgive_maximum = 70,
		mercy_base = 30,
		mercy_speech_weight = 3,
		mercy_affinity_weight = 1,
		mercy_minimum = 15,
		mercy_maximum = 95,
	},
	recruit = {
		base = 10,
		speech_weight = 3,
		affinity_weight = 2,
		minimum = 5,
		maximum = 75,
	},
}

_G.BNTALK_TUNING = T
return T
