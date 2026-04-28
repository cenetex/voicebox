#!/usr/bin/env bash
# Pick 3 random Kokoro v1.0 voices + a random fx for each, speak a flavor line.
# Each character gets a callsign so you can hear who's who.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VBX="$ROOT/voicebox"
KOKORO="$ROOT/models/kokoro"
WHISPER="$ROOT/models/whisper"
LLM="$ROOT/models/llm.gguf"

# voice catalog (same order as voices.bin)
NAMES=(
  af_alloy af_aoede af_bella af_heart af_jadzia af_jessica af_kore af_nicole af_nova af_river af_sarah af_sky
  am_adam am_echo am_eric am_fenrir am_liam am_michael am_onyx am_puck am_santa
  bf_alice bf_emma bf_isabella bf_lily
  bm_daniel bm_fable bm_george bm_lewis
)
TOTAL=${#NAMES[@]}

FX_OPTIONS=(none intercom vintage robot)
LINES=(
  "This is Hephaestus Dispatch. Drifter, you have an inbound contract."
  "Lighthouse Seven going dark. We are clear of the gate, captain."
  "Captain. Signal field is twitching. I'd recommend re-routing."
)
CALLS=(Hephaestus Lighthouse NavSeven)

for i in 0 1 2; do
    voice=$(( RANDOM % TOTAL ))
    fx="${FX_OPTIONS[$(( RANDOM % ${#FX_OPTIONS[@]} ))]}"
    name="${NAMES[$voice]}"
    echo
    echo "=== character $i  callsign=${CALLS[$i]}  voice=$voice ($name)  fx=$fx ==="
    "$VBX" \
        --say "${LINES[$i]}" \
        --voice "$voice" \
        --fx "$fx" \
        "$WHISPER" "$KOKORO" "$LLM"
done
