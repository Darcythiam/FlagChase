# FlagChase — Concurrent 2D Agent Simulation

<p align="center">
  <strong>C++17 concurrency, shared-state synchronization, benchmarking, and spatial occupancy optimization</strong>
</p>

<p align="center">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
  <img alt="Linux" src="https://img.shields.io/badge/Linux-tested-FCC624?logo=linux&logoColor=black">
  <img alt="CMake" src="https://img.shields.io/badge/CMake-3.15%2B-064F8C?logo=cmake&logoColor=white">
  <img alt="ThreadSanitizer" src="https://img.shields.io/badge/ThreadSanitizer-supported-success">
  <img alt="ASan + UBSan" src="https://img.shields.io/badge/ASan%20%2B%20UBSan-supported-success">
</p>

## Overview

**FlagChase** is a multithreaded 2D agent simulation written in C++17. The Looney Tunes-inspired flag-chase rules are the demonstration workload; the engineering focus is the concurrent simulation itself: one worker thread per agent, mutex-protected shared state, agent interactions, collision/occupancy checks, reproducible benchmarking, and performance optimization.

The current implementation uses a coarse-grained global board lock for correctness. A measured scalability bottleneck in the original occupancy check was then addressed by replacing an **O(N) scan across all agents** with a **direct spatial occupancy index** that maps each board cell to its current occupant.

### Highlights

- **Thread-per-agent execution** using `std::thread`
- **RAII-based synchronization** around shared simulation state
- Real agent behaviors: movement, jumping, burst movement, shooting, freezing, and cooldowns
- Debugged a real shared-state race involving an unsynchronized timer read
- **O(1) exact-cell occupancy lookup** using a discrete spatial index
- Reproducible benchmark and sweep modes with CSV output
- Streaming latency statistics using Welford aggregation
- Optional mutex wait-time instrumentation
- Dedicated **ThreadSanitizer** and **AddressSanitizer + UBSan** build modes
- Interactive ASCII rendering plus an optional **local browser dashboard**
- Live agent-state inspection, pause/resume, speed control, wall editing, and rolling telemetry
- Headless execution for benchmarks so UI work never contaminates performance measurements

---

## Architecture

```text
                             main()
                               │
                    parse args / print config
                               │
          ┌────────────────────┼────────────────────┐
          │                    │                    │
     interactive           benchmark              sweep
          │                    │                    │
          └────────────────────┴────────────────────┘
                               │
                          runOneGame()
                               │
                         shared Board
                               │
            ┌──────────────────┼──────────────────┐
            │                  │                  │
        board.mtx         ASCII output       optional --ui
            │                                     │
   shared simulation state              read-only snapshots
            │                                     │
   ┌────────┼────────┐                    localhost HTTP
   │        │        │                           │
thread 0 thread 1 thread N               browser dashboard
 agent 0   agent 1   agent N                     │
                                             commands
                                                 │
                                      thread-safe command queue
                                                 │
                                      main control loop applies
                                      commands under BoardLock
```

Every active agent owns an OS thread. Those workers interact through a single shared `Board` instance containing positions, terrain, timers, cooldowns, step counters, render state, and the spatial occupancy index.

The current synchronization model intentionally favors **simple correctness** over maximum parallelism: mutable board state is accessed under `board.mtx`. This means the project has a clear tradeoff to analyze — many agent threads execute independently, but updates to shared board state serialize through one global critical section.

---

## Concurrency Model

### Shared state

The `Board` owns the primary mutable simulation state:

```text
toonPos[]        current agent positions
occupant[]       direct board-cell → agent index
frozen_until[]   per-agent freeze deadline
next_shot_ok[]   per-agent shooting cooldown
steps[]          per-agent movement count
cell[][]         static terrain / walls
grid[]           ASCII render buffer
```

`BoardLock` wraps `board.mtx` using RAII so lock release is tied to scope. `RenderLock` separately serializes event output.

