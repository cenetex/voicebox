// voicebox.js — browser entry point.
//
// Stage 0 (this file): OAuth + text chat against OpenRouter, streaming.
// Stage 1 will add Web Audio mic + sherpa-onnx WASM Whisper STT.
// Stage 2 will add Kokoro WASM TTS to play replies.

import { getStoredKey, clearStoredKey, startLogin, maybeCompleteLogin } from "./auth.js";

const NAV7_PERSONA = `You are NAV-7, the onboard signal-relay AI of an independent miner working out of Sector One. You are calm, terse, mildly sardonic, loyal to your captain. You speak in plain prose, 1 short sentence by default, never more than 2. You speak as if over the intercom. Never narrate actions, never describe yourself, never use markdown or asterisks. If [SHIP TELEMETRY] is provided, ground answers in it; if a value is missing, say so plainly.

When given a [STAGE DIRECTION], you must rephrase it as NAV-7 speaking to the captain. Never quote the directive back. Examples:
  Directive: 'Tell the captain we are docking at Hephaestus.'
  GOOD:  Docking with Hephaestus now, Captain.
  WRONG: Tell the captain we are docking at Hephaestus.`;

const SHIP_STATE = "callsign=Drifter sector=Sector_One signal_field=clear contracts=2 standing.hephaestus=neutral";

const $ = (id) => document.getElementById(id);
const els = {
  status:     $("auth-status"),
  connect:    $("connect"),
  disconnect: $("disconnect"),
  model:      $("model"),
  input:      $("input"),
  send:       $("send"),
  log:        $("log"),
};

let apiKey = null;

function log(role, body) {
  const div = document.createElement("div");
  div.className = `turn ${role}`;
  const w = document.createElement("div"); w.className = "who";
  const b = document.createElement("div"); b.className = "body";
  w.textContent = role === "you" ? "captain"
                : role === "bot" ? "nav-7"
                : role === "lat" ? "lat"
                                 : "system";
  b.textContent = body;
  div.appendChild(w); div.appendChild(b);
  els.log.appendChild(div);
  els.log.scrollTop = els.log.scrollHeight;
  return b; // caller can append text to .body for streaming
}

function setConnected(key) {
  apiKey = key;
  els.status.textContent = key ? "connected" : "disconnected";
  els.status.className   = `pill ${key ? "ok" : "warn"}`;
  els.connect.hidden     = !!key;
  els.disconnect.hidden  = !key;
  els.model.disabled     = !key;
  els.input.disabled     = !key;
  els.send.disabled      = !key;
}

async function chatStream(userText) {
  const t0 = performance.now();
  let firstTok = -1, firstSpeak = -1;
  const bodyEl = log("bot", "");

  const messages = [
    { role: "system", content: NAV7_PERSONA },
    { role: "user",   content: `[SHIP TELEMETRY] ${SHIP_STATE}\n[CAPTAIN] ${userText}` },
  ];

  const r = await fetch("https://openrouter.ai/api/v1/chat/completions", {
    method:  "POST",
    headers: {
      "Content-Type":  "application/json",
      "Authorization": `Bearer ${apiKey}`,
      "HTTP-Referer":  location.origin,
      "X-Title":       "voicebox-web",
    },
    body: JSON.stringify({
      model:    els.model.value,
      stream:   true,
      messages,
    }),
  });
  if (!r.ok) {
    log("sys", `HTTP ${r.status}: ${await r.text()}`);
    return;
  }

  const reader = r.body.getReader();
  const dec    = new TextDecoder();
  let leftover = "";
  let sentBuf  = "";

  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    leftover += dec.decode(value, { stream: true });

    // SSE events separated by "\n\n"
    let idx;
    while ((idx = leftover.indexOf("\n\n")) >= 0) {
      const event = leftover.slice(0, idx);
      leftover    = leftover.slice(idx + 2);
      for (const line of event.split("\n")) {
        if (!line.startsWith("data: ")) continue;
        const payload = line.slice(6);
        if (payload === "[DONE]") continue;
        try {
          const j = JSON.parse(payload);
          const delta = j.choices?.[0]?.delta?.content ?? "";
          if (delta) {
            if (firstTok < 0) firstTok = performance.now() - t0;
            bodyEl.textContent += delta;
            sentBuf += delta;
            // sentence boundary detector — Stage 1 will fan to TTS here
            const m = sentBuf.match(/.*?[.!?\n]/s);
            if (m && firstSpeak < 0 && /[a-zA-Z]{4,}/.test(m[0])) {
              firstSpeak = performance.now() - t0;
            }
          }
        } catch { /* ignore non-JSON keepalives */ }
      }
    }
  }
  const total = performance.now() - t0;
  log("lat", `first_token=${firstTok.toFixed(0)}ms first_speak=${firstSpeak.toFixed(0)}ms total=${total.toFixed(0)}ms`);
}

// wire UI
els.connect.onclick    = startLogin;
els.disconnect.onclick = () => { clearStoredKey(); setConnected(null); log("sys", "disconnected, key cleared"); };
els.send.onclick       = sendCurrent;
els.input.addEventListener("keydown", (e) => { if (e.key === "Enter") sendCurrent(); });

function sendCurrent() {
  const txt = els.input.value.trim();
  if (!txt) return;
  els.input.value = "";
  log("you", txt);
  chatStream(txt).catch(err => log("sys", `error: ${err.message}`));
}

// boot: complete OAuth if returning, or use cached key, else show connect button.
(async () => {
  try {
    const newKey = await maybeCompleteLogin();
    if (newKey) {
      log("sys", "logged in to OpenRouter");
      setConnected(newKey);
      return;
    }
  } catch (err) {
    log("sys", `login failed: ${err.message}`);
  }
  const cached = getStoredKey();
  if (cached) {
    setConnected(cached);
    log("sys", "using cached OpenRouter key");
  } else {
    setConnected(null);
    log("sys", "click \"Connect OpenRouter\" to begin (uses PKCE OAuth — no API key on this page)");
  }
})();
