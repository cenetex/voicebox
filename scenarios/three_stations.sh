#!/usr/bin/env bash
# Multi-persona protocol exercise: NAV-7 + Prospect + Kepler + Helios all
# speak in turn through one voicebox subprocess, demonstrating per-utterance
# persona switching with distinct voices, fx, and (for ASKs) personalities.
set -u
emit() { echo "$1"; sleep "${2:-3}"; }

emit "STATE callsign=Drifter sector=Sector_One signal_field=clear contracts=2 standing.hephaestus=neutral last_haul=18kg_iridium" 1

emit "EVENT nav7 NAV-7 online. Sector One, signal clear, two open contracts." 10
emit "EVENT prospect Belt's quiet today, captain." 8
emit "EVENT kepler Bay clear. Mind the scaffold arm on the way out." 8
emit "EVENT helios Welcome to Helios. Always more to find." 10

# state shifts: signal disturbed
emit "STATE callsign=Drifter sector=Sector_One signal_field=disturbed contracts=2 nearest_megastructure=Hephaestus_Ring distance=4.2Mm" 1
emit "ASK nav7 Brief the captain on the signal disturbance and the distance to Hephaestus Ring." 16

# captain hails Prospect — Prospect responds in character with state context
emit "ASK prospect The captain just hailed. They've been off-grid 4 hours and have iridium aboard. Greet them, say something about the haul." 14

# Helios pitches an upgrade with current state in mind
emit "ASK helios The captain has 18kg iridium. Pitch them on a specialty copper furnace upgrade in your enthusiastic style." 18

emit "QUIT" 0
