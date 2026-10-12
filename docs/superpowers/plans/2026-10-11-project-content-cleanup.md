# Project content cleanup implementation plan

> Execute inline in the current user checkout; the latest authored Unreal assets are untracked and must be preserved through verified backups.

**Goal:** Organize Test_Map (menu) and Demo (gameplay) inside Content/Project, with the existing SinglePlayerMap as multiplayer lobby. Remove other maps and unused assets only within Content/Project. Preserve material-pack demonstration maps outside Project.

**Architecture:** Keep feature-owned assets under Content/Project/<Feature>. Use Unreal AssetRegistry dependency data and editor asset moves, retaining hard/soft references, native load paths, config roots, and GameFeature runtime assets. Content/Test_Map must become removable without breaking the retained content.

**Constraints:** Preserve the current BP_Actor fix and all shared runtime assets. Back up every modified/deleted package with hashes outside Content. Do not delete assets merely because their reference count is zero. Imported examples outside Project remain. Do not save unrelated assets or settings.

## Tasks

- [x] Inventory assets, maps, dependencies, referencers, code/config paths, redirects, and external actors. Write a machine-readable before report.
- [x] Create and verify backups. Establish retained runtime roots and deletion candidates; record the reasoning per package.
- [x] Copy retained maps and required Test_Map content into feature folders with UE AssetTools; update references and config while preserving source examples.
- [x] Remove unused Project maps and their exclusive dependency assets through an explicit, backed-up manifest. No global redirector fixup.
- [x] Validate three editor map loads, Cook, Blueprint wiring, GameFeature manifest, two-player Box PIE and pickup presentation. Production menu-to-lobby-to-game multiplayer travel and rendered inspection were not exercised.
- [x] Save a final cleanup manifest, backup/restore instructions, and concise user-facing map paths and verification limits.

## Rulings and progress

- User clarified: only Content/Project cleanup; preserve asset-pack demonstration maps.
- Use this checkout because it contains the latest authored assets; do not create an empty Git worktree that omits untracked Content.
- Keep the existing BMMCSystem/SinglePlayerMap location and package unchanged, per the established protected-template contract; the request explicitly relocates Content/Test_Map, not BMMCSystem. Three production map roles remain Test_Map, SinglePlayerMap, Demo.
- Copied 378 packages through UE AdvancedCopy with header reference patching. Default object duplication hit a Landscape subsystem assertion before writing output; the engine-supported header path succeeded and was checked by load/dependency/Cook validation.
- Deleted L_Test and eight exclusive legacy framework/input packages. Independent AI assets, reusable animation libraries, native load paths and test assets remain.
- Project assets: 269 -> 638. All 2,099 external Content files, including 464 BMMCSystem files and 410 Test_Map files, have unchanged SHA-256 hashes.
- Three-map Cook: success, 0 errors, 28 warnings (existing BMMC gameplay tags, disabled Steam, and original M_Decal_7 without an input texture). Six selected automation tests passed.
- Final independent read-only review passed; corrected map-load wording to accurately describe sequential loads in one editor process. Reviewer independently rehashed all 2,099 external Content files with no changes.
