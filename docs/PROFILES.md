# Chasm Profiles — bundle format & import design

This document specifies the **profile bundle** that ships with the Chasm Bridge FNV
mod, and the **chasm-side import mechanism** that consumes it. It is a design spec:
the FNV bundle already exists in this repo (see [Where the FNV bundle lives](#where-the-fnv-bundle-lives));
the chasm import code described in [Chasm-side import](#chasm-side-import-design)
and [Implementation checklist](#chasm-side-implementation-checklist) is **not yet
built** and should be implemented as a follow-up in the chasm repo.

## Why profiles

Chasm is a **game-agnostic** AI backend. It knows nothing about Fallout: New Vegas,
or any game, until it is given content: characters, lore, the actions NPCs may take,
quests, world/persona config. Historically that content lived inside chasm's own data
directory, which tied the "standalone" backend to one game.

A **profile** decouples them. A profile is a single self-contained folder holding
**everything authored about one game**. Profiles travel **with the game's mod**, not
with chasm:

- Chasm ships with **no game content** — empty `profiles/`, no characters, no lore.
- The FNV mod ships a profile **inside its mod download**. When the mod boots and
  chasm is running, chasm **copies that profile** into its own `profiles/` folder.
- Users can also **drag-and-drop** a profile folder into chasm.

The result: chasm is a clean standalone app; each game brings its own brain.

## Bundle format

A profile bundle is **one folder**, named by the profile id (e.g.
`fallout-new-vegas/`). The on-disk layout deliberately mirrors the per-profile layout
chasm already resolves in `crates/chasm-core/src/profiles.rs`
(`ProfilePaths`) and reads in `crates/chasm-st-compat/src/sources.rs`, so an
imported bundle loads **as-is** with no transformation:

```
fallout-new-vegas/
  profile.json                                  # manifest (see below)
  characters/                                   # SillyTavern PNG character cards (V2/V3)
    Easy Pete.png
    Doc Mitchell.png
    … (one .png per NPC; Todd = admin/director card)
  worlds/                                        # lorebooks (SillyTavern "World Info" JSON)
    Fallout New Vegas.json
  headless/
    action-books/                                # NPC/GM action allowlists
      Fallout New Vegas Action Book.json
    quest-books/                                 # quest pre-dialogue + prompt-safe metadata
      Fallout New Vegas Quest Book.json
    action-catalogs/                             # generated spawnable-record catalogs
      fallout-new-vegas.entities.json
      fallout-new-vegas.items.json
  groups/                                        # optional: SillyTavern group definition
    fnv-goodsprings.json
```

Each subfolder maps 1:1 to a `ProfilePaths` accessor, so `profiles/<id>/<subdir>`
wins over the legacy `{data_root}/<subdir>` automatically once the folder is present:

| Bundle path                              | `ProfilePaths` accessor      | Reader (`sources.rs`)                        |
|------------------------------------------|------------------------------|----------------------------------------------|
| `characters/*.png`                       | `characters_dir()`           | `read_character_card` (PNG `ccv3`/`chara`)   |
| `worlds/*.json`                          | `worlds_dir()`               | `read_lorebook` / `list_lorebooks`           |
| `headless/action-books/*.json`           | `action_books_dir()`         | `list_action_books`                          |
| `headless/quest-books/*.json`            | `quest_books_dir()`          | `list_quest_books` / `read_quest_book`       |
| `headless/action-catalogs/*.json`        | `action_catalogs_dir()`      | `read_action_catalog` / `list_action_catalogs` |

### `profile.json` (manifest)

Parsed by `GameProfile` (`crates/chasm-core/src/profiles.rs`). Known fields are
`id`, `name`, `description`, `characters[]`; **any other top-level keys are preserved
losslessly** via serde `flatten` into `extra` (the model and its round-trip test
explicitly keep `voice` and `comment`).

```json
{
    "id": "fallout-new-vegas",
    "name": "Fallout: New Vegas",
    "description": "Goodsprings starting area …",
    "bundleVersion": 1,
    "game": "fallout-new-vegas",
    "comment": "Authored content only; no runtime/user data bundled.",
    "voice": { "plugin": "FalloutNV.esm", "extractor": "extract_voices.py" },
    "characters": [
        { "name": "Easy Pete" },
        { "name": "Doc Mitchell" }
    ]
}
```

Field notes:

- **`id`** — folder name and the value chasm stores as the active profile id
  (`settings.profile_id`; `active_profile_id()` validates that `profiles/<id>/profile.json`
  exists). Use a filesystem-safe slug.
- **`name`, `description`** — shown in chasm's profile picker.
- **`bundleVersion`** — bundle-format version (this spec = `1`). Lets a future importer
  migrate older layouts. Lives in `extra`; not interpreted by today's `GameProfile`.
- **`game`** — stable game key (matches the `targetGame` used across the books, e.g.
  `fallout-new-vegas`). Also in `extra`.
- **`characters[]`** — `ProfileCharacter { name, edid?, voicetype? }`. Each `name`
  matches a card file stem in `characters/` (and the live participant name). `edid`
  (game editor id) and `voicetype` are **optional** voice-extractor hints; the FNV
  bundle omits them because the live bridge resolves NPCs at runtime via gamestate +
  the card's `participantId`, not a static edid table.
- **`voice`** — the per-profile voice-extractor block (`plugin`, `extractor`),
  preserved verbatim in `extra`. Drives chasm's voice extraction for this game.
- **`comment`** — free-form note, preserved in `extra`.

### What a bundle includes vs. excludes

**Include — authored game content only:**

- `profile.json` manifest
- character cards (`characters/*.png`)
- lorebook(s) (`worlds/*.json`)
- action book(s), quest book(s), action catalogs (`headless/…`)
- optional group definition (`groups/*.json`)

**Exclude — per-user runtime / transient data (chasm creates these on first use):**

- chat logs & history — `chats/`, `group chats/`, `*.jsonl`, `headless/live-chats.json`
- save-sync snapshots — `headless/save-sync/`
- world state — `headless/world-state.json` (global, not per-profile)
- embeddings / vector cache — `embed-cache/`, `vectors/`
- voices (refs + clones) — `voices/` (configured in chasm; not shipped)
- backups, thumbnails, downloads, model files, app settings

The included set is exactly the authored content the live readers in `sources.rs`
consume; everything excluded is regenerated by chasm per user/playthrough.

## Where the FNV bundle lives

The canonical FNV bundle is committed at:

```
mo2-mod/NVBridge/chasm-profile/fallout-new-vegas/
```

It lives **inside the MO2 mod folder** so it is part of what the user downloads and
what MO2 deploys — i.e. it literally "ships with the mod." Placing it under
`chasm-profile/` (a sibling of `nvse/`) keeps it out of the game's own asset paths;
it is reference content for chasm, not a game asset MO2 needs to virtualize.

The same folder is the **drag-and-drop unit**: a user can drag
`NVBridge/chasm-profile/fallout-new-vegas/` straight into chasm.

> Single source of truth: there is intentionally **one** copy of the bundle in this
> repo (no duplicate under a top-level `profiles/`), so the ~12 MB of catalogs and the
> character cards are not committed twice and cannot drift.

## Chasm-side import (design)

Two import paths, both landing the bundle in chasm's `profiles_dir`
(`CHASM_PROFILES_DIR`, else `{workspace_root}/profiles`). Both are **game-agnostic**:
chasm never hard-codes "Fallout"; it just copies validated bundle folders.

### A. Mod-boot copy (automatic)

The mod and chasm already share a **file-based bridge channel**, and chasm "reads and
writes the game-side bridge files directly" (see the repo README). The profile import
reuses that connection — no network, no new process.

**Trigger:** when chasm detects the game connection is live (the existing
heartbeat/PID-driven connection that flips chasm to **Connected**), it runs a
**one-shot profile sync**.

**Source location:** the connected mod advertises a **profile source directory** —
the folder that contains the bundle. Two concrete options (pick one when implementing):

1. **Convention relative to the bridge channel.** The bridge already exchanges files
   at a known mod path; chasm resolves the profile source as a fixed sibling, e.g.
   `<bridge_root>/../chasm-profile/`. Zero new config; works because the mod folder
   layout is fixed (`NVBridge/chasm-profile/<id>/`).
2. **Declared in a handshake/manifest field.** The mod's connection handshake (or a
   small `bridge.json`/heartbeat payload it already writes) includes
   `profileSource: "<abs path to chasm-profile dir>"`. Slightly more plumbing, but
   explicit and robust to layout changes.

   Recommended: **(1)** for FNV now (fixed layout, nothing to add to the plugin), with
   the importer falling back to **(2)** if a `profileSource` field is present — so other
   games can override the convention.

**Copy algorithm (per discovered `<source>/<id>/` bundle):**

1. Read `<source>/<id>/profile.json`; if missing/invalid → skip (not a bundle).
2. Validate `id` is a safe slug (no `/`, `\`, `..`); reject otherwise.
3. Compare with `profiles/<id>/`:
   - **absent** → copy the whole bundle in.
   - **present** → compare `bundleVersion` (and/or a content hash). Copy only if the
     mod's bundle is newer; otherwise leave the user's profile untouched (the user may
     have edited it). Never overwrite silently on equal/older versions.
4. Copy **content subfolders only** (`profile.json`, `characters/`, `worlds/`,
   `headless/action-books`, `headless/quest-books`, `headless/action-catalogs`,
   `groups/`). **Never** copy runtime dirs even if present in the source
   (`chats/`, `group chats/`, `embed-cache/`, `vectors/`, `voices/`, `headless/live-chats.json`,
   `headless/save-sync/`, `headless/world-state.json`).
5. Copy atomically: write to `profiles/<id>.tmp/`, then rename into place.
6. If no profile is active yet (`settings.profile_id` empty), optionally set the freshly
   imported profile as active so the game "just works" on first connect.

The copy is **idempotent** — reconnecting with the same bundle version is a no-op.

### B. Drag-and-drop (manual)

A drop target in chasm's UI (the Profiles settings page) accepts a **folder drop**.
On drop:

1. Locate `profile.json` at the dropped folder root (if the user dropped a parent that
   contains several `<id>/` bundles, import each).
2. Run the same validate + copy algorithm as the mod-boot path (steps 1–5 above),
   skipping the runtime dirs.
3. Refresh the profile list; surface name/description and let the user activate it.

This is the same importer as path A with a different trigger and source — implement
the copy once and call it from both.

### How this makes chasm standalone

- Chasm's repo and installer ship **no game content**; `profiles/` starts empty and the
  app shows no characters until a profile arrives.
- The **only** ways a profile arrives are (A) a connected mod copying its bundled
  profile in, or (B) a user dragging a folder in. Both are generic folder copies keyed
  off `profile.json` — chasm stays game-agnostic.
- Per-user data (chats, voices, embeddings, save-sync) is always created locally by
  chasm and is never part of a shippable bundle, so bundles stay small, reviewable, and
  free of personal data.

## Chasm-side implementation checklist

Concrete, ordered work for the follow-up in the **chasm** repo.
File references are to the current code read while writing this spec.

1. **Manifest fields (optional, additive).** In
   `crates/chasm-core/src/profiles.rs`, the new keys (`bundleVersion`, `game`,
   `voice`, `comment`) already round-trip via `GameProfile.extra` (serde `flatten`).
   If first-class access is wanted, add typed optional fields
   (`bundle_version: Option<u32>`, `game: Option<String>`) with `#[serde(default)]`;
   otherwise read them from `extra`. No format change required.

2. **Importer module.** Add `crates/chasm-core/src/profile_import.rs` exposing:
   - `fn import_bundle(source_bundle_dir: &Path, profiles_dir: &Path, opts) -> Result<ImportOutcome>`
     — validate `profile.json`, slug-check `id`, version-compare against
     `profiles/<id>/`, copy the **allowlisted** content subdirs only, atomic
     tmp-dir + rename. Returns `{ id, action: Installed | Updated | SkippedUpToDate | Rejected }`.
   - `fn import_from_source_root(source_root: &Path, profiles_dir: &Path, opts)` —
     iterate `<source_root>/<id>/` children and call `import_bundle` for each
     directory that has a `profile.json`.
   - An **allowlist constant** of copyable entries (mirror the "Include" list above)
     and a **denylist** of runtime dirs (mirror the "Exclude" list) so runtime data is
     never copied even if a malformed source contains it.

3. **Mod-boot trigger.** Where chasm flips to **Connected** (the heartbeat/PID
   connection lifecycle — see the recent "Tie game connection to the plugin PID" and
   "Auto-manage AI stack from the game connection" commits in the chasm repo), call
   `import_from_source_root` **once per connection** against the resolved profile source:
   - Resolve source via convention `<bridge_root>/../chasm-profile/` (FNV layout:
     `NVBridge/chasm-profile/`), with an optional `profileSource` override read from the
     mod handshake/heartbeat payload if present.
   - Guard with a per-connection "already synced" flag so it does not re-run every
     heartbeat.
   - On first import with no active profile, set `settings.profile_id` to the imported
     id and persist (`AppSettings::save`), so `active_profile_paths()` immediately
     resolves into the new profile.

4. **Drag-and-drop endpoint + UI.** In `crates/chasm-web`:
   - Add a route (e.g. `POST /api/profiles/import`) that accepts a folder path (native
     shell drop) or an uploaded archive, then calls the importer.
   - Add the drop target to the Profiles settings page
     (`crates/chasm-web/src/ui/settings.rs`, profiles category — the nav group
     already exists). Show import result (Installed/Updated/Skipped) and refresh the
     profile list.

5. **Active-profile resolution (already works).** No change needed:
   `AppSettings::active_profile_id(&profiles_dir)` returns the configured `profile_id`
   when its `profile.json` exists, else the first listed profile; `ProfilePaths`
   resolves each `profiles/<id>/<subdir>` over the legacy data root. Verify the
   importer writes the same subdir names the accessors expect (table above).

6. **Tests.** Add `profile_import` tests covering: install into empty `profiles/`;
   skip when same `bundleVersion`; update when newer; reject bad `id` (`..`, separators);
   and assert the denylisted runtime dirs are **not** copied even when present in the
   source fixture.

7. **Validation pass.** After implementing, point a dev chasm instance at this repo's
   `mo2-mod/NVBridge/chasm-profile/` as the source root, import, and confirm the
   Goodsprings roster, lorebook, action/quest books, and catalogs load (characters
   visible; lore/actions retrievable) — i.e. the bundle round-trips through the real
   readers in `sources.rs`.
