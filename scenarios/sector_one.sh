#!/usr/bin/env bash
# Drives voicebox --ship with Signal-flavored line protocol.
# Usage: ./scenarios/sector_one.sh | ./voicebox --ship models/whisper models/kokoro models/llm.gguf
set -u

emit() { echo "$1"; sleep "${2:-3}"; }

# initial state — Signal nouns: callsign, sector, signal field, contracts, standing, last haul
emit "STATE callsign=Drifter sector=Sector_One signal_field=clear contracts=2 standing.hephaestus=neutral last_haul=18kg_iridium time_off_grid=4h" 2

# pre-rendered station chatter (deterministic, bypasses LLM)
emit "EVENT NAV-7 online. Sector One, signal clear, two contracts open." 12

# state change + LLM-mediated brief
emit "STATE callsign=Drifter sector=Sector_One signal_field=disturbed contracts=2 standing.hephaestus=neutral last_haul=18kg_iridium nearest_megastructure=Hephaestus_Ring distance=4.2Mm" 1
emit "ASK Brief the captain on the signal disturbance. Note we are 4.2 megameters from Hephaestus Ring." 18

# contract notification — verbatim TTS
emit "EVENT Hephaestus Dispatch: contract acceptance window for haul C-218 closes in sixty seconds." 12

# captain may have answered — give them time, then state-aware ASK
emit "STATE callsign=Drifter sector=Sector_One signal_field=disturbed contracts=2 contract_C218_status=pending standing.hephaestus=neutral pending_haul=42kg_chromite_to_Hephaestus" 1
emit "ASK Confirm with the captain whether to accept contract C-218: forty-two kilos of chromite to Hephaestus." 20

# someone just left signal range — Signal lore: stations notice
emit "EVENT Signal trace: Lighthouse-7 has not reported in nineteen hours. Hephaestus has flagged the loss." 12

emit "QUIT" 0
