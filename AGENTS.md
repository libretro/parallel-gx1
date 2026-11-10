# AGENTS.md

Instructions for anyone, human or agent, changing this core. Read the
section that covers the code you touch before touching it.

## MSVC C89, gradually

The core is rewritten into C89 that MSVC compiles, a piece at a time, as
code is touched. No big-bang rewrite: each change leaves the code it
touches closer, and never adds C++ that was not there.

- **New files are C89.** `.c` files, test harnesses and tools included:
  declarations at the top of a block, `/* */` comments, no designated
  initializers, compound literals, VLAs or `//`.
- **Code touched in C++ files is written C89-shaped.** A struct and
  functions in place of a class, a switch on a kind in place of a
  virtual call, an explicit init and free in place of a constructor and
  destructor, no exceptions and no RTTI. Its codegen is checked to be
  faster than the C++ form, or at worst neutral.
- **No STL in the code a change touches.** `std::vector`,
  `std::function`, `std::array`, `std::string` and the rest go as the
  code around them is touched: fixed arrays where a bound is known, a
  pointer, count and capacity grown by doubling and kept where it is
  not, a function pointer and context in place of a callable, and a
  tagged record in place of a captured lambda. A list that is cleared
  keeps its storage, so play stops allocating once it has grown.
- **Frames stay small.** A function's stack frame is 4 KiB at most.
- **A file with no C++ left becomes a `.c` file.** Its header keeps
  `extern "C"` guards while C++ still includes it.

## Threads, waits and pacing

The model is parallel-ps2's: the frontend owns the pace, threads hand
work over through atomics and eventcounts, and nothing waits on a clock.

- **The frontend owns pacing.** One field per `retro_run`, run on the
  input polled in it. No wall clock decides emulated progress, how long
  a thread works, or when it stops: no time slices, no deadlines, no
  frame budgets. Background work that used to run every so many
  milliseconds counts frames (`Common/FrameClock.h`) instead.
- **One field per `retro_run`, through the frame gate.** The CPU thread
  parks at the end of each field until `retro_run` grants the next
  (`Core::RunField`), so between two runs it runs nothing; a field still
  running is never granted twice. A request that stops the CPU thread
  (pause, savestate, shutdown, a CPU-thread job) kicks it out of the gate
  (`FrameGateKick`), where it stops as a break at the field's end would,
  and owes the gate a wait before it runs on. Fields before the CPU
  thread reaches its run loop are boot work and are not gated. No
  per-frame pause and resume of the CPU thread.
- **No timed waits.** A thread waits for work, a field or a request on
  an eventcount, with no timeout. The one bound is the wedge guard: the
  frontend's thread waits at most `FIELD_WEDGE_US` (one second) with
  nothing at all arriving, so a wedged CPU or GPU thread degrades to
  repeated frames and not to a frozen frontend. A disc read, a compile
  or a slow field never reaches it, and it is never a pace.
