# Installing Chasm Bridge FNV

This guide sets up **Fallout: New Vegas** to talk to **chasm** (a local AI backend) so NPCs respond with live AI dialogue, in their own voice, and can act in the world. You set the game up once with Mod Organizer 2; chasm runs in the background and shows **Connected** when the game is live.

Everything below is free. Allow ~20–30 minutes the first time.

---

## What you need first

- **Fallout: New Vegas** (Steam or GOG) — installed, and launched once to the main menu so it generates its config.
- **chasm** — the AI backend, running on your PC. Start it before you play.
- A bit of disk space for Mod Organizer 2 and the mods below.

## The dependencies (all required)

| Mod | What it's for | Download |
|-----|---------------|----------|
| **xNVSE** (New Vegas Script Extender) | required by every other mod here | https://github.com/xNVSE/NVSE/releases |
| **JIP LN NVSE Plugin** | core scripting functions | https://www.nexusmods.com/newvegas/mods/58277 |
| **JohnnyGuitar NVSE** | core scripting functions | https://github.com/carxt/JohnnyGuitarNVSE/releases |
| **ShowOff xNVSE** | extra scripting functions | https://github.com/Demorome/ShowOff-NVSE/releases |
| **NVTF** (New Vegas Tick Fix) | stability + performance | https://www.nexusmods.com/newvegas/mods/66537 |
| **Chasm Bridge FNV** (this mod) | the bridge that connects FNV to chasm | https://github.com/chasmlol/sillybridge-fnv |

---

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

1. Download this repo: green **Code → Download ZIP** at https://github.com/chasmlol/sillybridge-fnv.
2. Inside the download, the mod is the **`mo2-mod/NVBridge`** folder. Zip up that **`NVBridge`** folder.
3. In MO2 → **Install a new mod from an archive** → select your zip → keep the name **`NVBridge`** → enable it.

## Step 5 — Load order

- Left pane (mods): the exact order rarely matters for these NVSE plugins — just keep **NVBridge** enabled alongside the others.
- Right pane (plugins): enable any `.esp/.esm` that appear. If unsure, run **LOOT** (built into MO2) to sort automatically.

## Step 6 — Launch and connect

1. Start **chasm** first — it will show **Not connected**.
2. In MO2, set the run target (top-right dropdown) to **`nvse_loader.exe`** and click **Run**.
3. Once you load into the game, chasm flips to **Connected**. Walk up to an NPC, hold your talk key, and speak — it works from there.

---

## Troubleshooting

- **chasm stays "Not connected"** → make sure you launched via **`nvse_loader.exe`** through MO2 (not the plain game exe), and that NVBridge is enabled in MO2.
- **Game crashes on launch** → you're likely missing xNVSE or a dependency, or the load order needs a LOOT sort.
- **xNVSE not detected** → its files must be in the game root (next to `FalloutNV.exe`), not inside MO2.

> This guide is plain Markdown — feel free to copy it into a Reddit post or wiki.
