# voicebox — minimal local STT+LLM+TTS in C
#
# Set these to where you built/installed the deps:
#   SHERPA_DIR : sherpa-onnx install prefix (has include/sherpa-onnx and lib/)
#   LLAMA_DIR  : llama.cpp build dir       (has include/ and build/bin or lib/)
#
# Vendored: src/miniaudio.h (single header; download from mackron/miniaudio)

SHERPA_DIR ?= $(HOME)/develop/voicebox/deps/sherpa-onnx/install
LLAMA_DIR  ?= $(HOME)/develop/voicebox/deps/llama.cpp

CC      ?= cc
CFLAGS  ?= -O3 -std=c11 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Isrc -I$(SHERPA_DIR)/include -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
LDFLAGS += -L$(SHERPA_DIR)/lib -L$(LLAMA_DIR)/build/bin -L$(LLAMA_DIR)/build/src

LDLIBS  = -lsherpa-onnx-c-api -lonnxruntime \
          -lllama -lggml -lggml-base -lggml-cpu \
          -lcurl \
          -lpthread -lm

# macOS: AudioToolbox + CoreAudio for miniaudio; Accelerate often pulled by ggml
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDLIBS += -framework CoreAudio -framework CoreFoundation -framework AudioToolbox \
            -framework AudioUnit -framework Accelerate -framework Metal \
            -framework MetalKit -framework Foundation
  LDFLAGS += -Wl,-rpath,$(SHERPA_DIR)/lib -Wl,-rpath,$(LLAMA_DIR)/build/bin
endif
ifeq ($(UNAME_S),Linux)
  LDLIBS  += -ldl -lasound
  LDFLAGS += -Wl,-rpath,$(SHERPA_DIR)/lib -Wl,-rpath,$(LLAMA_DIR)/build/bin
endif

voicebox: src/voicebox.c src/openrouter.c src/openrouter.h src/fx.c src/fx.h src/miniaudio.h
	$(CC) $(CFLAGS) src/voicebox.c src/openrouter.c src/fx.c -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f voicebox

.PHONY: clean