- **Events where there is work.** An emulated-time event fires where
  something happens, not on every tick, line or half-line to count it:
  the VI event runs only on half-lines with work (a field's edges, the
  SI poll, an interrupt's line) and counts the ones between when it
  fires. Whatever reads or changes that state between events (a register
  read or write, a savestate) catches it up first, so it sees what an
  event per half-line would have left; a savestate leaves the event as
  the old layout had it, so states load in either build.
- **Stop by request, not by polling.** A thread is parked, interrupted
  or quit by setting a flag and notifying it (`InterruptGpuLoop`,
  `GpuThreadPark`), never by a loop that comes up for air every few
  milliseconds to look.
- **Park, never spin.** An idle thread parks on its eventcount with the
  two-check protocol: check, prepare the wait, check again, then commit
  or cancel. Busy-spinning a thread, or an emulated-time event whose job
  is to let a thread sleep, is a defect. Finite polling before parking
  is a latency policy, and needs numbers that show it pays.
- **No locks.** Work is handed over through atomics, SPSC rings and
  eventcounts. A mutex added to fix a race is the wrong fix: give the
  state one owner.

## No allocations mid-game

After boot, nothing the core does in play touches the heap, on any
thread: not `retro_run`, not the CPU or GPU thread, not the compile
workers. The gate's allocation pass holds this at 0 on the main thread
and 0 on the workers (`Tools/stutter/baseline.txt`), and a single new
allocation fails it.

- **Make it at boot.** Storage play needs is made before the first
  frame: sized for the worst case where the bound is known, or grown
  during boot. A thread that starts at boot makes what it will use
  then (`shader_arena_prepare`), not on its first piece of work.
- **Keep what is freed.** Objects made and destroyed in play come from
  a pool (`Common/ObjectPool.h`); shader text and blobs from the
  thread's stash (`VideoCommon/ShaderArena.c`); a list that is cleared
  keeps its storage.
- **Fixed bounds where the hardware gives one.** A per-frame list with a
  natural bound is a fixed array; past it, the frame stops recording
  rather than allocating (the CPU-access and command-buffer-kick
  lists).
- **Large tables are untouched pages.** Memory that is mostly zero and
  written sparsely comes from `Common::AllocateMemoryPages`, which the
  system zeroes lazily, not `calloc` or a value-initialized vector.
- **No hidden allocators.** `std::string`, `fmt::format`, iostream
  parsing, `std::function` captures and growing containers allocate.
  None of them run per frame, per draw or per field; a setting read in
  play is read where config changes, not with `Config::Get` per call.
- **Find it with the tracer.** `Tools/stutter/allocs.c` names each site
  after boot; deepen its stack to find the owner. A site that is
  "one-time" is still a stall at the moment it happens, so it goes too.

## Zero-copy and borrowed lends

Every change looks for copies to remove in the code it touches, and
every delivery says what it found and removed. A copy that stays has a
reason a reader can see.

- **One owner, the rest borrow.** Data is owned once. Everything else
  takes a pointer and a length for a bounded time, and the borrow ends
  before the owner frees or reuses the memory: a borrowed slice outlives
  every consumer, including the driver call it is passed to.
- **Producers write where the data ends up.** A decoder writes into the
  memory the backend lends for the upload (`AbstractTexture::LendLoad`),
  a driver into the store it will be written to disk from
  (`LinearDiskCache::SingleValue`), a generator into the buffer the
  driver reads. No intermediate buffer, no `std::vector` returned by
  value only to be copied somewhere else.
- **No slurps.** Files are mapped, or read in place through
  libretro-common's filestream/VFS. A file read whole into a fresh
  allocation, to be parsed or compared, is a bug when it could be
  mapped or streamed (`filestream_matches_buf` compares without one).
- **Copies that must stay say why.** Some copies are what keeps the
  emulation correct, and stay, with a comment naming the reason:
  - guest memory the GPU reads later is snapshotted when the GPU thread
    takes it, as the emulated CPU goes on writing it (a texture, a
    vertex buffer, a palette);
  - data a driver or an API hands over only for the length of a call.
- **Measure it.** Bytes copied and allocations made are work results:
  count them where they happen, and say what changed. They are not
  frame-time results until timing shows it.

## Savestates and libretro-common

- **A state an older build saved loads the same.** A value a state
  keeps that a change now computes differently (a texture cache hash,
  say) is taken in its old form too, on its first use after loading,
  and replaced with the new one; the old function stays for that.
  Check it with a state the parent saved, loaded by the candidate.
- **libretro-common comes from upstream as it is.** A file it lacks is
  copied in unchanged from libretro/libretro-common; its files are not
  edited here.
- **JSON is read with libretro-common's rjson.** Files are pulled through
  `rjson_open_user` from a VFS `filestream`, parsed as they stream, with
  no document tree and no wrapper library between rjson and its caller.

## Shader generators build by calls

- **A generator builds its stage through rslc's builder**
  (`VideoCommon/ShaderBuild.h`, `AbstractGfx::CreateShaderFromBuild`), not
  by writing GLSL for a backend to parse. The backend asks rslc for its own
  destination: SPIR-V for Vulkan, DXBC for D3D11, DXIL for D3D12, GLSL for
  OpenGL, MSL for Metal. Resource layouts come from the `sb_*` helpers,
  not from header macros.
- **A stage moved to the builder compiles to the same words.** Before its
  text generator is deleted, the built stage and the old text are compiled
  side by side for Vulkan and D3D and must match word for word.
- **What the device can do goes in the builder's flags**
  (`SHADER_BUILD_SUBGROUP` and the driver-bug flags), set by the backend
  as its text header used to define them.

## Shader compilation and stutter

The target is the console's own behaviour: a game never waits for a
shader. Starting a game for the first time, with no shader cache, must
play the same as a run with a full one. Nobody should be able to see a
shader stutter or a pause. Shader compilation gets better with every
commit, and never worse.

The plan this section enforces, phase by phase, is
`docs/ShaderStutterPlan.md`.

### Invariants

1. **Nothing compiles on the draw path.** After boot, in the default
   compilation mode, no shader, pipeline or program is created on the
   GPU thread, the CPU thread or inside `retro_run`, and nothing there
   waits for a compile. This covers GX pipelines, ubershader pipelines,
   EFB copy, texture reinterpretation, palette conversion and texture
   decoding pipelines, and the shared pipelines. A code path that adds
   one is a bug, even when it only runs the first time a state is seen.
2. **Every draw has a correct pipeline ready.** A draw with no
   specialized pipeline yet uses a pipeline that renders the same
   result: the ubershader, or a utility pipeline built from uniforms. It
   never uses a skipped draw, a placeholder or a guessed approximation.
   Before a key loses a field to a uniform or to dynamic state, show
   that the dynamic version renders the same.
3. **Workers never hold up a draw.** Compile queues are bounded. A full
   queue defers speculative work, and the draw goes ahead with its ready
   pipeline. Finished work is adopted on the thread that owns the cache.
   Shutdown stops the producers, cancels queued work, finishes active
   work and drains results before any device object is destroyed. Parking
   a worker on an eventcount is fine. Busy-spinning one, or waiting on the
   draw path for a free slot, is not.
4. **Shader cache off by default.** The cache is an option for
   low-end machines. It is never what removes a stutter. Invariants 1
   and 2 hold with it off. When it is on:
   - nothing reads or writes the disk on the GPU or CPU thread, or in
     `retro_run`;
   - recorded uids, shader binaries and pipeline driver data collect
     in memory and are written by a worker at shutdown or on pause;
   - cache files live in the frontend's system directory.
5. **Zero-copy in the shader path.** Each shader's words are owned
   once and handed to the driver without a copy.
   - Cache files are memory-mapped, or read in place through
     libretro-common's filestream/VFS. Validated slices go straight to the
     driver (`vkCreatePipelineCache` initial data, program binaries).
   - No slurps, no byte-by-byte copies, no intermediate buffers.
   - A borrowed slice must outlive every consumer, including the driver
     call it is passed to.
   - Shader text and IR are built in fixed or reused storage, with no
     formatted temporaries and no heap allocation during play.
6. **Stable identity.** Keys are uids compared byte for byte:
   - zero the padding before hashing;
   - clear the bits a shader does not read (`ClearUnused*`);
   - include the backend, the target (SPIR-V version, DXBC/DXIL, GLSL
     dialect) and the host config.
   Never key on a pointer or an address. Changing what a key means, even
   when its size stays the same, bumps the cache version in the same
   commit. A corrupt or mismatched cache file is a recoverable miss.

### The gate: every commit, measured

Any commit that touches `Source/Core/VideoCommon`,
`Source/Core/VideoBackends`, the shader generators, rslc, or the cache
and file I/O behind them runs the stutter harness (`Tools/stutter/`) by
hand before delivery. It is not wired into CI. Report the results in the
delivery summary, not in the commit message.

- **Corpus.** Synthetic FIFO logs (`.dff`) generated by the harness to
  cover the pipeline state space: TEV stages, blending and logic ops,
  depth, culling, vertex formats, texgens, EFB copy formats, texture
  formats and palettes. No game names and no disc images, in files,
  comments or messages.
- **Runs.** Each backend that builds on the machine (Vulkan, GL, D3D11,
  D3D12), in four cache states, kept apart deliberately:
  1. cold: no cache at all;
  2. uid recipes only;
  3. everything warm;
  4. loading a savestate into a fresh process.
- **Hard gates.** These don't depend on the machine, so they must not
  get worse:
  - draw-path compiles after boot: must stay 0 in the default mode, by
    kind;
  - draw-path compile waits: must stay 0;
  - distinct pipelines and shaders built per corpus;
  - total emitted shader words and instructions per backend target;
  - heap allocations during play attributed to the core (allocation
    tracer);
  - pictures identical to the reference, with no validation errors.
- **Timed gates.** These compare the commit with its parent on the same
  machine:
  - boot-to-first-frame time;
  - ubershader precompile time;
  - median and maximum per-shader generate+emit time;
  - worker pipeline creation time;
  - `retro_run` p50/p99/max and the count of runs over the frame budget.

  A regression beyond the run-to-run spread blocks the commit.
- **Cold must match warm.** The cold run's p99 and max `retro_run` stay
  within the measured spread of the warm run's. A gap is a stutter to
  fix.
- **Ratchet.** `Tools/stutter/baseline.txt` holds the current numbers.
  A commit that improves a number lowers it in the same commit. Nothing
  ever raises it. A deliberate, justified exception needs its own commit
  and the user's approval.

### Measuring honestly

- **Clean runs.** Time on an idle machine, with nothing else building,
  and alternate parent and candidate (ABBA). Timing runs take no
  screenshots, picture hashes, readbacks or GPU query polling; picture
  checks are separate runs. Report medians, p99 and max with frame
  numbers, never a single run.
- **Explain hitches with counters.** Don't diagnose a hitch by how it
  looks. A long frame is a compile only when the compile counters put
  one at that frame. Readbacks, guest CPU work and I/O look the same.
- **Counters cost nothing in release builds.** They exist only in the
  stats build (`make STATS=1`). Each is one atomic add where the event
  happens, read by the harness. No logging, no wall clocks, no extra work.
  Cycle counts use `cpu_features_get_perf_counter()`.
- **Count useful work.** Count new pictures and emulated progress too.
  A run that repeats frames or skips guest work only looks faster.
- **Keep work and time apart.** Fewer compiles, fewer bytes copied and
  fewer allocations are work results. They are not frame-time results
  until timing shows it.

### Rendering cost of what rslc emits

glslang and SPIRV-Cross are gone; rslc emits SPIR-V, DXBC and DXIL
directly. A report that a shader got slower on the GPU is a top-priority
regression. To fix it:

1. Reproduce it in the corpus.
2. Compare the emitted code with what the old toolchain produced.
3. Bring the missing optimization into rslc.
4. Keep a test that pins the result.

Static metrics (words, instructions, branches, samples) are tracked per
corpus. They show the work was removed, not the GPU time.
