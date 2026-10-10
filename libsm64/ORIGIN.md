# Vendored libsm64

Upstream: https://github.com/libsm64/libsm64 at commit `fd11813208272b4271d92bd92feb8f3fdbe61be5`.

Local changes for Mario Mode in Ship of Harkinian:

- `src/load_surfaces.c`: when a surface object is deleted, clear Mario's platform pointer if it pointed at that
  object, so the next tick doesn't read freed memory.
- `import-mario-geo.py`: can read Mario's geo/model from a local checkout of n64decomp/sm64 via `SM64_DECOMP_DIR`
  (otherwise downloads them from GitHub at build time, as upstream does).

Build: `make lib` (Linux), `make lib CC=x86_64-w64-mingw32-gcc OS=Windows_NT` (Windows DLL).
The library contains no game assets; it loads Mario's textures, animations and sounds from a user-supplied
Super Mario 64 (USA) ROM at runtime.
