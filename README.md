# voicebox

A computer that **listens, thinks, and talks**, fully local, written in C. ~1200 lines.

```
mic → Silero VAD → Whisper STT → llama.cpp / OpenRouter → Kokoro TTS → speaker
```

Designed as a per-character voice subprocess for games (the original use case is the
ship-AI in [cenetex/signal](https://github.com/cenetex/signal)). Speaks a tiny line
protocol on stdin so the host game stays free of ggml/onnxruntime.

## What's in the box

- **Local-only stack:** Whisper tiny.en (sherpa-onnx) + Qwen3-1.7B-Q4 (llama.cpp) + Kokoro v1.0 (sherpa-onnx)
- **Optional cloud LLM** via OpenRouter (`OPENROUTER_API_KEY`) — same protocol, lower local resource cost, broader model choice
- **Multi-persona** — register N personas, each with its own voice (Kokoro index), speed, fx, and system prompt
- **DSP coloration** — `intercom`, `vintage`, `robot` filter chain in `fx.c` (~50 LOC)
- **Streaming token → sentence → TTS** — sub-second first-audio with KV-cached system prompt
- **Async playback worker** — LLM keeps generating while audio plays
- **Barge-in** — high-energy mic input during TTS aborts the speaker queue
- **Wake word + sleep phrase** with 30 s follow-up window
- **Whisper hallucination guard** — drops `[BLANK_AUDIO]`, `(cough)`, `(laughs)`, transcripts under 2 letters

## Protocol (stdin)

```
STATE <free-form text>           replace ship telemetry blob
PERSONA <name> <file>            register a persona at runtime
EVENT [<persona>] <line>         speak verbatim via TTS (deterministic chatter)
ASK   [<persona>] <directive>    LLM paraphrases with state, in that persona's voice
QUIT
```

Pilot speech (mic) is always on. Captured audio → Whisper → LLM with current `STATE` injected → TTS in the default persona's voice.

## Quick start (native, all-local)

```bash
./setup.sh                              # builds deps, downloads models (~1.5 GB)
make
./scenarios/three_stations.sh \
  | ./voicebox --ship \
      --persona-add nav7     personas/nav7.persona \
      --persona-add prospect personas/prospect.persona \
      --persona-add kepler   personas/kepler.persona \
      --persona-add helios   personas/helios.persona \
      models/whisper models/kokoro models/llm.gguf
```

Plays four distinct voices reading station chatter and a couple of LLM-driven asks with current ship state injected.

## Quick start (browser demo, OpenRouter LLM via OAuth)

```bash
./web/serve.sh
# open http://localhost:8765 — click "Connect OpenRouter"
```

OAuth-PKCE flow, no key handling. Type to NAV-7. Audio (STT/TTS) coming in a follow-up; this is the LLM-only proof.

## Persona file format

```
voice=<int>           # Kokoro v1.0 voice index, 0..54
speed=<float>         # 1.0 = native, 1.05 = slightly clipped
fx=<name>             # none | intercom | vintage | robot
---
<system prompt body — multi-line, ends at EOF>
```

## Stack notes

| stage | model / lib                      | size   | notes |
|-------|----------------------------------|--------|-------|
| audio | `miniaudio.h`                    | 1 file | single-header, vendored |
| STT   | Whisper tiny.en (int8)           | ~40 MB | via sherpa-onnx C API |
| VAD   | Silero VAD                       | ~2 MB  | endpointing |
| LLM   | Qwen3-1.7B Q4_K_M                | ~1 GB  | via llama.cpp; OR via OpenRouter |
| TTS   | Kokoro v1.0 multi-lang           | ~333 MB| 54 voices, 8 languages |

Total disk: ~1.5 GB. CPU-only. Sub-second first audio with KV-cached persona prompts on M-series Mac.

## Layout

```
src/
  voicebox.c      main pipeline + protocol parser (~1100 lines)
  openrouter.c    BYO-LLM backend (libcurl + SSE)
  fx.c            DSP filters (intercom / vintage / robot)
  miniaudio.h     vendored single-header audio I/O
  mictest.c       standalone mic capture diagnostic
personas/         four reference personas (NAV-7 + 3 stations)
scenarios/        sector_one, three_stations, audition, random_characters
web/              browser demo (OpenRouter OAuth + streaming chat)
```

## Status

Functional prototype. Used as the reference implementation for
[cenetex/signal#417 (umbrella)](https://github.com/cenetex/signal/issues/417) and
[cenetex/signal#423 (MVP slice)](https://github.com/cenetex/signal/issues/423).
