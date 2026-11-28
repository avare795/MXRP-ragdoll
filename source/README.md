# MXRP Ragdoll — source code

This folder holds the full source of MXRP Ragdoll (plugin, installer, automatic updates, tests), so anyone can read it and check what the plugin does.

- `src/` — the plugin (`MXRP_Ragdoll.dlo`). `src/update.c` is the automatic update (GitHub check, SHA-256 + author signature, ECDSA P-256 public key inside).
- `installer/` — the `.exe` installer.
- `tests/` — tests run by `make` on Linux before every build.
- `config/` — default settings; `MXRP_Ragdoll_LISEZ-MOI_README.txt` — the guide shipped with the plugin.
- `tools/` — scripts that generated the 3D models in `src/*_model.inc` (the original model files are not included) and `sign_release.py` (signs a release; the private key is never in this repository).

## Releases are built by GitHub Actions

Since Beta 3.4, every release is built from this folder by [GitHub Actions](../.github/workflows/release.yml) started by hand from the Actions tab: the Actions log shows the commit, the build, the tests and the SHA-256 of every file attached to the release (`SHA256SUMS.txt`).

## Build it yourself

On Linux with MinGW-w64 (`gcc-mingw-w64-x86-64`):

```
make                                   # tests + build/MXRP_Ragdoll.dlo
make installer-release NAME="MXRP Beta X.Y"   # build/MXRP Beta X.Y.exe
```

## Licence

See [LICENSE.txt](LICENSE.txt): all rights reserved; the code is published so it can be read, built and checked.

Third-party data used by the plugin: walking animation from the CMU Graphics Lab Motion Capture Database (mocap.cs.cmu.edu); FMX ramps from PROCworks FMX Ramp PACK, the "FMX Ramps" pack and "thefluffygecko fmx ramps"; green camping chair by lpfeffer (CC-BY-4.0); folding table by Innerscene (CC0).