### Lock ordering

The code enforces one lock-order rule:

```text
board.mtx  →  render_mtx
```

A thread-local assertion catches attempts to acquire the board lock while already holding the render lock in debug builds, reducing the chance of introducing an inverted lock-order deadlock later.

### Race-condition debugging

During development, a genuine race was found in the freeze-timer path: one code path read `frozen_until[t]` without synchronization while another path could update that same value while holding the board mutex.

The fix was to move the read under the same synchronization discipline as the write.

The important lesson is simple:

> Protecting writes alone is not sufficient when concurrent reads can race with those writes.

---

## Agent Behaviors

The three behavior roles repeat when the simulation is run with more than three agents:

| Role | Behavior |
|---|---|
| **RoadRunner** | Faster pacing with occasional burst movement |
| **Coyote** | Can jump over a blocked cell |
| **Yosemite Sam** | Can target another agent, shoot, and temporarily freeze it subject to a cooldown |

All behaviors operate against the same shared board state, making them useful for exercising synchronization under different access patterns.

---

## Board-Centric Browser Dashboard

FlagChase includes an optional **board-centric engineering dashboard** served directly by the C++ process on **localhost**. The simulation remains a discrete 2D grid: walls, the goal, and agents are rendered directly from live simulation snapshots, while controls and debugging information stay outside the board. The frontend is embedded HTML/CSS/JavaScript served by a lightweight C++ HTTP server, so there is no Node.js, React, or external GUI framework required.

Run it with:

```bash
./build/toons \
  --rows 60 \
  --cols 120 \
  --toons 120 \
  --seed 12345 \
  --delay-ms 60 \
  --max-steps 100000 \
  --ui
```

Then open:

```text
http://127.0.0.1:8080
```

Use a different port if needed:

```bash
./build/toons --ui --ui-port 9090
```

### Dashboard features

- real-time **grid/board view** of agents, walls, and the goal
- optional cell-grid and **spatial-index overlays** for inspecting the O(1) occupancy mapping
- pause and resume controls
- live simulation-speed control
- zoom and pan
- click-to-select agent inspection
- searchable agent ID selection
- live operational state: position, step count, frozen state, remaining freeze time, and Yosemite Sam shooting cooldown/readiness
- freeze / unfreeze controls for the selected agent
- wall add/remove edit mode
- direct spatial-index information for the selected cell
- rolling throughput graph
- live frozen-agent count graph
- final-state inspection after a natural winner is declared

The live state panel intentionally derives its status from the simulation's existing data rather than adding a cosmetic state machine. For example, `FROZEN` is derived from `frozen_until`, while shooting readiness is treated as a capability of the Yosemite Sam role rather than incorrectly turning the entire agent into a global `COOLDOWN` state.

### UI concurrency boundary

The browser does **not** receive a pointer to simulation state and does not mutate `Board` from the HTTP thread.

Read path:

```text
Browser poll
    ↓
HTTP server thread
    ↓
brief BoardLock
    ↓
copy agent/layout snapshot
    ↓
unlock
    ↓
serialize JSON / render in browser
```

Write/control path:

```text
Browser control
    ↓
HTTP server thread
    ↓
thread-safe command queue
    ↓
main simulation control loop
    ↓
BoardLock when board mutation is required
```

That boundary matters for the hardware/device-control style of reasoning behind the live-state feature: inspect current state and validate the operation before applying it, instead of letting presentation code modify shared concurrent state directly.

### Large-agent runs

The dashboard reports statistics for the full population but caps the number of serialized/rendered agent markers to avoid sending enormous JSON frames. The default is 5,000 agents:

```bash
./build/toons --ui --ui-max-agents 2000
```

High-scale throughput claims should still be measured through `--benchmark` / `--sweep`, not through the dashboard. Those modes automatically disable the UI and ASCII rendering.

---

## Spatial Occupancy Optimization

### Original approach

