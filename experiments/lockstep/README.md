# The lockstep experiment

**Purpose:** how to build and run the debugger-lockstep experiment. **Status:** procedure, for a
throwaway experiment; the questions it answers and its results are in
[docs/debugger-lockstep.md](../../docs/debugger-lockstep.md). **Read when:** running it.

Two processes: `engine` sends a frame (a random-walk price) over a Unix socket, then calls
`fillscope_frame_anchor`; `viewer` draws the frames with Dear ImGui and counts its own paints.

## Build
```sh
cmake -S experiments/lockstep -B build/lockstep -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/lockstep
```
Needs GCC, CMake, Ninja, `glfw-devel` and `mesa-libGL-devel`; Dear ImGui is fetched at a pinned tag.

## Run without a debugger
Start `build/lockstep/viewer`, then `build/lockstep/engine`. The price line grows five frames a
second.

## Run in VS Code (CodeLLDB)
Run and Debug, then one of the two compounds (each builds first):
- **viewer + engine (self-stop):** the anchor executes a breakpoint instruction when a debugger is
  attached.
- **viewer + engine (breakpoint by name):** LLDB sets a breakpoint on the anchor at launch.

The engine stops at frame 0. Each Continue (F5) should add one point to the viewer's price line;
F10 from the anchor steps back into the loop, and the point appears when `send` runs. "Viewer
paints" should keep rising while the engine is stopped.
