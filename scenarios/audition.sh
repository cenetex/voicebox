#!/usr/bin/env bash
# Audition Kokoro v1.0 voices — synthesize a short NAV-7 line with each
# candidate index and play it. Runs through the list one at a time so you
# can hear them in sequence.
#
# Usage: ./scenarios/audition.sh [start_idx] [end_idx]   (default 12..28)

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TTS="$ROOT/deps/sherpa-onnx/install/bin/sherpa-onnx-offline-tts-play"
KOKORO="$ROOT/models/kokoro"
TEXT="${TEXT:-Captain. Sector One signal is clear. Two contracts open. Hephaestus is hailing.}"

START=${1:-12}
END=${2:-28}

# Voice catalog (Kokoro v1.0 voices.bin order — English-tier)
declare -a NAMES=(
  af_alloy af_aoede af_bella af_heart af_jadzia af_jessica af_kore af_nicole af_nova af_river af_sarah af_sky
  am_adam am_echo am_eric am_fenrir am_liam am_michael am_onyx am_puck am_santa
  bf_alice bf_emma bf_isabella bf_lily
  bm_daniel bm_fable bm_george bm_lewis
)

for i in $(seq "$START" "$END"); do
  name="${NAMES[$i]:-?}"
  echo
  echo "=== voice $i  ($name) ==="
  "$TTS" \
    --kokoro-model="$KOKORO/model.onnx" \
    --kokoro-voices="$KOKORO/voices.bin" \
    --kokoro-tokens="$KOKORO/tokens.txt" \
    --kokoro-data-dir="$KOKORO/espeak-ng-data" \
    --kokoro-lexicon="$KOKORO/lexicon-us-en.txt,$KOKORO/lexicon-gb-en.txt" \
    --kokoro-dict-dir="$KOKORO/dict" \
    --kokoro-lang="en-us" \
    --num-threads=2 \
    --sid="$i" \
    --output-filename="/tmp/nav7-$i.wav" \
    "$TEXT" 2>/dev/null
done
echo
echo "Pick one and update personas/nav7.persona — set voice=<index>."
