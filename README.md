# Chasm Bridge FNV

The in-game bridge that connects **Fallout: New Vegas** NPCs to the **chasm** AI backend — live AI dialogue, voice, and actions, in your own playthrough.

Walk up to an NPC, hold a key, and **talk to them with your own voice**. Chasm transcribes what you said, decides who you're addressing, generates an in-character reply with a local LLM, speaks it back in that character's **own cloned voice**, and — when the moment calls for it — has the NPC **act in the world** (follow you, draw a weapon, start combat, hand something over, and more). All of it runs locally on your machine.

This repository is the **game side** of that loop: a native xNVSE plugin plus a Mod Organizer 2 mod folder. It is deliberately thin. New Vegas captures game state (who's nearby, who you're looking at, distance, microphone audio, playback position) and exchanges that with chasm over a local file-based bridge channel. **Chasm owns the brains** — characters, prompts, LLM/TTS/STT settings, voices, lore, quests, and the catalogue of actions an NPC may take.

```
Fallout: New Vegas  ──►  Chasm Bridge FNV  ──►  chasm  ──►  LLM · TTS · STT · actions
   (xNVSE plugin)        (this repo)          (backend)        (local models)
```

---

## What it does

- **Voice in, voice out.** Hold a key while looking at a nearby NPC to speak to them. Your mic audio is transcribed by chasm; the reply is synthesised in the character's cloned voice and played back in-game as **3D positional audio** with range falloff, subtitles, and lip/speech animation.
- **Picks the right speaker.** The bridge reports every nearby, audible NPC (within range) to chasm's Live Chat, and chasm chooses who responds. A separate push-to-talk key routes to an admin/director character ("Todd") for out-of-world / debug requests.
- **Agentic actions.** Alongside the spoken line, chasm can emit structured actions from its **Action Book**. The bridge resolves the trusted binding for each action and runs it in-game via xNVSE, so new actions can be added by editing an Action Book entry — no new C++ handler per action.
- **Low latency.** TTS is streamed sentence-by-sentence so an NPC can start talking before the whole line is synthesised; streamed audio/commands are staged outside MO2's virtual filesystem for native-speed reads.

---

## How it relates to chasm

**chasm** is a local-first, game-agnostic AI backend for agentic game characters. It exposes a headless HTTP API and knows nothing about any specific game until you give it a **bridge**. Chasm Bridge FNV is that bridge for Fallout: New Vegas.

Everything game-specific lives here; everything reusable lives in chasm. Provider-specific LLM/TTS/STT settings, character cards, voice maps, lore, and action definitions are configured in chasm, not in this mod. That keeps the bridge contract universal so other games can use the same API.

The bridge logic itself runs **inside chasm** — chasm reads and writes the game-side bridge files directly. There is no separate process to start and no socket to manage: you run chasm, you launch the game, and they connect.

---

## Requirements

- **Fallout: New Vegas**, launched through **Mod Organizer 2 (MO2)**.
- **xNVSE** (the modern NVSE script extender) installed in the game root.
- **JIP LN NVSE**, **JohnnyGuitar NVSE**, and **NVTF** installed and enabled (ShowOff xNVSE is also recommended).
- **chasm** running, with an LLM selected and TTS/STT configured.

The full, step-by-step list (with download links) is in **[INSTALL.md](INSTALL.md)**.

---

## Install & setup

Setup is a one-time MO2 job, then chasm connects automatically every time you play.

**Follow [INSTALL.md](INSTALL.md)** for the complete walkthrough: installing Mod Organizer 2, xNVSE, the dependency mods (JIP LN, JohnnyGuitar, ShowOff, NVTF), and this mod, plus load order and launch.

The short version of the flow once everything is installed:

1. Install this mod's **`NVBridge`** folder (from `mo2-mod/NVBridge`) into MO2 alongside the dependencies, per INSTALL.md.
2. Start **chasm** — it shows **Not connected**.
3. Launch the game **through MO2** (via `nvse_loader.exe`).
4. Once you load into the game, chasm flips to **Connected**. Walk up to an NPC, hold your talk key, and speak.

> The mod-folder name `NVBridge` and the plugin's internal identifiers matter — don't rename them.

Which NPCs map to which chasm characters, the active group/chat, voices, and the Action Book are all configured **in chasm**, not in this repo.

---

## How it works

1. You hold the talk key while looking at a nearby NPC in range (or the admin/director key to speak to "Todd").
2. The native DLL records your microphone audio and writes a voice request into the bridge folder.
3. chasm picks up the request, transcribes the audio, updates Live Chat with every nearby audible NPC, and generates the turn.
4. chasm selects the responding NPC, generates the reply, and synthesises speech in the character's cloned voice (and any actions).
5. chasm streams the audio and writes any action commands back to the bridge folder.
6. The native DLL plays the voice as 3D positional audio with subtitles and speech animation, and runs any returned in-game actions.

Action execution is data-driven: chasm resolves trusted `fallout-new-vegas:xnvse` bindings and writes a native action command containing the script body, which the plugin compiles and runs through xNVSE. Because that command channel is a local folder written by your own chasm backend, treat your chasm instance as trusted — it is what tells the plugin which in-game scripts to run.

---

## Repository layout

- `mo2-mod/NVBridge/` — the Mod Organizer 2 mod folder (the packaged plugin DLL plus lightweight NVSE bootstrap scripts).
- `native/nvse-plugin/` — C++ source and the Visual Studio project for the native xNVSE bridge DLL. See `native/README.md` for build instructions (the xNVSE SDK is expected at `native/xnvse-sdk/` and is kept local / git-ignored).
- `config/native_debug.example.cfg` — native DirectSound/runtime debug config template.

---

## Building the plugin

The packaged DLL is included in the MO2 mod folder, so most users never need to build. If you want to rebuild it, see `native/README.md`. In short, from a Visual Studio developer shell with the xNVSE SDK checked out at `native/xnvse-sdk/`:

```powershell
msbuild native\nvse-plugin\fnv_bridge_native.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145
```

---

## Status

Experimental, and developed alongside chasm. APIs and data formats may change. Fallout: New Vegas, its assets, and its characters are the property of their respective owners; this is an unofficial, fan-made bridge mod and is not affiliated with or endorsed by Bethesda or Obsidian.
