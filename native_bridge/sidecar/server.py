#!/usr/bin/env python3
import argparse
import copy
import json
import sys
import threading
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 45123
DEFAULT_CONFIG_PATH = Path(__file__).resolve().parents[2] / "config" / "provider.openai.runtime"
DEFAULT_LOG_PATH = Path(__file__).resolve().parents[1] / "build" / "ai_dialogue.log"
DEFAULT_PROMPT_PATH = Path(__file__).resolve().parents[2] / "config" / "ai_prompts.txt"

_JOB_LOCK = threading.Lock()
_JOBS: Dict[str, Dict[str, Any]] = {}


def _coerce_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"1", "true", "yes", "on"}:
            return True
        if lowered in {"0", "false", "no", "off"}:
            return False
    return default


def _coerce_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _load_json_file(path: Optional[str]) -> Dict[str, Any]:
    if not path:
        return {}
    raw = Path(path).read_text(encoding="utf-8")
    data = json.loads(raw)
    return data if isinstance(data, dict) else {}


def _load_prompt_sections(path: Path) -> Dict[str, str]:
    try:
        if not path.exists() or not path.is_file():
            return {}
        raw = path.read_text(encoding="utf-8")
    except Exception:
        return {}

    sections: Dict[str, str] = {}
    current_name: Optional[str] = None
    current_lines = []

    def flush() -> None:
        nonlocal current_name, current_lines
        if current_name is None:
            return
        text = "\n".join(current_lines).strip()
        if text:
            sections[current_name] = text

    for line in raw.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]") and len(stripped) > 2:
            flush()
            current_name = stripped[1:-1].strip()
            current_lines = []
            continue
        if current_name is None:
            continue
        if stripped.startswith("#"):
            continue
        current_lines.append(line.rstrip())

    flush()
    return sections


def _resolve_prompt_text(section_name: str, default_text: str) -> str:
    sections = _load_prompt_sections(DEFAULT_PROMPT_PATH)
    text = sections.get(section_name, "")
    return text if text else default_text


def _resolve_ai_outcome(context: Dict[str, Any], bridge: Dict[str, Any]) -> str:
    intent = str(bridge.get("interaction_intent") or context.get("planned_intent") or "talk")
    tone = str(context.get("planned_tone") or "plain")
    hostile_tone = tone in {"abuse", "threat", "abusive_threat"}

    if intent == "beg":
        forgive_chance = _coerce_int(context.get("planned_forgive_chance"), 0)
        mercy_chance = _coerce_int(context.get("planned_mercy_chance"), 0)
        forgive_roll = _coerce_int(context.get("planned_forgive_roll"), 101)
        mercy_roll = _coerce_int(context.get("planned_mercy_roll"), 101)
        if forgive_chance <= 0 and mercy_chance <= 0:
            return "invalid"
        if hostile_tone:
            return "fail"
        if forgive_roll <= forgive_chance:
            return "forgive"
        if mercy_roll <= mercy_chance:
            return "mercy"
        return "fail"

    if intent == "recruit":
        recruit_chance = _coerce_int(context.get("planned_recruit_chance"), 0)
        recruit_roll = _coerce_int(context.get("planned_recruit_roll"), 101)
        if recruit_chance <= 0:
            return "invalid"
        if hostile_tone:
            return "fail"
        if recruit_roll <= recruit_chance:
            return "success"
        return "fail"

    return ""


