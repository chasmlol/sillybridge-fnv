# Chasm Bridge FNV

The in-game bridge that connects **Fallout: New Vegas** NPCs to the **chasm** AI backend — live AI dialogue, voice, and actions, in your own playthrough.

Walk up to an NPC, hold a key, and **talk to them with your own voice**. Chasm transcribes what you said, decides who you're addressing, generates an in-character reply with a local LLM, speaks it back in that character's **own cloned voice**, and — when the moment calls for it — has the NPC **act in the world** (follow you, draw a weapon, start combat, hand something over, and more). All of it runs locally on your machine.

```
Fallout: New Vegas  ──►  Chasm Bridge FNV  ──►  chasm  ──►  LLM · TTS · STT · actions
   (xNVSE plugin)        (this repo)          (backend)        (local models)
```

This repository is the **game side** of that loop: a native xNVSE plugin plus a Mod Organizer 2 mod folder. It is deliberately thin — New Vegas captures game state (who's nearby, who you're looking at, distance, microphone audio, playback position) and exchanges it with chasm over a local bridge channel. **Chasm owns the brains** — characters, prompts, LLM/TTS/STT settings, voices, lore, quests, and the catalogue of actions an NPC may take.

---

# Installation

Setup is a one-time Mod Organizer 2 (MO2) job; after that, chasm connects automatically every time you play. Everything below is free — allow ~20–30 minutes the first time.

## What you need first

- **Fallout: New Vegas** (Steam or GOG) — installed, and launched once to the main menu so it writes its config.
- **chasm** — the AI backend, running on your PC. Start it before you play.
- A little disk space for Mod Organizer 2 and the mods below.

## The dependencies (all required)

| Mod | What it's for | Download |
|-----|---------------|----------|
| **xNVSE** (New Vegas Script Extender) | required by every other mod here | https://github.com/xNVSE/NVSE/releases |
| **JIP LN NVSE Plugin** | core scripting functions | https://www.nexusmods.com/newvegas/mods/58277 |
| **JohnnyGuitar NVSE** | core scripting functions | https://github.com/carxt/JohnnyGuitarNVSE/releases |
| **ShowOff xNVSE** | extra scripting functions | https://github.com/Demorome/ShowOff-NVSE/releases |
| **NVTF** (New Vegas Tick Fix) | stability + performance | https://www.nexusmods.com/newvegas/mods/66537 |
| **Chasm Bridge FNV** (this mod) | connects FNV to chasm | this repository |

## Step 1 — Install Mod Organizer 2 (MO2)

MO2 keeps your mods separate from the game so nothing gets messy.

1. Download the latest **`Mod.Organizer-x.x.x.7z`** from https://github.com/ModOrganizer2/modorganizer/releases.
2. Extract it somewhere simple, e.g. `C:\Modding\MO2`, and run **`ModOrganizer.exe`**.
3. On first run, choose **Create a new instance → Fallout: New Vegas**, and point it at your FNV folder (the one containing **`FalloutNV.exe`**).

## Step 2 — Install xNVSE (into the **game folder**, not MO2)

xNVSE is the script extender and must live in the game's root folder — this is the one mod that does **not** go through MO2.

1. Download xNVSE from https://github.com/xNVSE/NVSE/releases (the `.7z`).
2. Open the archive and copy the **`nvse_*.dll`** files **and `nvse_loader.exe`** into your Fallout: New Vegas folder (right next to `FalloutNV.exe`).
3. You'll launch the game through `nvse_loader.exe` (Step 6).

## Step 3 — Install the four dependency mods (through MO2)

Do this for **JIP LN**, **JohnnyGuitar NVSE**, **ShowOff xNVSE**, and **NVTF**:

1. Download the mod's archive from its link in the table above. (Nexus → use **Manual Download**; GitHub → grab the release `.7z`/`.zip`.)
2. In MO2, click the **disk/archive icon** ("Install a new mod from an archive"), pick the file, and install it.
3. Tick its **checkbox** in the left pane to enable it.

## Step 4 — Install Chasm Bridge FNV (this mod)

**Easiest — from the latest [Release](../../releases):**

1. Download **`NVBridge.zip`** from the Releases page.
2. In MO2 → **Install a new mod from an archive** → select `NVBridge.zip` → keep the name **`NVBridge`** → enable it.

**Or from source:** download this repo (green **Code → Download ZIP**), zip up the **`mo2-mod/NVBridge`** folder, and install that through MO2 the same way.

> The mod-folder name `NVBridge` and the plugin's internal identifiers matter — **don't rename them.**

The mod also ships the **Fallout: New Vegas chasm profile** (characters, lorebook, action/quest books) under `mo2-mod/NVBridge/chasm-profile/` — that's the game content chasm loads, so you don't have to assemble it yourself.

## Step 5 — Load order

- Left pane (mods): the exact order rarely matters for these NVSE plugins — just keep **NVBridge** enabled alongside the others.
- Right pane (plugins): enable any `.esp/.esm` that appear. If unsure, run **LOOT** (built into MO2) to sort automatically.