The original occupancy test scanned every agent whenever a worker wanted to determine whether a destination cell was occupied:

```cpp
for (size_t k = 0; k < board.toonPos.size(); ++k) {
    if (static_cast<int>(k) != self &&
        board.toonPos[k].r == r &&
        board.toonPos[k].c == c) {
        return true;
    }
}
```

That made each lookup **O(N)** in the number of agents.

Because the lookup happened while the global board mutex was held, increasing agent count had two effects at once:

```text
more agents
   ↓
longer occupancy scan
   ↓
longer critical section
   ↓
other workers wait longer for board.mtx
```

### Current approach

The board now maintains a flat occupancy index:

```cpp
vector<int> occupant;   // -1 = empty, otherwise agent id
```

A board coordinate maps directly to one slot:

```cpp
index = row * board.C + col;
```

Occupancy checking becomes a direct lookup:

```cpp
int who = board.occupant[index];
return who != -1 && who != self;
```

Since this simulation already uses a discrete grid and allows at most one agent per cell, a one-cell-to-one-occupant index is simpler and more precise than a generalized bucket structure.

### Complexity

| Operation | Before | Current |
|---|---:|---:|
| Exact-cell occupancy lookup | **O(N)** | **O(1)** |
| Position/index update | O(1) | **O(1)** |
| Additional memory | none | **O(rows × cols)** |

`move_toon_locked()` updates `toonPos` and the occupancy index together while the existing board lock is held. Debug assertions verify that both representations agree before each move.

---

## Performance

Benchmarks use the same `runOneGame()` simulation core as normal execution. Benchmark mode disables rendering and artificial delays, uses a fixed tick budget, runs repeated trials, and reports throughput plus inter-tick latency.

### Original O(N) occupancy baseline

400 × 1000 board, 10-trial sweep:

| Agents | Throughput | Latency |
|---:|---:|---:|
| 10 | 2457.6 ± 57.9 ticks/s | 407.1 ± 53.2 µs |
| 100 | 2469.2 ± 22.0 ticks/s | 405.0 ± 37.8 µs |
| 1,000 | 2434.6 ± 40.5 ticks/s | 410.8 ± 51.5 µs |
| 10,000 | 2193.3 ± 80.1 ticks/s | 456.5 ± 92.6 µs |
| 50,000 | **1676.8 ± 31.9 ticks/s** | **596.6 ± 103.7 µs** |

### After spatial indexing

At 50,000 agents, repeated measurements reached approximately:

- **2,200–2,500 ticks/sec**
- **400–455 µs latency**
- **31–49% higher throughput** than the original O(N) implementation

Low-agent-count changes were small, which is consistent with the optimization: the linear scan was inexpensive when `N` was small and became increasingly important at scale.

> Benchmark claims should always be reproduced on the same machine, build type, board dimensions, seed policy, and trial configuration before comparison.

---

## Benchmarking Methodology

The benchmark path is intentionally separate from interactive presentation concerns:

- rendering forced **off**
- `delay_ms` forced to **0**
- fixed tick budget via `--max-steps`
- multiple independent trials
- one seed per trial derived from the configured base seed
- throughput measured over a bounded trial window
- inter-tick latency accumulated using Welford's online algorithm
- aggregate results printed to stdout
- sweep results written to CSV

Example sweep:

```bash
./build/toons \
  --rows 400 \
  --cols 1000 \
  --seed 12345 \
  --max-steps 10000 \
  --benchmark-trials 10 \
  --sweep \
  --sweep-counts 10,100,1000,10000,50000 \
  --csv-out bench_spatial_index.csv \
  --no-render
```

---

## Build

### Requirements

- C++17-compatible compiler
- CMake 3.15+
- Linux or another environment with C++ threading support

### Release build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run the simulation:

```bash
./build/toons
```

Example custom run:

```bash
./build/toons \
  --rows 30 \
  --cols 80 \
  --toons 12 \
  --seed 12345 \
  --max-steps 5000
```

