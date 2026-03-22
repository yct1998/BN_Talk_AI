# BN Talk for CBN

[中文说明 / 中文版文档](README.zh-CN)

BN Talk is an experimental AI-driven NPC interaction mod for **Cataclysm: Bright Nights (CBN)**.

Instead of relying only on static vanilla dialogue, it combines **Lua + an injected native bridge DLL + a localhost sidecar service + an OpenAI-compatible API** to support free-form NPC interaction in a RimTalk-like style.

Current interaction types:
- Talk
- Beg hostile NPC
- Recruit / invite NPC to join
- Insult / hostility escalation based on player input and AI interpretation

---

## Features

- Free-form player input
- Asynchronous NPC replies
- Runtime AI-generated lines in the message log
- NPC memory and affinity tracking
- Beg / mercy / recruit gameplay resolution
- Prompt configuration moved out of code into a text file
- Tunable gameplay odds in Lua
- Native injector can auto-start the sidecar

---

## Project structure

Main files and directories:

- [`main.lua`](main.lua)
  - Main mod logic
  - BN Talk Console
  - Async request flow
  - Beg / recruit / insult gameplay resolution

- [`preload.lua`](preload.lua)
  - Hook registration entry

- [`lua/context.lua`](lua/context.lua)
  - AI-readable context builder

- [`lua/memory.lua`](lua/memory.lua)
  - Persistent NPC memory, affinity, interaction counters

- [`lua/provider_bridge.lua`](lua/provider_bridge.lua)
  - Lua → native bridge request packing

- [`lua/tuning.lua`](lua/tuning.lua)
  - Tunable insult / beg / recruit parameters

- [`config/provider.openai.runtime`](config/provider.openai.runtime)
  - Runtime API configuration

- [`config/ai_prompts.txt`](config/ai_prompts.txt)
  - Editable AI prompt definitions

- [`native_bridge/bridge_dll/lua_runtime_bridge.cpp`](native_bridge/bridge_dll/lua_runtime_bridge.cpp)
  - Registers native Lua functions after injection

- [`native_bridge/common/bntalk_protocol.cpp`](native_bridge/common/bntalk_protocol.cpp)
  - Sidecar transport + fallback routing

- [`native_bridge/sidecar/server.py`](native_bridge/sidecar/server.py)
  - Local sidecar service
  - Talks to an OpenAI-compatible API
  - Parses structured AI output

- [`native_bridge/injector/main.cpp`](native_bridge/injector/main.cpp)
  - DLL injector
  - Auto-starts the sidecar

- [`native_bridge/build/rebuild_bridge.cmd`](native_bridge/build/rebuild_bridge.cmd)
  - Windows rebuild script for bridge artifacts

---

## How it works

1. The player opens BN Talk Console and enters a free-form line.
2. Lua builds a request from NPC + player state.
3. The injected bridge exposes native functions to Lua at runtime.
4. Lua sends the request to the local sidecar.
5. The sidecar calls an OpenAI-compatible model (or a deterministic fallback).
6. The model returns structured data such as:
   - `generated_text`
   - `emotion_delta`
   - `interaction_outcome`
7. Lua applies the real game result:
   - prints the final line to the message log
   - adjusts affinity
   - changes hostile / friendly / follow state
   - applies beg / mercy / recruit consequences

---

## Requirements

Current target environment:
- Windows 10 / 11
- Windows build of CBN
- Python available from command line (`python`)
- Visual Studio C++ toolchain if you want to rebuild the bridge yourself

Notes:
- The injector currently looks for `cataclysm-bn-tiles.exe`
- If your executable name differs, adjust injector usage or source code

---

## Installation

### 1. Place the mod
Copy the whole [`bntalk`](bntalk) folder into your CBN mod directory.

Example:
- `.../userdata/mods/bntalk`

### 2. Enable the mod
Enable `BN Talk` when creating or editing a world.

---

## API configuration

Edit:
- [`config/provider.openai.runtime`](config/provider.openai.runtime)

Example format:

```json
{
  "base_url": "https://api.siliconflow.cn/v1",
  "api_key": "your-api-key",
  "model": "zai-org/GLM-4.6",
  "temperature": 0.2,
  "max_tokens": 1200,
  "timeout": 20
}
```

Fields:
- `base_url` — OpenAI-compatible endpoint root
- `api_key` — API key
- `model` — model name
- `temperature` — sampling temperature
- `max_tokens` — generation limit
- `timeout` — request timeout in seconds


---

## AI prompt configuration

Edit:
- [`config/ai_prompts.txt`](config/ai_prompts.txt)

Currently supported sections:
- `[base_system]`
- `[utterance_reply_system]`

You can modify prompts here without changing Python code.

Notes:
- Keep section names unchanged
- If a section is missing, the sidecar falls back to built-in default text

---

## Running the mod

### 1. Start CBN
Launch the game normally.

