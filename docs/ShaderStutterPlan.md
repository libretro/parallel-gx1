# Shader compilation and stutter: the plan

Goal: starting a game for the first time, with no shader cache, plays
the same as a run with a full one. No shader stutters, no pauses.
Compilation gets cheaper commit by commit. The standing rules and the
per-commit gate are in `AGENTS.md`. This file lays out the work in
phases. Each phase is done when its gate numbers say so, not when the
code lands.

## Where the core stands

- **Compilation mode.** The default is Async (UberShaders). A draw
  without its specialized pipeline uses an ubershader pipeline, which
  renders the same result, while the specialized one builds on worker
  threads. The workers are retro_atomic based and adopt results on the
  GPU thread.
- **Ubershader pipelines.** Those recorded in the uid cache are built
  before the game starts. The rest of the enumerable set is queued in
  the background, with a placeholder vertex format and default
  raster/depth/blend state.
- **Shader front end.** rslc emits SPIR-V, DXBC and DXIL directly, with
  no glslang and no SPIRV-Cross. The generators still format GLSL-like
  text for rslc to parse.
- **Disk caches** (uid recipes, shader binaries, the driver pipeline
  blob) are on by default and have no core option.

### Draw-path compiles still left

- **Ubershader misses.** `ShaderCache::GetUberPipelineForUid` creates
  the pipeline synchronously when a draw's vertex format,
  raster/depth/blend state or primitive type has no built ubershader
  pipeline. With the cache off, this is the main first-play stutter.
- **Lazy utility pipelines.** `GetEFBCopyToVRAMPipeline`,
  `GetEFBCopyToRAMPipeline`, `GetTextureReinterpretPipeline` and
  `GetTextureDecodingShader` compile shader and pipeline on first use.
- **Synchronous modes.** Synchronous and Sync (UberShaders) compile at
  the draw by design. They stay as options and are not the default.
- **Disk writes.** New uids already collect in memory until close. But
  with the cache on, each new shader binary and each pipeline's driver
  data is appended to its cache file on the GPU thread as it is inserted
  (`LinearDiskCache::Append`).

### Measurement in the repo

None yet. The harnesses used so far (vkh, glh, dxh, swh, the allocation
tracer) are local.

## Phase 0: measure first (lands before any optimization)

1. **Stats build.** `make STATS=1` adds counters, each one atomic add
   at the event. In release builds they compile to nothing.
   - Draw-path compiles, by kind: GX specialized, ubershader, EFB copy
     to VRAM, EFB copy to RAM, reinterpret, palette, texture decode,
     shared.
   - Draw-path compile waits.
   - Worker compiles and their cycle counts, split into generate, rslc
     emit and driver pipeline creation.
   - Queue-full deferrals, and adoptions on the GPU thread.
   - Disk reads and writes from the GPU and CPU threads.

   Cycle counts use `cpu_features_get_perf_counter()`. No wall clocks,
   no logging.
2. **`Tools/stutter/`.** A C89 harness that builds on Linux and on
   Windows (MSYS2 MINGW64). It loads the core through the libretro API
   for each backend (Vulkan, GL, D3D11, D3D12 under wine on Linux) and
   reads the counters through a stats-build export. It times
   `retro_run` on the harness side and writes one results line per run.
3. **Synthetic corpus generator.** It enumerates the pipeline state
   space into `.dff` FIFO logs:
   - TEV stage counts and ops, alpha test and z-texture;
   - blend, logic op and dual-source modes;
   - depth and cull modes;
   - each vertex attribute format and size, and texgen counts;
   - line and point expansion;
   - every EFB copy format and filter, every texture format and
     palette, and reinterpretations.

   No game data.
4. **`run.sh`.** Runs the four cache states (cold, recipes only, warm,
   savestate into a fresh process) on each backend. It runs parent and
   candidate ABBA, compares picture hashes in separate runs, and runs
   the allocation tracer.
5. **`Tools/stutter/baseline.txt`.** The first numbers. From here on the
   ratchet in AGENTS.md applies.

Done when: the harness reproduces today's numbers on two runs within
the spread, and lists every draw-path compile by kind.

Landed so far: the stats build (draw-path compiles by kind, waits,
draw-path disk writes, worker compiles, shaders and pipelines made),
the harness on Vulkan and the software renderer, a corpus of blend,
depth, cull, vertex format, primitive, TEV and alpha test states, and
the Vulkan baseline. Still to come: EFB copy, texture, palette and
texgen states in the corpus, the GL and D3D backends in the harness,
and the allocation tracer. Counters are gated over the whole run:
where boot ends races the GPU thread, so the boot/play split is
reported, not gated.

The first Vulkan numbers, cold: 175 ubershader pipelines and 2 EFB
copy pipelines built on the draw path, 1 wait for the workers, 91
cache writes from the draw path; 115 shaders and 1017 pipelines made.
Recipes and warm remove the ubershader builds; nothing removes the EFB
copy builds.

