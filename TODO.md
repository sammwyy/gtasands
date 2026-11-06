# Interior and entry/exit integration

## Further IPL fidelity and verification

- [ ] Test all 376 IPL `enex` records in the emulator, especially nested interiors, low-altitude area codes, authored one-way exits, time-gated shops and the outlying stadium/mission rooms. Current round-trip smoke coverage includes area 3 (Carl's house), area 9 (Cluckin' Bell), and an unpaired exterior rooftop warp; the Python extraction and world build cover all IPL records/areas.
- [ ] Port authored per-door pivots and animation curves where available. The runtime checks that a standalone door model has an edge hinge and chooses the opening direction away from CJ; embedded doors inside a building DFF have no separate door object to rotate and use CJ's `Walk_DoorPartial` plus a safe fade/warp. Door-only IFP clips must never be assigned to CJ's skeleton.
- [ ] Integrate actual mission/progression unlock conditions when SCM globals and save state exist. The current game has neither, so IPL entrances start disabled and the `CHTS > WORLD` unlock is session-only; it bypasses authored access/time restrictions intentionally. Keep the synthetic reverse portal for IPL area-changing records without a named partner, and replace it with a script-owned partner when SCM provides one.
- [ ] Profile worst-case interiors and verify STREAM/VRAM budgets across repeated transitions and all source map variants. The IPL-only build occupies about 387 MB against the 448 MB NitroFS limit.

## SCM-created entries (out of scope of IPL-only integration)

- [ ] Implement the SCM VM/mission progress and locate the `create_entry_exit`/`set_entry_exit_*` opcodes in `data/script/main.scm` and `script.img`, including mission-specific `CEntryExitManager::AddOne/DeleteOne` calls and conditional access. Do not treat every IPL door as unlocked by default: the original game changes availability from script and save data.
- [ ] Expose a common runtime `EntryExit` registration/lookup API for IPL-baked and SCM-created entries. Give each script-created record a stable script-owned ID distinct from an IPL record index. Apply the same area, collision, marker, streaming, fade and return-path logic to both sources; ensure `unlock all` overrides access checks without discarding script ownership or delete semantics.
- [ ] Add a versioned save-state module with mission flags, per-entry access, current area, entrance stack/return position, and the script-owned entry list. Restore IPL records before applying saved overrides, then restore script-created records after SCM globals/threads. Keep the debug unlock session-only unless a separate persisted option is explicitly introduced.
