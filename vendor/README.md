# vendor/

Third-party binaries you supply yourself. Nothing in here is committed (see `.gitignore`); these files are
not ours to redistribute.

| File | Needed for | Where it comes from |
|---|---|---|
| `nvidia/nvngx_dlssnr.dll` | Always: the DLSS 5 neural-rendering snippet (310.8 tested) | NVIDIA (not distributed here) |
| `rtinitfix/GFSDK_Aftermath_Lib.x64.dll` | Always: deploy installs it in place of the game's copy | RTInitFix |
| `rtinitfix/sl.interposer.dll` | Only with `tools\deploy.ps1 -UseRTInitFix` | RTInitFix |

`tools\deploy.ps1` checks for these and says which one is missing.