def _extract_context(request_payload: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    context_keys = {
        "phase",
        "npc_name",
        "player_name",
        "last_topic",
        "last_event",
        "last_seen_turn",
        "following",
        "friend",
        "enemy",
        "affinity",
        "times_spoken",
        "planned_intent",
        "planned_outcome",
        "planned_summary",
        "planned_debug",
        "planned_tone",
        "planned_forgive_chance",
        "planned_mercy_chance",
        "planned_recruit_chance",
        "planned_forgive_roll",
        "planned_mercy_roll",
        "planned_recruit_roll",
    }
    bridge_keys = {
        "protocol",
        "allow_dynamic_text",
        "preferred_dynamic_topic",
        "sidecar_enabled",
        "sidecar_url",
        "sidecar_timeout_ms",
        "request_kind",
        "utterance",
        "request_id",
        "interaction_intent",
        "social_skill",
        "is_enemy",
        "is_following",
    }

    context: Dict[str, Any] = {}
    bridge: Dict[str, Any] = {}

    extra_body = request_payload.get("extra_body")
    if isinstance(extra_body, dict):
        nested_context = extra_body.get("context")
        nested_bridge = extra_body.get("bridge")
        if isinstance(nested_context, dict):
            context.update(nested_context)
        if isinstance(nested_bridge, dict):
            bridge.update(nested_bridge)

    for key in context_keys:
        if key not in context and key in request_payload:
            context[key] = request_payload.get(key)
    for key in bridge_keys:
        if key not in bridge and key in request_payload:
            bridge[key] = request_payload.get(key)

    return context, bridge


def _request_kind(request_payload: Dict[str, Any], bridge: Dict[str, Any]) -> str:
    raw = request_payload.get("request_kind", bridge.get("request_kind", "route"))
    return str(raw or "route")


def _request_id(request_payload: Dict[str, Any], bridge: Dict[str, Any]) -> str:
    raw = request_payload.get("request_id", bridge.get("request_id", ""))
    return str(raw or "")


def _utterance_text(request_payload: Dict[str, Any], bridge: Dict[str, Any]) -> str:
    raw = request_payload.get("utterance", bridge.get("utterance", ""))
    return str(raw or "")


def _build_debug(context: Dict[str, Any], request_payload: Dict[str, Any], bridge: Dict[str, Any]) -> str:
    return ";".join(
        [
            f"phase={context.get('phase', 'dialogue_start')}",
            f"affinity={_coerce_int(context.get('affinity'), 0)}",
            f"spoken={_coerce_int(context.get('times_spoken'), 0)}",
            f"friend={str(_coerce_bool(context.get('friend'))).lower()}",
            f"following={str(_coerce_bool(context.get('following'))).lower()}",
            f"enemy={str(_coerce_bool(context.get('enemy'))).lower()}",
            f"planned_intent={context.get('planned_intent', '')}",
            f"planned_outcome={context.get('planned_outcome', '')}",
            f"last_topic={context.get('last_topic', '')}",
            f"last_event={context.get('last_event', '')}",
            f"last_seen_turn={context.get('last_seen_turn', '')}",
            f"request_kind={_request_kind(request_payload, bridge)}",
        ]
    )


def _append_generation_log(request_payload: Dict[str, Any], decision: Dict[str, Any], service_config: Dict[str, Any]) -> None:
    try:
        context, bridge = _extract_context(request_payload)
        log_path = Path(str(service_config.get("log_path") or DEFAULT_LOG_PATH))
        log_path.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "provider": decision.get("provider", ""),
            "topic_id": decision.get("topic_id", ""),
            "mode": decision.get("mode", ""),
            "reason": decision.get("reason", ""),
            "generated_text": decision.get("generated_text", ""),
            "emotion_delta": decision.get("emotion_delta", 0),
            "debug": decision.get("debug", ""),
            "phase": context.get("phase", ""),
            "npc_name": context.get("npc_name", ""),
            "player_name": context.get("player_name", ""),
            "affinity": context.get("affinity"),
            "times_spoken": context.get("times_spoken"),
            "following": context.get("following"),
            "friend": context.get("friend"),
            "enemy": context.get("enemy"),
            "planned_outcome": context.get("planned_outcome", ""),
            "planned_tone": context.get("planned_tone", ""),
            "bridge_protocol": bridge.get("protocol"),
            "preferred_dynamic_topic": bridge.get("preferred_dynamic_topic", ""),
            "request_kind": _request_kind(request_payload, bridge),
            "utterance": _utterance_text(request_payload, bridge),
            "request_id": _request_id(request_payload, bridge),
        }
        with log_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, ensure_ascii=False) + "\n")
    except Exception as exc:
        sys.stdout.write(f"[bntalk-sidecar] failed to append ai dialogue log: {exc}\n")
        sys.stdout.flush()


