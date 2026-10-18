# MW2-RE4

Passthrough mod: Call of Duty: Modern Warfare 2 (2009) gameplay inside Resident Evil 4 (2005) Ultimate HD Edition (Steam).
An x86 ASI plugin (`host/src/MW2RE4.cpp`) inside `bio4.exe` talks to the IW4L MW2 runtime (from the MW2-GTA project) over shared memory.

- Build: `host\Build-ASI.cmd` (MSVC x86) -> `host\build\MW2RE4.asi`
- Install: Ultimate ASI Loader (`dinput8.dll`), `MW2RE4.asi` and `MW2RE4.ini` in RE4's `Bin32`
- Notes and version history: `CLAUDE-CODE-HANDOVER.md`

Not included (never committed): game files, MW2/RE4 assets, the MW2 runtime, `account.dat`, built binaries.
Reference source used: re4_tweaks (commit 92c0208b) for patterns and SDK layouts.