## Phase 1: zero draw-path compiles in the default mode

1. **Ubershader coverage through dynamic state.** Take every field the
   driver can set at draw time out of the ubershader pipeline key, and
   enumerate what remains in full at boot on the workers.
   - **Vulkan:** cull mode, front face, depth test/write/compare,
     primitive topology (extended dynamic state); blend enable, factors,
     equation and color write mask (extended dynamic state 3); vertex
     input (vertex input dynamic state). Graphics pipeline libraries
     (`VK_EXT_graphics_pipeline_library`) pre-build the vertex-input,
     pre-raster, fragment and output parts, so linking a new combination
     costs microseconds.
   - **D3D12 and D3D11:** take out what the API allows, and enumerate the
     rest.
   - **GL:** the program does not include the fixed-function state, so
     only program identity counts. Separable programs and
     `ARB_parallel_shader_compile` completion polling keep the link off
     the draw.
   - **Where no extension is available:** enumerate the static uber set
     the corpus reaches. Record its size and build time as gate numbers.
     The miss stays correct but becomes a counted bug.
2. **Utility pipelines built ahead.** The EFB copy (TCShaderUid and the
   RAM copy key), reinterpret and decode key spaces are finite. Build
   them all at boot on the workers. Where a key space is too large,
   replace it with one pipeline driven by uniforms, after showing it
   renders the same result.
3. **Specialized pipelines.** These are already asynchronous.
   - Bound the queues: a full queue defers, never blocks.
   - The ubershader draws until adoption.
   - Count misses by reason: no sibling, queue full, or adoption.
4. **Dual-source blend, logic ops and integer output.** Cover the
   ubershader variants these need, so a blend mode change never meets a
   missing pipeline.

Landed so far: on Vulkan the logic op is set per draw, and the
ubershader precompile covers logic ops drawn with dual-source output
asked for. Cold, of the 175 ubershader pipelines a draw builds, 172
reuse a VkPipeline already compiled (the `draw_driver_pipeline` counter
holds the rest): what is left is a draw reaching a pipeline before the
workers have, with boot not waiting for them.

Done when: draw-path compiles and waits are 0 by kind, cold, on every
backend that builds, across the whole corpus. Cold p99 and max
`retro_run` are within the spread of warm.

## Phase 2: cache off by default, no disk I/O on the draw path

1. **Core option.** Add "Shader Cache", default disabled. Its
   description explains that low-end machines can still use it.
2. **Cache writes off the draw thread.** Shader binaries and pipeline
   driver data join the uids in a bounded in-memory arena. A worker
   writes them with nonblocking filestream I/O at shutdown or on pause.
   Nothing touches the disk on the GPU or CPU thread.
3. **Cache reads.**
   - Map the files, or borrow them through VFS.
   - Validate the header, version, sizes and alignment before borrowing
     slices.
   - Pass the driver blob straight into `vkCreatePipelineCache` initial
     data. Shader binaries go to the driver from the mapping.
   - No slurp buffers. A fallback that must read keeps one reused
     buffer.
4. **Version policy.** Any change to what a key means bumps the cache
   version. A mismatch is a recoverable miss.

Done when: with the cache on or off, the counters show 0 disk
operations on the draw path. Cold and warm runs are within the spread
of each other.

## Phase 3: cheaper compiles, commit by commit

1. **Generators in C89, emitting rslc IR directly.** No text
   formatting, no parsing. Use fixed or reused storage, with no heap
   allocation for a shader whose size was seen before.
2. **Fewer permutations.** Canonicalize uids, clearing every bit a
   shader does not read and moving state that can be dynamic out of the
   key. Distinct pipelines per corpus is a ratcheted gate: reducing the
   state space beats compiling it faster.
3. **rslc speed and output.**
   - Track generate+emit cycles per shader, and words and instructions
     per backend target.
   - When a shader is slower on the GPU than with the old toolchain,
     reproduce it in the corpus, compare against what glslang emitted,
     bring the optimization into rslc, and pin it with a test.
4. **Driver work.** Track worker pipeline creation cycles. Smaller,
   simpler modules and GPL links are where driver time goes down.

Done when: never. Each commit holds or improves every number.

## Phase 4: zero-copy through the shader path

- **Words.** Each shader's words are owned once and handed to the
  driver without a copy. No reflection pass: layouts are fixed by the
  generator.
- **Uniforms.** Compare and write in place in the destination block.
  Upload only changed ranges. Never overwrite a snapshot the GPU may
  still read.
- **Ownership.** Prove each borrowed slice's lifetime through the last
  CPU and GPU consumer, and list it in the commit summary.
- **Gate.** Core heap allocations during play stay at 0 through the
  shader path.

## Order

Phases run in order. Phase 1 starts once Phase 0's baseline exists.
Phase 3 and Phase 4 work can run alongside Phases 1 and 2. Every one of
those commits passes the gate.