def _deterministic_route(request_payload: Dict[str, Any]) -> Dict[str, Any]:
    context, bridge = _extract_context(request_payload)

    phase = str(context.get("phase") or "dialogue_start")
    npc_name = str(context.get("npc_name") or "someone")
    player_name = str(context.get("player_name") or "you")
    last_topic = str(context.get("last_topic") or "")
    last_event = str(context.get("last_event") or "")
    affinity = _coerce_int(context.get("affinity"), 0)
    times_spoken = _coerce_int(context.get("times_spoken"), 0)
    following = _coerce_bool(context.get("following"), False)
    friend_flag = _coerce_bool(context.get("friend"), False)
    enemy = _coerce_bool(context.get("enemy"), False)
    allow_dynamic_text = _coerce_bool(bridge.get("allow_dynamic_text"), False)
    preferred_dynamic_topic = str(bridge.get("preferred_dynamic_topic") or "TALK_BNTALK_DYNAMIC")
    protocol = _coerce_int(bridge.get("protocol"), 0)
    request_kind = _request_kind(request_payload, bridge)
    utterance = _utterance_text(request_payload, bridge)

    topic_id = preferred_dynamic_topic
    mode = "dynamic_topic"
    reason = "default_dynamic"
    generated_text = ""
    emotion_delta = 0

    interaction_intent = str(bridge.get("interaction_intent") or "talk")
    social_skill = _coerce_int(bridge.get("social_skill"), 0)

    if request_kind == "utterance_reply":
        mode = "utterance_reply"
        reason = "utterance_reply"
        if interaction_intent == "beg":
            outcome = _resolve_ai_outcome(context, bridge)
            if outcome == "forgive":
                generated_text = "All right. This time I'll let you live. Drop your guard and get out of here."
                emotion_delta = 0
            elif outcome == "mercy":
                generated_text = "I'm not killing you right now. You have one minute to get out of my sight."
                emotion_delta = -1
            elif enemy:
                generated_text = f"{npc_name} stares at you as if weighing whether to spare you for a moment."
                emotion_delta = -1
            else:
                generated_text = f"{npc_name} looks confused, because you are not actually being hunted right now."
                emotion_delta = 0
            reason = outcome or reason
            return {
                "provider": "sidecar_deterministic",
                "topic_id": topic_id,
                "mode": mode,
                "reason": reason,
                "generated_text": generated_text,
                "emotion_delta": emotion_delta,
                "interaction_outcome": outcome,
                "debug": _build_debug(context, request_payload, bridge),
            }
        elif interaction_intent == "recruit":
            outcome = _resolve_ai_outcome(context, bridge)
            if outcome == "success":
                generated_text = "Fine. I'll go with you, but don't make me regret it."
                emotion_delta = 1
            elif enemy:
                generated_text = f"{npc_name} coldly rejects your invitation and clearly has no intention of going with you."
                emotion_delta = -2
            elif friend_flag or affinity >= 4 or social_skill >= 4:
                generated_text = f"{npc_name} listens to your invitation seriously, but still does not agree."
                emotion_delta = 0
            else:
                generated_text = f"{npc_name} hears your invitation, but remains cautious and reserved."
                emotion_delta = 0
            reason = outcome or reason
            return {
                "provider": "sidecar_deterministic",
                "topic_id": topic_id,
                "mode": mode,
                "reason": reason,
                "generated_text": generated_text,
                "emotion_delta": emotion_delta,
                "interaction_outcome": outcome,
                "debug": _build_debug(context, request_payload, bridge),
            }
        elif enemy or affinity <= -3:
            generated_text = f"{npc_name} frowns after hearing you and sounds noticeably less friendly."
            emotion_delta = -2
        elif friend_flag or affinity >= 4:
            generated_text = f"{npc_name} replies with a tone that already carries some trust."
            emotion_delta = 1
        else:
            generated_text = f"{npc_name} hears what you said and gives a cautious but otherwise normal reply."
            emotion_delta = 0
    elif phase == "self_test":
        reason = "self_test"
        generated_text = f"Sidecar self-test succeeded for {npc_name} using protocol {protocol}."
    elif enemy:
        topic_id = "TALK_BNTALK_ENEMY"
        mode = "topic_route"
        reason = "enemy"
        generated_text = f"{npc_name} replies with immediate hostility and treats {player_name} as a threat."
    elif following:
        topic_id = "TALK_BNTALK_FOLLOWER"
        mode = "topic_route"
        reason = "following"
        generated_text = f"{npc_name} is already following your lead and waits for the next concrete order."
    elif times_spoken <= 1:
        topic_id = "TALK_BNTALK_FIRST_MEET"
        mode = "topic_route"
        reason = "first_meet"
        generated_text = f"{npc_name} sounds cautious, like this first real exchange still needs to earn trust."
    elif affinity <= -3:
        topic_id = "TALK_BNTALK_NEUTRAL"
        mode = "topic_route"
        reason = "low_affinity"
        generated_text = f"{npc_name} keeps the answer short and guarded, clearly not ready to open up."
    elif friend_flag or affinity >= 6:
        reason = "friend_dynamic" if friend_flag else "high_affinity_dynamic"
        generated_text = f"{npc_name} speaks to {player_name} with the ease of someone who has learned to rely on you."
    elif last_topic == "TALK_BNTALK_SMALLTALK":
        reason = "smalltalk_followup"
        generated_text = f"{npc_name} smoothly continues the earlier small talk instead of resetting the mood."
    elif last_event == "dialogue_end":
        reason = "resume_after_dialogue"
        generated_text = f"{npc_name} resumes the conversation as if the last exchange is still fresh."
    else:
        generated_text = f"{npc_name} answers in a grounded way shaped by the current BN Talk sidecar router."

    if not allow_dynamic_text and mode == "dynamic_topic":
        generated_text = ""

    return {
        "provider": "sidecar_deterministic",
        "topic_id": topic_id,
        "mode": mode,
        "reason": reason,
        "generated_text": generated_text,
        "emotion_delta": emotion_delta,
        "debug": _build_debug(context, request_payload, bridge),
    }


