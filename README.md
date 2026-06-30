# Chasm Bridge FNV

The in-game bridge that connects **Fallout: New Vegas** NPCs to the **chasm** AI backend — live AI dialogue, voice, and actions, in your own playthrough.

Walk up to an NPC, hold a key, and **talk to them with your own voice**. Chasm transcribes what you said, decides who you're addressing, generates an in-character reply with a local LLM, speaks it back in that character's **own cloned voice**, and — when the moment calls for it — has the NPC **act in the world** (follow you, draw a weapon, start combat, hand something over, and more). All of it runs locally on your machine.

This repository is the **game side** of that loop: a native xNVSE plugin plus a Mod Organizer 2 mod folder and a small helper. It is deliberately thin. New Vegas captures game state (who's nearby, who you're looking at, distance, microphone audio, playback position) and asks chasm what to do over chasm's headless HTTP API. **Chasm owns the brains** — characters, prompts, LLM/TTS/STT settings, voices, lore, quests, and the catalogue of actions an NPC may take.

```
Fallout: New Vegas  ──►  Chasm Bridge FNV  ──►  chasm  ──►  LLM · TTS · STT · actions
   (xNVSE plugin)        (this repo)          (backend)        (local models)
```

---

## What it does

- **Voice in, voice out.** Hold a key while looking at a mapped NPC to speak to them. Your mic audio is transcribed by chasm; the reply is synthesised in the character's cloned voice and played back in-game as **3D positional audio** with range falloff, subtitles, and lip/speech animation.
- **Picks the right speaker.** The bridge reports every nearby, audible NPC (within range) to chasm's Live Chat, and chasm chooses who responds. A separate push-to-talk key routes to an admin/director character ("Todd") for out-of-world / debug requests.
- **Agentic actions.** Alongside the spoken line, chasm can emit structured actions from its **Action Book**. The bridge resolves the trusted binding for each action and runs it in-game via xNVSE, so new actions can be added by editing an Action Book entry — no new C++ handler per action.
- **Low latency.** TTS is streamed sentence-by-sentence so an NPC can start talking before the whole line is synthesised; streamed audio/commands are staged outside MO2's virtual filesystem for native-speed reads.

---

## How it relates to chasm

**chasm** is a local-first, game-agnostic AI backend for agentic game characters. It exposes a headless HTTP API and knows nothing about any specific game until you give it a **bridge**. Chasm Bridge FNV is that bridge for Fallout: New Vegas.

Everything game-specific lives here; everything reusable lives in chasm. Provider-specific LLM/TTS/STT settings, character cards, voice maps, lore, and action definitions are configured in chasm, not in this mod. That keeps the bridge contract universal so other games can use the same API.

---

## Requirements

- **Fallout: New Vegas**, launched through **Mod Organizer 2 (MO2)**.
- **xNVSE** (the modern NVSE script extender) installed in the game root.
- **JohnnyGuitar NVSE**, **JIP LN NVSE**, and **NVTF** installed and enabled (ShowOff NVSE is also recommended).
- **chasm** running with its headless API enabled, an LLM selected, and TTS/STT configured.
- A `HEADLESS_API_KEY` available to the helper (via `.env` or the environment) if your chasm instance requires one.

---

## Install & setup

### The easy way — let chasm install it

chasm ships an MO2 auto-setup that can install this bridge for you: it detects your Mod Organizer 2, pre-flights the required mods (xNVSE, JohnnyGuitar, JIP LN, NVTF, etc.), drops in the `NVBridge` mod folder from this repo, and warms the AI stack on **Play**. If you're using chasm's launcher, you generally don't need to touch this repo by hand.

> Note: the mod-folder name `NVBridge` and the plugin's internal identifiers are detected by chasm by name — don't rename them.

### The manual way — MO2

1. Copy `mo2-mod/NVBridge` into your Mod Organizer 2 `mods` folder and enable it.
2. Make sure `fnv_bridge_native.dll` ends up at `Data/NVSE/Plugins/fnv_bridge_native.dll` inside the active MO2 virtual filesystem.
3. Copy `config/native_debug.example.cfg` to your writable runtime bridge folder as `native_debug.cfg`, and set `bridge_root_path` to your MO2 overwrite `NVBridge` folder.
4. Import `easy-pete.character.json` into chasm as a starter character card.
5. In chasm, create a group containing the NPC character cards you want the game to use.
6. Copy `nvbridge.config.example.json` to a local config (e.g. `nvbridge.config.local.json`), set `groupId` to your chasm group, and map native NPC keys to character ids under `npcCharacterMap`. Replace the `<path-to-your-...>` placeholders with your real install paths (or set them via the `NVBRIDGE_DATA_ROOT(S)` / `NVBRIDGE_NATIVE_ROOT(S)` environment variables instead).
7. Start chasm.
8. Start the helper:

   ```powershell
   npm run start
   ```

   Or with a custom config:

   ```powershell
   node tools/nvbridge-helper.mjs --config .\nvbridge.config.local.json
   ```

> Keep your local config out of version control — don't commit real API keys, group ids, or machine-specific paths.

---

## How it works

1. You hold the talk key while looking at a mapped NPC in range (or the admin/director key to speak to "Todd").
2. The native DLL records your microphone audio and writes a voice request into the bridge folder.
3. The helper sends the audio to chasm for transcription, updates Live Chat with every nearby audible NPC, and asks chasm to generate the turn.
4. chasm selects the responding NPC, generates the reply, and streams back synthesised speech (and any actions).
5. The helper streams the audio and writes any action commands back to the native DLL.
6. The native DLL plays the voice as 3D positional audio with subtitles and speech animation, and runs any returned in-game actions.

Action execution is data-driven: the helper requests trusted `fallout-new-vegas:xnvse` bindings from chasm and writes a native action command containing the script body, which the plugin compiles and runs through xNVSE. Because that command channel is a local folder written by your own helper/backend, treat your chasm instance as trusted — it is what tells the plugin which in-game scripts to run.

---

## Repository layout

- `mo2-mod/NVBridge/` — the Mod Organizer 2 mod folder (the packaged plugin DLL plus lightweight NVSE bootstrap scripts).
- `native/nvse-plugin/` — C++ source and the Visual Studio project for the native xNVSE bridge DLL. See `native/README.md` for build instructions (the xNVSE SDK is expected at `native/xnvse-sdk/` and is kept local / git-ignored).
- `tools/nvbridge-helper.mjs` — the Node helper that watches the bridge files, calls chasm, streams audio, and publishes responses back to the native DLL.
- `easy-pete.character.json` — a starter Easy Pete character card for testing.
- `nvbridge.config.example.json` — helper config template.
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