### 2. Run the injector
Run:
- [`native_bridge/build/bntalk_injector.exe`](native_bridge/build/bntalk_injector.exe)

The injector will:
1. auto-start the local sidecar ([`sidecar/server.py`](native_bridge/sidecar/server.py))
2. inject [`bntalk_bridge_lua.dll`](native_bridge/build/bntalk_bridge_lua.dll) into the CBN process

If injection succeeds, Lua can call:
- `bntalk_native_status`
- `bntalk_native_route`
- `bntalk_native_prompt_text`

---

## In-game usage

Open BN Talk Console in-game.

Current menu actions:
- `Talk to nearby NPC (async)`
- `Beg hostile NPC (async)`
- `Invite nearby NPC to join (async)`
- `Show bridge status`
- `Show pending async jobs`
- `Show last generated line`
- `Run native bridge self-test`
- `Show quick test hint`

### Talk
- Select `Talk to nearby NPC (async)`
- Pick a nearby NPC
- Enter any line
- The AI reply appears in the message log shortly after

### Beg
- Make an NPC hostile first
- Select `Beg hostile NPC (async)`
- Enter a plea
- The AI returns a line and the mod tries to apply the gameplay outcome:
  - continue attacking
  - temporary mercy
  - full forgiveness

### Recruit
- Select `Invite nearby NPC to join (async)`
- Enter an invitation line
- The AI evaluates the request using:
  - speech skill
  - affinity
  - utterance tone
  - pre-rolled values
  - context
- Lua then applies the returned `interaction_outcome`

---

## Tuning gameplay odds

Edit:
- [`lua/tuning.lua`](lua/tuning.lua)

Currently tunable:

### insult
- `base`
- `emotion_weight`
- `speech_weight`
- `positive_affinity_divisor`
- `extreme_bonus`
- `minimum`
- `maximum`

### beg
- `forgive_base`
- `forgive_speech_weight`
- `forgive_affinity_weight`
- `forgive_minimum`
- `forgive_maximum`
- `mercy_base`
- `mercy_speech_weight`
- `mercy_affinity_weight`
- `mercy_minimum`
- `mercy_maximum`

### recruit
- `base`
- `speech_weight`
- `affinity_weight`
- `minimum`
- `maximum`

These values are already wired into the main gameplay logic.

---

## Current decision model

For `beg` and `recruit`, the current architecture is:
- Lua provides:
  - baseline chances
  - pre-rolled dice values
  - utterance tone classification
  - runtime context
- The sidecar / AI returns:
  - `generated_text`
  - `emotion_delta`
  - `interaction_outcome`
- Lua applies the actual game result using `interaction_outcome`

The design goal is to let AI participate in the final decision while still keeping gameplay-side application inspectable.

---

## Debugging and logs

### In-game debug
BN Talk Console can show:
- bridge status
- pending jobs
- generated output
- quick probability hints
- extra debug lines

### Log files
Main logs:
- [`native_bridge/build/ai_dialogue.log`](native_bridge/build/ai_dialogue.log)
- [`native_bridge/build/bridge_runtime.log`](native_bridge/build/bridge_runtime.log)

Useful fields to inspect:
- `phase`
- `utterance`
- `planned_outcome`
- `planned_tone`
- `interaction_outcome`
- `generated_text`
- `emotion_delta`

---

## Rebuilding (developers)

If you modify:
- [`lua_runtime_bridge.cpp`](native_bridge/bridge_dll/lua_runtime_bridge.cpp)
- [`bntalk_protocol.cpp`](native_bridge/common/bntalk_protocol.cpp)
- [`injector/main.cpp`](native_bridge/injector/main.cpp)
- [`server.py`](native_bridge/sidecar/server.py)

Rebuild with:
- [`native_bridge/build/rebuild_bridge.cmd`](native_bridge/build/rebuild_bridge.cmd)

Outputs:
- [`native_bridge/build/bntalk_bridge_lua.dll`](native_bridge/build/bntalk_bridge_lua.dll)
- [`native_bridge/build/bntalk_injector.exe`](native_bridge/build/bntalk_injector.exe)

---

## Known limitations

Current limitations include:
1. Output still appears mainly in the message log, not the vanilla dialogue body
2. Some NPC state refresh depends on native CBN AI timing
3. There may still be edge cases where AI wording and gameplay resolution diverge and need further iteration
4. This is not a pure Lua mod; it depends on an injected native bridge


---

## Quick start

1. Enable [`bntalk`](modinfo.json)
2. Configure [`config/provider.openai.runtime`](config/provider.openai.runtime)
3. Optionally edit [`config/ai_prompts.txt`](config/ai_prompts.txt)
4. Start CBN
5. Run [`native_bridge/build/bntalk_injector.exe`](native_bridge/build/bntalk_injector.exe)
6. Open BN Talk Console in-game and interact with NPCs