def _normalize_model_route(raw: Any, deterministic: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    if isinstance(raw, str):
        raw = raw.strip()
        if not raw:
            return None
        raw = json.loads(raw)
    if not isinstance(raw, dict):
        return None

    topic_id = str(raw.get("topic_id") or deterministic["topic_id"])
    mode = str(raw.get("mode") or deterministic.get("mode") or "dynamic_topic")
    reason = str(raw.get("reason") or "model_generated")
    generated_text = str(raw.get("generated_text") or "")
    debug = str(raw.get("debug") or deterministic.get("debug") or "")
    provider = str(raw.get("provider") or "sidecar_model")
    interaction_outcome = str(raw.get("interaction_outcome") or "")
    if not interaction_outcome and reason in {"success", "fail", "forgive", "mercy", "invalid"}:
        interaction_outcome = reason
    emotion_delta = _coerce_int(raw.get("emotion_delta"), _coerce_int(deterministic.get("emotion_delta"), 0))
    ready = _coerce_bool(raw.get("ready"), True)
    request_id = str(raw.get("request_id") or "")

    return {
        "provider": provider,
        "topic_id": topic_id,
        "mode": mode,
        "reason": reason,
        "generated_text": generated_text,
        "emotion_delta": emotion_delta,
        "interaction_outcome": interaction_outcome,
        "debug": debug,
        "ready": ready,
        "request_id": request_id,
    }


def _call_openai_compatible(request_payload: Dict[str, Any], config: Dict[str, Any]) -> Dict[str, Any]:
    deterministic = _deterministic_route(request_payload)
    context, bridge = _extract_context(request_payload)
    request_kind = _request_kind(request_payload, bridge)

    base_url = str(config.get("base_url") or "").rstrip("/")
    model = str(config.get("model") or request_payload.get("model") or "")
    api_key = str(config.get("api_key") or "")
    temperature = config.get("temperature", 0.2)
    max_tokens = config.get("max_tokens", 120)

    if not base_url or not model:
        deterministic["provider"] = "sidecar_deterministic"
        deterministic["reason"] = "model_config_missing"
        return deterministic

    base_system_default = (
        "You are the BN Talk sidecar for Cataclysm BN. "
        "Return a JSON object with topic_id, mode, reason, generated_text, emotion_delta, and optional debug. "
        "emotion_delta must be an integer from -3 to 3."
    )
    utterance_reply_default = (
        "You are generating an NPC reply to the player's free-form utterance for Cataclysm BN. "
        "Return a JSON object with topic_id, mode, reason, generated_text, emotion_delta, and optional debug. "
        "Keep generated_text concise, suitable for the message log, and set emotion_delta to an integer from -3 to 3. "
        "Do NOT include the NPC's name at the start of generated_text. Do NOT prefix with speaker labels like 'Name:' or 'NPC:'. "
        "Write only the spoken content itself. "
        "If the player's utterance is a severe insult, threat, or repeated abuse, set emotion_delta to -3 and make the tone clearly hostile. "
        "If bridge.interaction_intent is beg or recruit, you must return interaction_outcome. "
        "Use the provided planned roll values and planned base chances as baseline dice inputs, then judge whether utterance tone, wording, and context should shift the effective chance up or down before choosing interaction_outcome. "
        "For beg, interaction_outcome must be one of forgive, mercy, fail, invalid. "
        "For recruit, interaction_outcome must be one of success, fail, invalid. "
        "Your generated_text must naturally match the interaction_outcome you chose. "
        "If the utterance is insulting, coercive, or threatening, you should usually downgrade the outcome rather than accept it at face value."
    )
    system_content = _resolve_prompt_text("base_system", base_system_default)
    if request_kind == "utterance_reply":
        system_content = _resolve_prompt_text("utterance_reply_system", utterance_reply_default)

    body = {
        "model": model,
        "messages": [
            {
                "role": "system",
                "content": system_content,
            },
            {
                "role": "user",
                "content": json.dumps(
                    {
                        "context": context,
                        "bridge": bridge,
                        "request_kind": request_kind,
                        "deterministic_fallback": deterministic,
                    },
                    ensure_ascii=False,
                ),
            },
        ],
        "temperature": temperature,
        "max_tokens": max_tokens,
        "response_format": {"type": "json_object"},
    }

    debug_parts = [deterministic.get("debug", "")]
    debug_parts.append(f"base_url={base_url}")
    debug_parts.append(f"model={model}")

    headers = {
        "Content-Type": "application/json",
        "User-Agent": "BN-Talk-Sidecar/0.1",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        f"{base_url}/chat/completions",
        data=data,
        method="POST",
        headers=headers,
    )

    try:
        with urllib.request.urlopen(req, timeout=float(config.get("timeout", 45))) as resp:
            raw_text = resp.read().decode("utf-8", errors="replace")
            debug_parts.append(f"http_status={getattr(resp, 'status', 'unknown')}")
            debug_parts.append(f"raw_response={raw_text[:300]}")
            payload = json.loads(raw_text)
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode("utf-8", errors="replace")
        deterministic["provider"] = "sidecar_deterministic"
        deterministic["reason"] = f"model_http_{exc.code}"
        debug_parts.append(f"http_error_body={body_text[:300]}")
        deterministic["debug"] = ";".join(part for part in debug_parts if part)
        return deterministic
    except urllib.error.URLError as exc:
        deterministic["provider"] = "sidecar_deterministic"
        deterministic["reason"] = "model_request_failed"
        debug_parts.append(f"url_error={exc}")
        deterministic["debug"] = ";".join(part for part in debug_parts if part)
        return deterministic
    except Exception as exc:
        deterministic["provider"] = "sidecar_deterministic"
        deterministic["reason"] = "model_exception"
        debug_parts.append(f"exception={type(exc).__name__}:{exc}")
        deterministic["debug"] = ";".join(part for part in debug_parts if part)
        return deterministic

    try:
        content = payload["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        deterministic["provider"] = "sidecar_deterministic"
        deterministic["reason"] = "model_payload_missing_content"
        debug_parts.append(f"payload_error={type(exc).__name__}:{exc}")
        deterministic["debug"] = ";".join(part for part in debug_parts if part)
        return deterministic

    normalized = _normalize_model_route(content, deterministic)
    if not normalized:
        deterministic["provider"] = "sidecar_deterministic"
        deterministic["reason"] = "model_payload_invalid"
        debug_parts.append(f"content={str(content)[:300]}")
        deterministic["debug"] = ";".join(part for part in debug_parts if part)
        return deterministic

    allow_dynamic_text = _coerce_bool(bridge.get("allow_dynamic_text"), False)
    if not allow_dynamic_text and normalized.get("mode") == "dynamic_topic":
        normalized["generated_text"] = ""
    if not normalized.get("provider"):
        normalized["provider"] = "sidecar_model"
    normalized["debug"] = ";".join(part for part in ([normalized.get("debug", "")] + debug_parts) if part)
    return normalized


def _make_pending_response(request_id: str, reason: str = "queued") -> Dict[str, Any]:
    return {
        "provider": "sidecar_queue",
        "topic_id": "",
        "mode": "background_pending",
        "reason": reason,
        "generated_text": "",
        "emotion_delta": 0,
        "debug": f"request_id={request_id}",
        "request_id": request_id,
        "ready": False,
    }


def _run_background_job(request_id: str, request_payload: Dict[str, Any], model_config: Dict[str, Any], service_config: Dict[str, Any]) -> None:
    work_payload = copy.deepcopy(request_payload)
    work_payload["request_kind"] = "utterance_reply"
    extra_body = work_payload.get("extra_body")
    if isinstance(extra_body, dict):
        bridge = extra_body.get("bridge")
        if isinstance(bridge, dict):
            bridge["request_kind"] = "utterance_reply"
            bridge["request_id"] = request_id

    if model_config.get("base_url") and model_config.get("model"):
        decision = _call_openai_compatible(work_payload, model_config)
    else:
        decision = _deterministic_route(work_payload)

    decision["request_id"] = request_id
    decision["ready"] = True
    _append_generation_log(work_payload, decision, service_config)

    with _JOB_LOCK:
        _JOBS[request_id] = {
            "status": "done",
            "result": decision,
        }


def _enqueue_background_job(request_payload: Dict[str, Any], model_config: Dict[str, Any], service_config: Dict[str, Any]) -> Dict[str, Any]:
    request_id = uuid.uuid4().hex
    with _JOB_LOCK:
        _JOBS[request_id] = {
            "status": "pending",
            "created_at": datetime.now(timezone.utc).isoformat(),
        }

    thread = threading.Thread(
        target=_run_background_job,
        args=(request_id, request_payload, model_config, service_config),
        daemon=True,
    )
    thread.start()
    return _make_pending_response(request_id)


def _poll_background_job(request_payload: Dict[str, Any]) -> Dict[str, Any]:
    _, bridge = _extract_context(request_payload)
    request_id = _request_id(request_payload, bridge)
    if not request_id:
        return {
            "provider": "sidecar_queue",
            "topic_id": "",
            "mode": "background_missing",
            "reason": "request_id_missing",
            "generated_text": "",
            "emotion_delta": 0,
            "debug": "poll_missing_request_id",
            "request_id": "",
            "ready": False,
        }

    with _JOB_LOCK:
        entry = _JOBS.get(request_id)
        if not entry:
            return {
                "provider": "sidecar_queue",
                "topic_id": "",
                "mode": "background_missing",
                "reason": "request_not_found",
                "generated_text": "",
                "emotion_delta": 0,
                "debug": f"request_id={request_id}",
                "request_id": request_id,
                "ready": False,
            }
        if entry.get("status") != "done":
            return _make_pending_response(request_id, "pending")
        result = dict(entry.get("result") or {})
        _JOBS.pop(request_id, None)
        return result


class BNTalkSidecarHandler(BaseHTTPRequestHandler):
    service_config: Dict[str, Any] = {}

    def _write_json(self, status: int, payload: Dict[str, Any]) -> None:
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        except ConnectionResetError:
            sys.stdout.write("[bntalk-sidecar] client disconnected before response flush\n")
            sys.stdout.flush()
        except BrokenPipeError:
            sys.stdout.write("[bntalk-sidecar] broken pipe while writing response\n")
            sys.stdout.flush()

    def do_GET(self) -> None:
        if self.path == "/health":
            config = self.service_config
            self._write_json(
                200,
                {
                    "ok": True,
                    "service": "bntalk_sidecar",
                    "model_enabled": bool(config.get("model")),
                    "host": config.get("host", DEFAULT_HOST),
                    "port": config.get("port", DEFAULT_PORT),
                },
            )
            return
        self._write_json(404, {"ok": False, "error": "not_found"})

    def do_POST(self) -> None:
        if self.path != "/route":
            self._write_json(404, {"ok": False, "error": "not_found"})
            return

        content_length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(content_length)
        try:
            request_payload = json.loads(raw.decode("utf-8", errors="replace"))
        except json.JSONDecodeError:
            self._write_json(400, {"ok": False, "error": "invalid_json"})
            return

        if not isinstance(request_payload, dict):
            self._write_json(400, {"ok": False, "error": "request_must_be_object"})
            return

        config = self.service_config
        model_config = config.get("model") or {}
        use_model = bool(model_config.get("base_url") and model_config.get("model"))
        _, bridge = _extract_context(request_payload)
        request_kind = _request_kind(request_payload, bridge)

        if request_kind == "enqueue_utterance":
            decision = _enqueue_background_job(request_payload, model_config if use_model else {}, config)
        elif request_kind == "poll_utterance":
            decision = _poll_background_job(request_payload)
        elif use_model:
            decision = _call_openai_compatible(request_payload, model_config)
            _append_generation_log(request_payload, decision, config)
        else:
            decision = _deterministic_route(request_payload)
            _append_generation_log(request_payload, decision, config)

        self._write_json(200, decision)

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stdout.write("[bntalk-sidecar] " + (fmt % args) + "\n")
        sys.stdout.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description="BN Talk localhost sidecar service")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument(
        "--config",
        default=str(DEFAULT_CONFIG_PATH),
        help="OpenAI-compatible JSON config path. Defaults to bntalk/config/provider.openai.runtime",
    )
    args = parser.parse_args()

    config_path = Path(args.config)
    config = _load_json_file(str(config_path)) if config_path.exists() else {}
    service_config = {
        "host": args.host,
        "port": args.port,
        "model": config,
        "config_path": str(config_path),
        "log_path": str(DEFAULT_LOG_PATH),
    }
    BNTalkSidecarHandler.service_config = service_config

    server = ThreadingHTTPServer((args.host, args.port), BNTalkSidecarHandler)
    print(f"BN Talk sidecar listening on http://{args.host}:{args.port}")
    print(f"BN Talk sidecar config path: {config_path}")
    print(f"BN Talk sidecar dialogue log: {DEFAULT_LOG_PATH}")
    if config:
        print("BN Talk sidecar model routing is enabled")
    else:
        print("BN Talk sidecar model routing is disabled; deterministic routing only")
        if not config_path.exists():
            print("BN Talk sidecar config file not found; create it to enable API-backed generation")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
