# Cinematics

- [x] Extract selected original `CUTS.IMG` scenes and convert ANPK `.IFP` clips to streamed DANM; support the wider cutscene root translations in v3.
- [x] Chain PROLOG1 (airport/area 14) into PROLOG3 (exterior/area 0) with bounded world/collision/cast/vehicle loading, original camera keys, timed GXT `INTRO1` captions and CUTSCENE audio tracks 706/708; skip and natural completion restore gameplay.
- [x] Render original PROLOG1 steward/Bogman/suitcase and PROLOG3 gun/handcuff/cash/Gizmo props, stock Tenpenny/Hernandez/Pulaski meshes, and original taxi/cop-car DFFs with root tracks. Release scene references on completion.
- [ ] Restore the missing backdrop/floor for the late PROLOG1 taxi shot: the current area-14 streamed map does not cover that cutscene camera location, so the cast/vehicle can appear against sky.
- [ ] Apply original camera FOV/roll, vehicle wheel/door/body tracks, and correct prop attachments/visibility; replace stock cop bodies with original cutscene-only models where DS memory allows.
- [ ] Translate the post-PROLOG3 INTRO scripted dialogue/actions and INTRO1a/b mission scenes faithfully, or execute them through an SCM VM; neither the original prologue script nor the full campaign is complete yet.

# Interior and entry/exit integration

## Further IPL fidelity and verification

- [ ] Test all 376 IPL `enex` records in the emulator, especially nested interiors, low-altitude area codes, authored one-way exits, time-gated shops and the outlying stadium/mission rooms. Current round-trip smoke coverage includes area 3 (Carl's house), area 9 (Cluckin' Bell), and an unpaired exterior rooftop warp; the Python extraction and world build cover all IPL records/areas.
- [ ] Port authored per-door pivots and animation curves where available. The runtime checks that a standalone door model has an edge hinge and chooses the opening direction away from CJ; embedded doors inside a building DFF have no separate door object to rotate and use CJ's `Walk_DoorPartial` plus a safe fade/warp. Door-only IFP clips must never be assigned to CJ's skeleton.
- [ ] Port SCM globals, threads, authored access/time conditions and entry creation. The invented three-tier progression has been removed; the partial INTRO/INTRO1 translation only unlocks CJ's IPL house after Big Smoke. The session-only `CHTS > WORLD` overrides authored access/time restrictions. Keep the synthetic reverse portal for IPL area-changing records without a named partner, and replace it with a script-owned partner when SCM provides one.
- [ ] Profile worst-case interiors and verify STREAM/VRAM budgets across repeated transitions and all source map variants. The IPL-only build occupies about 387 MB against the 448 MB NitroFS limit.

## SCM-created entries (out of scope of IPL-only integration)

- [ ] Implement the SCM VM/mission progress and locate the `create_entry_exit`/`set_entry_exit_*` opcodes in `data/script/main.scm` and `script.img`, including mission-specific `CEntryExitManager::AddOne/DeleteOne` calls and conditional access. Do not treat every IPL door as unlocked by default: the original game changes availability from script and save data.
- [ ] Expose a common runtime `EntryExit` registration/lookup API for IPL-baked and SCM-created entries. Give each script-created record a stable script-owned ID distinct from an IPL record index. Apply the same area, collision, marker, streaming, fade and return-path logic to both sources; ensure `unlock all` overrides access checks without discarding script ownership or delete semantics.
- [ ] Extend the versioned, checksummed 16-byte SD mission snapshot (now v2, incompatible with prototype saves) to a full save-state module: SCM flags, per-entry access, player position, inventory, area, entrance stack/return position, and the script-owned entry list. Restore IPL records before applying saved overrides, then restore script-created records after SCM globals/threads. Keep the debug unlock session-only unless a separate persisted option is explicitly introduced.