## Step 6 — Launch and connect

1. Start **chasm** first — it will show **Not connected**.
2. In MO2, set the run target (top-right dropdown) to **`nvse_loader.exe`** and click **Run**.
3. Once you load into the game, chasm flips to **Connected**. Walk up to an NPC, hold your talk key, and speak — it works from there.

## Troubleshooting

- **chasm stays "Not connected"** → make sure you launched via **`nvse_loader.exe`** through MO2 (not the plain game exe), and that NVBridge is enabled in MO2.
- **Game crashes on launch** → you're likely missing xNVSE or a dependency, or the load order needs a LOOT sort.
- **xNVSE not detected** → its files must be in the game root (next to `FalloutNV.exe`), not inside MO2.

> Which NPCs map to which chasm characters, the active chat, voices, and the Action Book are all configured **in chasm**, not in this repo.

---

## What it does

- **Voice in, voice out.** Hold a key while looking at a nearby NPC to speak to them. Your mic audio is transcribed by chasm; the reply is synthesised in the character's cloned voice and played back in-game as **3D positional audio** with range falloff, subtitles, and lip/speech animation.
- **Picks the right speaker.** The bridge reports every nearby, audible NPC (within range) to chasm's Live Chat, and chasm chooses who responds. A separate push-to-talk key routes to an admin/director character ("Todd") for out-of-world / debug requests.
- **Agentic actions.** Alongside the spoken line, chasm can emit structured actions from its **Action Book**. The bridge resolves the trusted binding for each action and runs it in-game via xNVSE, so new actions can be added by editing an Action Book entry — no new C++ handler per action.
- **Low latency.** TTS is streamed sentence-by-sentence so an NPC can start talking before the whole line is synthesised; streamed audio/commands are staged outside MO2's virtual filesystem for native-speed reads.

## How it relates to chasm

**chasm** is a local-first, game-agnostic AI backend for agentic game characters. It knows nothing about any specific game until you give it a **bridge**. Chasm Bridge FNV is that bridge for Fallout: New Vegas.

Everything game-specific lives here (the bridge plugin + the FNV profile content); everything reusable lives in chasm. Provider-specific LLM/TTS/STT settings, character cards, voice maps, lore, and action definitions are configured in chasm, not in this mod. That keeps the bridge contract universal so other games can use the same backend.

## How it works

1. You hold the talk key while looking at a nearby NPC in range (or the admin/director key to speak to "Todd").
2. The native DLL records your microphone audio and sends a voice request to chasm.
3. chasm transcribes the audio, updates Live Chat with every nearby audible NPC, and generates the turn.
4. chasm selects the responding NPC, generates the reply, and synthesises speech in the character's cloned voice (and any actions).
5. chasm streams the audio and any action commands back to the plugin.
6. The native DLL plays the voice as 3D positional audio with subtitles and speech animation, and runs any returned in-game actions.

Action execution is data-driven: chasm resolves trusted `fallout-new-vegas:xnvse` bindings and sends a native action command containing the script body, which the plugin compiles and runs through xNVSE. Because that command comes from your own chasm backend, treat your chasm instance as trusted — it is what tells the plugin which in-game scripts to run.

## Transport (file or HTTP)

By default the plugin exchanges turns with chasm through a **local bridge folder** — simple, asynchronous, and resilient to chasm restarting. An **HTTP transport** is also available: set `transport=http` in `native_debug.cfg` and the plugin POSTs each turn to chasm's streaming `/api/game/v1/turn` endpoint instead, consuming the reply + audio + actions off a background thread. Either way the turn contract is identical; the file path is the default and the safe fallback.

---

## Repository layout

- `mo2-mod/NVBridge/` — the Mod Organizer 2 mod folder (the packaged plugin DLL, the NVSE bootstrap scripts, and the bundled `chasm-profile/` FNV game content).
- `native/nvse-plugin/` — C++ source and the Visual Studio project for the native xNVSE bridge DLL. See `native/README.md` for build instructions (the xNVSE SDK is expected at `native/xnvse-sdk/` and is kept local / git-ignored).
- `docs/PROFILES.md` — the portable chasm-profile format and how chasm imports it.
- `config/native_debug.example.cfg` — native DirectSound/runtime debug config template.

## Building the plugin

The packaged DLL is included in the MO2 mod folder, so most users never need to build. If you want to rebuild it, see `native/README.md`. In short, from a Visual Studio developer shell with the xNVSE SDK checked out at `native/xnvse-sdk/`:

```powershell
msbuild native\nvse-plugin\fnv_bridge_native.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145
```

## Status

Experimental, and developed alongside chasm. APIs and data formats may change. Fallout: New Vegas, its assets, and its characters are the property of their respective owners; this is an unofficial, fan-made bridge mod and is not affiliated with or endorsed by Bethesda or Obsidian.
