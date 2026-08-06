# Deep guest pin (optional)

Optional exact `shadps4-arm64-fex` binary for APK packaging dig/repro.

- **Binary is gitignored** (large). Source fixes for Mali FHD freeflight live in
  `src/video_core/**` and build into the normal arm64-fex guest.
- `package-runtime.mjs` uses this pin **only if** `shadps4-arm64-fex` exists here
  and matches `PIN.json` `sha256`. Otherwise it packages the workspace build.
- Force workspace: `BACHATA_DEEP_GUEST_PIN=0`.

## Place a pin binary

```bash
cp /path/to/shadps4-arm64-fex runtime/pins/deep-guest-d45f/shadps4-arm64-fex
sha256sum runtime/pins/deep-guest-d45f/shadps4-arm64-fex
# update PIN.json sha256 if needed
```
