#!/usr/bin/env bash
# voicebox — fetch and build deps, download models.
# Run once. Idempotent.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="$ROOT/deps"
MODELS="$ROOT/models"
mkdir -p "$DEPS" "$MODELS"

# ---------- 1. sherpa-onnx (STT + VAD + TTS) ----------
if [ ! -d "$DEPS/sherpa-onnx/install" ]; then
  echo ">> building sherpa-onnx"
  cd "$DEPS"
  [ -d sherpa-onnx ] || git clone --depth 1 https://github.com/k2-fsa/sherpa-onnx.git
  cd sherpa-onnx
  cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DSHERPA_ONNX_ENABLE_TTS=ON \
        -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX="$DEPS/sherpa-onnx/install"
  cmake --build build -j --target install
fi

# ---------- 2. llama.cpp ----------
if [ ! -f "$DEPS/llama.cpp/build/bin/libllama.dylib" ] && \
   [ ! -f "$DEPS/llama.cpp/build/bin/libllama.so" ]; then
  echo ">> building llama.cpp"
  cd "$DEPS"
  [ -d llama.cpp ] || git clone --depth 1 https://github.com/ggml-org/llama.cpp.git
  cd llama.cpp
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
  cmake --build build -j
fi

# ---------- 3. Whisper tiny.en (sherpa-onnx flavour) ----------
if [ ! -f "$MODELS/whisper/whisper-encoder.onnx" ]; then
  echo ">> downloading whisper tiny.en"
  mkdir -p "$MODELS/whisper"
  cd "$MODELS/whisper"
  BASE=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models
  curl -L -o tiny.en.tar.bz2 "$BASE/sherpa-onnx-whisper-tiny.en.tar.bz2"
  tar xjf tiny.en.tar.bz2 --strip-components=1
  rm tiny.en.tar.bz2
  # rename canonical files for our code
  mv tiny.en-encoder.int8.onnx whisper-encoder.onnx
  mv tiny.en-decoder.int8.onnx whisper-decoder.onnx
  mv tiny.en-tokens.txt        whisper-tokens.txt
  # silero VAD
  curl -L -o silero_vad.onnx \
    https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx
fi

# ---------- 4. Kokoro TTS (multi-lingual v1.0) ----------
if [ ! -f "$MODELS/kokoro/model.onnx" ]; then
  echo ">> downloading kokoro-en-v0_19"
  mkdir -p "$MODELS/kokoro"
  cd "$MODELS/kokoro"
  curl -L -o kokoro.tar.bz2 \
    https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-en-v0_19.tar.bz2
  tar xjf kokoro.tar.bz2 --strip-components=1
  rm kokoro.tar.bz2
fi

# ---------- 5. tiny LLM (Gemma 3 270M IT, Q4) ----------
if [ ! -f "$MODELS/llm.gguf" ]; then
  echo ">> downloading gemma-3-270m-it Q4_K_M"
  curl -L -o "$MODELS/llm.gguf" \
    "https://huggingface.co/unsloth/gemma-3-270m-it-GGUF/resolve/main/gemma-3-270m-it-Q4_K_M.gguf?download=true"
fi

echo
echo "all set. now run:"
echo "  make"
echo "  ./voicebox models/whisper models/kokoro models/llm.gguf"
