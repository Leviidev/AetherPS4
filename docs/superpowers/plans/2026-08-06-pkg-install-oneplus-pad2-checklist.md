# OnePlus Pad 2 (OPD2403) install acceptance

Device only: model **OPD2403** / OnePlus Pad 2. Do not sign off on other devices.

## Preconditions

- [ ] `adb devices -l` shows `OPD2403` online (not offline)
- [ ] Debug APK installed with packaged managed runtime (`assets/runtime/manifest.json` + `runtime.zip`)

## Cases

1. **Valid PKG** — Import known-valid `.pkg` from library picker → progress states visible (Validating → … → Installed) → Launch game without manual folder setup
2. **Invalid package** — Import garbage/non-PKG file → `Failed` with specific `InstallErrorCode` (not generic crash)
3. **Interrupt mid-extract** — Start PKG import, `adb shell am force-stop <package>` during extract → relaunch app → no completed/bogus library game for partial tree
4. **Persist after stop** — After successful install, force-stop + reboot → game still listed and launchable
5. **Unified path** — Folder import + PKG import both launch through same gate (`install.manifest` + eboot)

## Results

| Case | Result | Notes |
|------|--------|-------|
| 1 Valid PKG | _pending_ | Pad 2 offline at implementation time |
| 2 Invalid | _pending_ | |
| 3 Interrupt | _pending_ | |
| 4 Persist | _pending_ | |
| 5 Unified | _pending_ | |

Record date/device serial when executed.