---

## Sanitizers

Sanitizer builds are intentionally separate from performance builds because instrumentation changes runtime characteristics.

### ThreadSanitizer

```bash
cmake -S . -B build-tsan \
  -DSANITIZER=thread \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build-tsan -j"$(nproc)"
```

Example stress run:

```bash
./build-tsan/toons \
  --rows 400 \
  --cols 1000 \
  --toons 1000 \
  --benchmark \
  --benchmark-trials 5 \
  --max-steps 10000 \
  --seed 12345 \
  --no-render
```

### AddressSanitizer + UBSan

```bash
cmake -S . -B build-asan \
  -DSANITIZER=address \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build-asan -j"$(nproc)"
```

```bash
./build-asan/toons \
  --rows 400 \
  --cols 1000 \
  --toons 1000 \
  --benchmark \
  --benchmark-trials 5 \
  --max-steps 10000 \
  --seed 12345 \
  --no-render
```

---

## Lock Instrumentation

The project can optionally measure time spent waiting for `board.mtx`.

Build separately so timing instrumentation does not contaminate the primary throughput measurements:

```bash
cmake -S . -B build-instrumented \
  -DLOCK_INSTRUMENTATION=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-instrumented -j"$(nproc)"
```

This mode reports the average percentage of each worker thread's lifetime spent waiting to acquire the global board mutex.

---

## Useful CLI Options

```text
--rows N
--cols N
--toons N
--max-steps N
--seed N
--delay-ms N
--shoot-chance X
--shoot-cooldown N
--freeze-ms N
--jump-chance X
--no-render
--ui
--ui-port N
--ui-max-agents N
--benchmark
--benchmark-trials N
--sweep
--sweep-counts LIST
--csv-out PATH
```

See all options:

```bash
./build/toons --help
```

---

## Current Limitations

The current implementation intentionally keeps several design constraints visible rather than hiding them:

- A **single global board mutex** serializes mutable board-state access.
- The simulation uses **one OS thread per agent**, so very large agent counts can oversubscribe the machine.
- Interactive ASCII rendering remains synchronous and is not designed for high-scale runs.
- The browser dashboard currently targets Linux and binds only to `127.0.0.1`.
- Very large populations are intentionally capped in the visualization payload even though simulation statistics still reflect the full population.
- The simulation core is still concentrated in `src/main.cpp`; the dashboard server is separated into `src/web_ui.cpp` / `src/web_ui.hpp`, but broader engine/domain modularization is future work.
- The occupancy index optimizes collision lookup, but it does not remove the global-lock architecture itself.

These constraints are useful because they make the tradeoffs measurable and provide clear directions for future work.

---

## Roadmap

Planned improvements are intentionally kept separate from the benchmarked core so each architectural change can be measured independently.

### Dashboard follow-ups

The browser dashboard is implemented. Potential incremental improvements include:

- restart/reset without relaunching the process
- spawning/removing agents at runtime
- richer action-history/event tracing
- optional snapshots suitable for recording/replay
- a dedicated live latency metric if it can be collected without contaminating benchmark behavior

### Additional architectural experiments

Potential future work includes:

- separating simulation, rendering, and benchmark code into modules
- reducing the scope of the global critical section
- comparing alternative scheduling models after establishing reproducible baselines

Those are deliberately future experiments rather than claims about the current implementation.

---

## Why This Project Exists

The project started as a small concurrent simulation and became an exercise in systems reasoning:

```text
build concurrent behavior
        ↓
find a real synchronization bug
        ↓
make shared-state access consistent
        ↓
measure scaling behavior
        ↓
identify an O(N) hot path inside the critical section
        ↓
change the data representation
        ↓
rerun the same benchmark
        ↓
quantify the improvement
```

The main goal is not the game itself. It is understanding how correctness, synchronization, algorithms, and measurement interact in a concurrent C++ program.
