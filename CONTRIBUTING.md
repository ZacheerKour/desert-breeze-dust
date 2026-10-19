# Contributing

- Build from source: see "Building from source" in [README.md](README.md) (`INSTALL.bat` with your own disc image). Engineering notes are in [docs/RECOMP_ANALYSIS.md](docs/RECOMP_ANALYSIS.md).
- Layout: `pd-game-v2/` game project, `xboxrecomp/` runtime and recompiler, `tools/pipeline/` code generation, `tools/analysis/` research scripts, `dev/` test and automation scripts, `docs/` notes.
- Pull requests must not contain game data (disc images, extracted files, `default.xbe`) or generated code (`pd-game-v2/src/recomp/gen`, `recomp-analysis/`). They are regenerated locally from your own disc.
- Do not put personal paths, user names or e-mail addresses in files or commit messages.
- Describe what you tested (which mission or menu) and attach the relevant part of `pd-game-v2\run_user.err` for runtime changes.
