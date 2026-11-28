# MXRP Ragdoll — public source (installer and automatic updates)

This folder holds the parts of MXRP Ragdoll that touch your PC's files and the internet, so anyone can check what they do:

- `installer/installer.c`, `installer.rc`, `installer.manifest` — the `.exe` installer: finds MX Bikes, writes the plugin files into the game's `plugins` folder, removes older copies of this plugin (and an old `mxb_ragdoll*.dlo` only if it really is an old version of this plugin).
- `src/update.c`, `src/update.h` — automatic updates: asks GitHub for the latest release of this repository, downloads the new plugin only if its SHA-256 matches the one GitHub lists **and** its `.sig` signature is valid for the author's public key (ECDSA P-256, in `update.c`), then installs it (asking first by default).
- `src/inflate.c`, `src/inflate.h` — small "deflate" decompressor used by the installer (written after Mark Adler's *puff*).

The rest of the plugin (ragdoll, walking, objects, online play) is not published.

These files match the plugin version of the latest release.

## Build

With MinGW-w64 (the plugin file `build/MXRP_Ragdoll.dlo` is embedded in the installer, so the installer needs it next to the sources):

```
x86_64-w64-mingw32-windres -DRC_PLUGIN="\"<version>\"" installer/installer.rc -O coff -o build/installer.res
x86_64-w64-mingw32-gcc -O2 -Wall -s -mwindows -static -DVER="\"<version>\"" installer/installer.c src/inflate.c build/installer.res -o build/MXRP_Ragdoll_Installer.exe -lshell32 -lole32 -ladvapi32
```

## Licence

See [LICENSE.txt](LICENSE.txt): all rights reserved; the code is published so it can be read and checked.
