// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Always-on, nearly free time accounting for the PS3 port (PPU time base, 79.8 MHz on a PS3):
// how much of a frame goes to the GE (display lists) and to vertex transform.
#pragma once
#include <cstdint>
#include "ppsspp_config.h"

#if PPSSPP_ARCH(POWERPC)
namespace PS3Profile {
extern uint64_t ge;         // Display list processing (includes transform and drawing).
extern uint64_t transform;  // Software vertex transform + clipping + submitting primitives.
extern uint64_t decode;     // Vertex decoding (part of transform).
extern uint64_t batch;      // Batched path: transform, lighting, culling (part of transform, excludes the RSX submit).
extern uint64_t directTL;   // Draws taken by the direct T&L path (count).
// 3D draws the direct T&L path didn't take: lines/points/rectangles, generated UVs or powered
// diffuse, 32-bit indices, other (clear mode, immediate draws, continued strips).
extern uint64_t tlMiss[4];
extern uint64_t tlMerged;
extern uint64_t tlMergeFail[16];  // Merge attempts that failed, by reason (TransformUnit.cpp MergeTLJob).
// MergeTLJob, by part (build 255 diagnostics): 0 kind/format checks, 1 index range, 2 state
// comparison, 3 transform parameters, 4 appending; attempts made.
extern uint64_t tlMergeT[5];
extern uint64_t tlMergeTries;
extern uint64_t tlMergeFast;      // Merges with no GE state change since the job (no comparison).
extern uint64_t gePrim, geReaderFlush, geSyncT, geXfer;  // Emulation-thread GE parts (time).
extern uint64_t geReaderFlushes, geSyncs, geXfers, gePrims;     // (counts)
// Parts of the draw commands (gePrim): state (binner UpdateState), transform state, the direct T&L
// path (of which merging into the open job and preparing a new one), the CPU paths.
extern uint64_t directRects;  // Draws of 3D rectangles on the direct path (count).
extern uint64_t geState, geXformState, geDirect, geMerge, gePrep, geCpuPath, geCpuPrims;
// Draw commands added to the open T&L job without any state work (FastMergePrim, with marking the
// framebuffer dirty), and ones a skipped frame only steps over: time and count (Frontend 131).
extern uint64_t geFastMerge, geFastMerges, geSkipT, geSkips;
extern uint64_t geStPixel, geStSampler, geStCount, geXfLight;  // Parts of geState/geXformState (Frontend 135).
extern uint64_t geCmds, geCmdsSkipped;
extern uint64_t geLoop;
// RSX: the CPU waiting for the RSX (command buffer space; fences: time, count); GE worker jobs that
// took over 0.5 ms (count, time) by job type.
extern uint64_t rsxCbWait, rsxFenceWait, rsxFenceWaits;
extern uint64_t xfMemoHits, xfMemoMisses;  // Transform state: matrix products reused / computed.
extern uint64_t geSlowJobs[16], geSlowJobTicks[16];  // SoftGPU's command loop (with its handlers).
// Parts of a new T&L job on the emulation thread (PrepareTLJob and around it): index scan, vertex
// copy (time, bytes), state capture, parameters, strip to list, render target note.
extern uint64_t tlScan, tlCopy, tlCopyBytes, tlCapture, tlParams, tlExpand, tlNote, tlSetup;
// GE commands SoftGPU's loop ran a handler for: calls and time (1 in 16 timed, x16) by command.
extern uint64_t geExecTicks[256], geExecCount[256];
extern uint32_t geExecSample;
extern uint64_t jitCodeSpaceLeft;  // PPC JIT: native code space left at the last compile.  // GE commands run in SoftGPU's loop, in drawn / skipped frames.
extern uint64_t geWorker;    // Time the GE worker thread spent drawing.
extern uint64_t geWait;      // Time the emulation thread waited for the GE worker.
extern uint64_t geDrains, geWaitFull;  // Waits for the whole queue / for a free slot (counts).
extern uint64_t sasMix;     // sceSas voice mixing (any thread).
extern uint64_t jitCompile; // IR + native compile of MIPS blocks.
extern uint64_t jitFrontend, jitNative;  // Parts of jitCompile: MIPS to optimized IR, IR to PPC code (with linking).
extern uint64_t jitBlocks, jitInvalidated, jitClears;
// Parts of the above: the IR optimization passes, linking (incl. making code visible).
extern uint64_t jitPasses, jitLink;
extern uint64_t jitCodeGen, jitCodeLoop;  // Frontend 110: parts of jitNative (CompileBlock, its instruction loop).
// Blocks run in the IR interpreter before being compiled (runs, time) and blocks compiled after that.
extern uint64_t jitInterpRuns, jitInterp, jitPromoted;
// Blocks compiled again at an address compiled before (since the last cache clear).
extern uint64_t jitRecompiles;
// Blocks of rewritten code brought back unchanged instead of compiled again (build 188).
extern uint64_t jitRevived;
extern volatile uint32_t jitLastRecompilePC;
// While set, newly compiled PPC blocks count their entries in jitBlockCounts[block number]
// (PS3 frontend "jitprofile": one sample of a few seconds, with the JIT cache cleared around it).
extern bool jitBlockProfile;
constexpr int JIT_BLOCK_COUNTS = 65536;
extern uint32_t jitBlockCounts[JIT_BLOCK_COUNTS];
// Time base ticks from each block's entry to the next block entry, and the generated code size.
extern uint32_t jitBlockTicks[JIT_BLOCK_COUNTS];
extern uint16_t jitBlockNativeBytes[JIT_BLOCK_COUNTS];
extern uint32_t jitBlockNativeOffset[JIT_BLOCK_COUNTS];  // From the start of the code area.
struct JitProfState { uint64_t lastTb; uint32_t lastBlockX4; uint32_t pad; };
extern JitProfState jitProfState;
extern uint64_t audioDecode; // ATRAC3/ATRAC3+/MP3/AAC frame decoding.
// Async GE study (build 145): emulation-thread time, not in the GE, from display list processing
// to the next point the game learns the GE is done (GeWorkerSync): CPU work that could run while
// another thread runs the lists. geOverlapFirst/geOverlapGeAtFirst: the open window (0: none).
extern uint64_t geOverlapWin, geOverlapCount, geOverlapFirst, geOverlapGeAtFirst;
void NoteGeProcessed(uint64_t geScopeStart);
void NoteGeBarrier();
// GE lists on their own thread: the emulation thread waiting for it (time, count).
extern uint64_t geAsyncWait, geAsyncWaits;
// ... and only until it got as far in emulated time as the game (CoreTiming slice ends).
extern uint64_t geAsyncGate, geAsyncGates;
extern uint64_t realTimeWait; // Emulation thread sleeping to keep the game at real-time speed (speed limit).
// What the emulation thread is in right now, for the stall report: an HLE call's name, or the PSP
// address of a block being compiled (0 = neither).
extern const char *volatile hleRunning;
extern volatile uint32_t jitCompilingPC;
inline uint64_t Now() {
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}
struct Scope {
	uint64_t &acc;
	uint64_t start;
	explicit Scope(uint64_t &a) : acc(a), start(Now()) {}
	~Scope() { acc += Now() - start; }
};
// Times one call in 8 and counts it 8 times (for code run ~100,000 times a frame, where two
// time base reads per call add up).
struct SampledScope {
	uint64_t &acc;
	uint64_t start;
	explicit SampledScope(uint64_t &a, uint32_t &n) : acc(a), start((++n & 7) == 0 ? Now() : 0) {}
	~SampledScope() { if (start) acc += (Now() - start) * 8; }
};
// Leaves the time of a compile or a warm-up interpreter run out of the JIT block profile (it would be
// charged to whichever block ran before). Sets lastTb from its value on entry, so HLE time already
// left out inside is not left out twice.
struct JitProfExclude {
	uint64_t start, saved;
	JitProfExclude() : start(jitBlockProfile ? Now() : 0), saved(jitProfState.lastTb) {}
	~JitProfExclude() { if (start && jitBlockProfile) jitProfState.lastTb = saved + (Now() - start); }
};
}  // namespace PS3Profile
#define PS3_PROFILE(var) PS3Profile::Scope ps3prof_##var(PS3Profile::var)
#define PS3_PROFILE_SAMPLED(var) static uint32_t ps3profn_##var; PS3Profile::SampledScope ps3prof_##var(PS3Profile::var, ps3profn_##var)
#else
#define PS3_PROFILE(var)
#define PS3_PROFILE_SAMPLED(var)
#endif
