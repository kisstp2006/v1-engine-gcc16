# v1 Engine – GCC 16 Linux Port

A [v1](https://github.com/r-lyeh/v1) C game framework Linux-compatible fork, fixed for **GCC 16+** compatibility.

## What's fixed (vs original)

This fork patches several compilation errors that appear when building with GCC 14+ / GCC 16 on Linux, where implicit function pointer type mismatches became hard errors:

| File | Issue | Fix |
|------|-------|-----|
| `engine/engine.h` + `engine/split/engine_obj.h` | `obj_method` macro calls `void(*)(void)` vtable with arguments | Cast to `void*(*)(void*,...)` |
| `engine/engine.c` + `engine/split/engine_file.c` | `strrchr()` returns `const char*`, written to without cast | `((char*)strrchr(...))` explicit cast |
| `engine/engine.c` + `engine/split/engine_netsync.c` | `found->function(args)` called on untyped pointer | Typed function pointer casts per RPC signature |
| `engine/engine.c` + `engine/split/engine_render.c` | `obj_ctor[OBJTYPE_light] = light_ctor` — type mismatch | `(void(*)(void))light_ctor` cast |
| `engine/engine.c` + `engine/split/engine_scene.c` | `camera/node/scene` vtable assignments — type mismatches | `(void(*)(void))` casts |
| `demos/09-*.c`, `demos/99-sprite.c` | `int main(int argc, char** argv)` incompatible with bootstrap macro | Changed to `int main()` |
| `MAKE.bat` | Missing `-Wno-implicit-int` for GCC 16 | Added to Linux args |

## Build (Linux)

```bash
# Install dependencies (Arch)
sudo pacman -S gcc libx11 mesa

# Build everything
bash MAKE.bat

# Or manually:
cc -o libengine.so engine/engine.c -shared -fPIC -O1 -lm -ldl -lpthread -lX11 -w -Wno-implicit-int -Iengine/
cc -o hello hello.c -O1 libengine.so -Wl,-rpath,./ -lm -ldl -lpthread -lX11 -w -Wno-implicit-int -Iengine/
./hello
```

## Tested on

- **OS:** Arch Linux
- **GPU:** NVIDIA GeForce RTX 2060 (driver 610.43.02)
- **GCC:** 16.1.1
- **OpenGL:** 3.2

## Original project

Based on [r-lyeh/v1](https://github.com/r-lyeh/v1) — a single-header C game framework.
