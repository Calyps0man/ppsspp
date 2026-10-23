#pragma once

#include <vector>
#include <list>

#include "ppsspp_config.h"
#include "Common/Common.h"
#include "Common/Swap.h"
#include "Core/MemMap.h"
#include "Common/MemoryUtil.h"
#include "GPU/GPU.h"
#include "GPU/Debugger/Record.h"
#include "GPU/Debugger/Breakpoints.h"
#include "GPU/GPUDefinitions.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/Debugger/Debugger.h"
#include "GPU/ge_constants.h"

#include <atomic>
#include <mutex>

// X11, sigh.
#ifdef None
#undef None
#endif

class FramebufferManagerCommon;
class TextureCacheCommon;
class DrawEngineCommon;
class VertexDecoder;
class GraphicsContext;
struct PspGeListArgs;
struct GEState;
class PointerWrap;
struct VirtualFramebuffer;

namespace Draw {
class DrawContext;
}

class StringWriter;
struct DisplayLayoutConfig;

inline bool IsTrianglePrim(GEPrimitiveType prim) {
	// TODO: KEEP_PREVIOUS is mistakenly treated as TRIANGLE here... This isn't new.
	//
	// Interesting optimization, but not confident in performance:
	// static const bool p[8] = { false, false, false, true, true, true, false, true };
	// 10111000 = 0xB8;
	// return (0xB8U >> (u8)prim) & 1;

	return prim > GE_PRIM_LINE_STRIP && prim != GE_PRIM_RECTANGLES;
}

struct TransformStats;
extern int g_geTimingPercent;

// GE lists on a thread of their own (PS3 setting, off by default): ProcessDLQueue hands the queue to
// that thread and returns, and the emulation thread runs on. What the processing does to the
// emulated system (GE interrupts, sync events) is kept and done on the emulation thread once the
// thread is done; anything else that touches the GPU first waits for it (AsyncWait). The game
// learns the GE is done only through those, so it can't reuse memory the lists still read.
extern bool g_geAsyncLists;
// ... and emulated time may run past what the list thread has got to: its interrupts and sync
// events come when it's done (as if the GE were slower), and the game's code for the next frame
// runs while it works through this one. Games only see the GE done through those, so they can't
// reuse memory the lists still read.
extern bool g_geAsyncRunAhead;
// HACK on top of run-ahead: sceGeDrawSync / sceGeListSync(…, 0) don't wait for the list thread, and
// the game goes on as if its lists were drawn. Only safe if the game doesn't reuse what the lists
// read before the next frame (double-buffered lists and vertex data).
extern bool g_geAsyncNoSyncWait;
// Why the emulation thread waited for the GE list thread to finish (PS3 perf line).
enum GeAsyncReason { GEASYNC_OTHER, GEASYNC_GAME_SYNC, GEASYNC_NEXT_LISTS, GEASYNC_VBLANK, GEASYNC_PRESENT, GEASYNC_IDLE, GEASYNC_COUNT };
extern uint64_t g_geAsyncWaitTicks[GEASYNC_COUNT], g_geAsyncWaitCount[GEASYNC_COUNT];
class GeListThread;

// Display lists the GE ran from VRAM in the last few frames (PS3 RSX renderer): a framebuffer
// write-back leaves them alone. Midnight Club L.A. CALLs one kept right after a small effect buffer.
// Returns the first such list overlapping [start, end) (VRAM addresses, any mirror).
bool GeVramListOverlap(u32 start, u32 end, u32 *listStart, u32 *listEnd);
// Every GE CALL (both GPUCommon's and the software GPU's): depth is the stack slot it pushes.
void GeVramCallNote(u32 target, int depth);

class GPUCommon {
public:
	GPUCommon(const GPUCommon &) = delete;
	GPUCommon &operator=(const GPUCommon &) = delete;
	// The constructor might run on the loader thread.
	GPUCommon(GraphicsContext *gfxCtx, Draw::DrawContext *draw);
	virtual ~GPUCommon();

	virtual void GetStats(StringWriter &w) = 0;
	virtual std::vector<const VirtualFramebuffer *> GetFramebufferList() const = 0;
		
	// Needs to be called from the GPU thread, so on the same thread as a notification is fine.
	// Calling from a separate thread (e.g. UI) may fail.
	virtual bool GetCurrentFramebuffer(GPUDebugBuffer &buffer, GPUDebugFramebufferType type, int maxRes = -1) {
		// False means unsupported.
		return false;
	}
	virtual bool GetCurrentDepthbuffer(GPUDebugBuffer &buffer) { return false; }
	virtual bool GetCurrentStencilbuffer(GPUDebugBuffer &buffer) { return false; }
	virtual bool GetCurrentTexture(GPUDebugBuffer &buffer, int level, bool *isFramebuffer) { return false; }
	virtual bool GetCurrentClut(GPUDebugBuffer &buffer) { return false;}
	virtual bool GetOutputFramebuffer(GPUDebugBuffer &buffer) { return false; }

	bool GetCurrentDisplayList(DisplayList &list) const;
	bool GetCurrentDrawAsDebugVertices(GECommand cmd, GEPrimitiveType prim, GEPrimitiveType *outPrim, int count, std::vector<GPUDebugVertex> *vertices, std::vector<u16> *indices, int *lowerIndexBound, TransformStats *stats, DebugVertexFlags flags) const;
	int GetCurrentPrim(GEPrimitiveType *prim, GECommand *outCmd) const;  // Return value has the count.

	// FinishInitOnMainThread runs on the main thread, of course.
	virtual void FinishInitOnMainThread() {}

	Draw::DrawContext *GetDrawContext() {
		return draw_;
	}

	virtual void DeviceLost() = 0;
	virtual void DeviceRestore(Draw::DrawContext *draw) = 0;

	virtual u32 CheckGPUFeatures() const = 0;

	virtual void UpdateCmdInfo() = 0;

	virtual void Reinitialize();

	virtual void BeginHostFrame(const DisplayLayoutConfig &config);
	virtual void EndHostFrame();

	void InterruptStart(int listid);
	void InterruptEnd(int listid);
	void SyncEnd(GPUSyncType waitType, int listid, bool wokeThreads);
	void EnableInterrupts(bool enable) {
		interruptsEnabled_ = enable;
	}

	virtual void CheckDisplayResized() = 0;
	virtual void CheckConfigChanged(const DisplayLayoutConfig &config) = 0;

	virtual void NotifyDisplayResized();
	virtual void NotifyRenderResized(const DisplayLayoutConfig &config);
	virtual void NotifyConfigChanged();

	void DumpNextFrame();

	virtual void PreExecuteOp(u32 op, u32 diff) {}

	// allowAsync: may hand the lists to the GE list thread (g_geAsyncLists); only from a game thread's
	// sceGeListEnQueue / sceGeListUpdateStallAddr, everything else keeps the exact order.
	DLResult ProcessDLQueue(bool allowAsync = false);
	// Waits for the GE list thread (g_geAsyncLists), then does what its processing left for the
	// emulation thread. Cheap when it's idle.
	// reason: for the PS3 perf line (GeAsyncReason).
	void AsyncWait(int reason = 0);
	bool AsyncBusy() const { return asyncBusy_.load(std::memory_order_acquire); }
	void StopAsync();
	// CoreTiming, at each slice end (mustFinish false): emulated time doesn't pass what the list
	// thread has got to (startingTicks + its cycles so far), so its interrupts and sync events are
	// scheduled at the same emulated times as without it. Before idling (true): waits for it.
	void AsyncGate(bool mustFinish);

	u32 UpdateStall(int listid, u32 newstall, bool *runList);
	u32 EnqueueList(u32 listpc, u32 stall, int subIntrBase, PSPPointer<PspGeListArgs> args, bool head, bool *runList);
	u32 DequeueList(int listid);
	virtual int ListSync(int listid, int mode);
	virtual u32 DrawSync(int mode);
	int GetStack(int index, u32 stackPtr);
	virtual bool GetMatrix24(GEMatrixType type, u32_le *result, u32 cmdbits);
	virtual void ResetMatrices();
	virtual void DoState(PointerWrap &p);
	bool BusyDrawing();
	u32 Continue(bool *runList);
	u32 Break(int mode);

	virtual bool FramebufferDirty() = 0;
	virtual bool FramebufferReallyDirty() = 0;

	virtual void ReapplyGfxState();

	// Returns true if we should split the call across GE execution.
	// For example, a debugger is active.
	bool ShouldSplitOverGe() const;

	uint32_t SetAddrTranslation(uint32_t value);
	uint32_t GetAddrTranslation();

	virtual void SetDisplayFramebuffer(u32 framebuf, u32 stride, GEBufferFormat format) = 0;
	virtual void SetCurFramebufferDirty(bool dirty) = 0;
	virtual void PrepareCopyDisplayToOutput(const DisplayLayoutConfig &config) = 0;
	virtual void CopyDisplayToOutput(const DisplayLayoutConfig &config) = 0;
	virtual bool PresentedThisFrame() const = 0;

	// Invalidate any cached content sourced from the specified range.
	// If size = -1, invalidate everything.
	virtual void InvalidateCache(u32 addr, int size, GPUInvalidationType type) = 0;

	// These return true if they handled the operation enough that the actual memory operation should be skipped. Not always a clear-cut case...
	virtual bool PerformMemoryCopy(u32 dest, u32 src, int size, GPUCopyFlag flags = GPUCopyFlag::NONE);
	virtual bool PerformMemorySet(u32 dest, u8 v, int size);

	virtual bool PerformReadbackToMemory(u32 dest, int size);
	virtual bool PerformWriteColorFromMemory(u32 dest, int size);

	virtual void PerformWriteFormattedFromMemory(u32 addr, int size, int width, GEBufferFormat format);
	virtual bool PerformWriteStencilFromMemory(u32 dest, int size, WriteStencil flags);

	// Tells the texture cache that a block copy moved pixels, so it can carry "this is video" from
	// the source to the destination. Cheap and safe to call for any copy.
	void NotifyVideoCopy(u32 dest, u32 src, int size);

	virtual void ExecuteOp(u32 op, u32 diff) = 0;

	void Execute_OffsetAddr(u32 op, u32 diff);
	void Execute_Vaddr(u32 op, u32 diff);
	void Execute_Iaddr(u32 op, u32 diff);
	void Execute_Origin(u32 op, u32 diff);
	void Execute_Jump(u32 op, u32 diff);
	void Execute_BJump(u32 op, u32 diff);
	void Execute_Call(u32 op, u32 diff);
	void Execute_Ret(u32 op, u32 diff);
	void Execute_End(u32 op, u32 diff);

	void Execute_BoundingBox(u32 op, u32 diff);

	void Execute_MorphWeight(u32 op, u32 diff);

	void Execute_ImmVertexAlphaPrim(u32 op, u32 diff);

	void Execute_Unknown(u32 op, u32 diff);

	static int EstimatePerVertexCost();
	// Fill time for a through-mode rectangle draw textured from a decoded video frame, or (when
	// enabled) a clear. Zero for anything else. We don't model fill rate in general, but a movie
	// player can be paced by it.
	int EstimateFillCycles(GEPrimitiveType prim, const void *verts, const void *inds, int count, const VertexDecoder *dec, u32 vertType) const;

	// Memory a video decoder has written a frame into recently (PerformWriteFormattedFromMemory),
	// or a copy of such a frame (NotifyVideoCopy). An entry ages out a few flips after its last write.
	struct VideoInfo {
		u32 addr;
		u32 size;
		int flips;
	};
	bool IsVideo(u32 addr) const;
	// Video diagnostics: the list run in progress (for a GE sync scheduled far ahead).
	std::string RunSoFarText() const;
	bool VideoIsPlaying() const {
		return !videos_.empty();
	}
	const std::vector<VideoInfo> &Videos() const {
		return videos_;
	}

	virtual void Flush();

#ifdef USE_CRT_DBG
#undef new
#endif
	void *operator new(size_t s) {
		return AllocateAlignedMemory(s, 16);
	}
	void operator delete(void *p) {
		FreeAlignedMemory(p);
	}
#ifdef USE_CRT_DBG
#define new DBG_NEW
#endif

	virtual FramebufferManagerCommon *GetFramebufferManagerCommon() { return nullptr; }
	virtual TextureCacheCommon *GetTextureCacheCommon() { return nullptr; }
	const DrawEngineCommon *GetDrawEngineCommon() const { return drawEngineCommon_; }

	virtual std::vector<std::string> DebugGetShaderIDs(DebugShaderType shader) { return std::vector<std::string>(); };
	virtual std::string DebugGetShaderString(std::string id, DebugShaderType shader, DebugShaderStringType stringType) {
		return "N/A";
	}

	virtual bool DescribeCodePtr(const u8 *ptr, std::string &name);

	std::vector<DisplayList> ActiveDisplayLists() const;
	void ResetListPC(int listID, u32 pc);
	void ResetListStall(int listID, u32 stall);
	void ResetListState(int listID, DisplayListState state);

	GPUDebugOp DisassembleOp(u32 pc, u32 op);
	std::vector<GPUDebugOp> DisassembleOpRange(u32 startpc, u32 endpc);

	u32 GetRelativeAddress(u32 data);
	u32 GetVertexAddress();
	u32 GetIndexAddress();
	const GEState &GetGState();
	void SetCmdValue(u32 op);

	DisplayList* getList(int listid) {
		AsyncWait();
		return &dls[listid];
	}

	const std::list<int> &GetDisplayListQueue() {
		AsyncWait();
		return dlQueue;
	}
	const DisplayList &GetDisplayList(int index) {
		AsyncWait();
		return dls[index];
	}

	s64 GetListTicks(int listid) const {
		if (listid >= 0 && listid < DisplayListMaxCount) {
			return dls[listid].waitUntilTicks;
		}
		return -1;
	}

	virtual void GetReportingInfo(std::string &primaryInfo, std::string &fullInfo) const {
		primaryInfo = reportingPrimaryInfo_;
		fullInfo = reportingFullInfo_;
	}

	void PSPFrame();

	GPURecord::Recorder *GetRecorder() {
		return &recorder_;
	}
	GPUBreakpoints *GetBreakpoints() {
		return &breakpoints_;
	}

	void ClearBreakNext();
	void SetBreakNext(GPUDebug::BreakNext next);
	void SetBreakCount(int c, bool relative = false);
	GPUDebug::BreakNext GetBreakNext() const {
		return breakNext_;
	}
	int GetBreakCount() const {
		return breakAtCount_;
	}
	bool SetRestrictPrims(std::string_view rule);
	std::string_view GetRestrictPrims() {
		return restrictPrimRule_;
	}

	int PrimsThisFrame() const {
		return primsThisFrame_;
	}
	int PrimsLastFrame() const {
		return primsLastFrame_;
	}

	void NotifyFlush();

protected:
	// While debugging is active, these may block.
	void NotifyDisplay(u32 framebuf, u32 stride, int format);

	void UpdateMatrixProducts();

	bool NeedsSlowInterpreter() const;
	GPUDebug::NotifyResult NotifyCommand(u32 pc, GPUBreakpoints *breakpoints);

	virtual void ClearCacheNextFrame() {}

	virtual void CheckRenderResized(const DisplayLayoutConfig &config) {}

	void SetDrawType(DrawType type, GEPrimitiveType prim) {
		if (type != lastDraw_) {
			// We always flush when drawing splines/beziers so no need to do so here
			gstate_c.Dirty(DIRTY_UVSCALEOFFSET | DIRTY_VERTEXSHADER_STATE);
			lastDraw_ = type;
		}
		// Prim == RECTANGLES can cause CanUseHardwareTransform to flip, so we need to dirty.
		// Also, culling may be affected so dirty the raster state.
		if (IsTrianglePrim(prim) != IsTrianglePrim(lastPrim_)) {
			Flush();
			gstate_c.Dirty(DIRTY_RASTER_STATE | DIRTY_VERTEXSHADER_STATE);
			lastPrim_ = prim;
		}
	}

	virtual void CheckDepthUsage(VirtualFramebuffer *vfb) {}
	virtual void FastRunLoop(DisplayList &list) = 0;

	bool SlowRunLoop(DisplayList &list);  // Returns false on breakpoint.
	void UpdatePC(u32 currentPC, u32 newPC);
	void UpdateState(GPURunState state);
	void FastLoadBoneMatrix(u32 target);
	void FlushImm();
	void DoBlockTransfer(u32 skipDrawReason);

	// TODO: Unify this. Vulkan and OpenGL are different due to how they buffer data.
	virtual void FinishDeferred() {}

	virtual void BuildReportingInfo() = 0;

	virtual void UpdateMSAALevel(Draw::DrawContext *draw) {}

	enum {
		DisplayListMaxCount = 64
	};

	DrawEngineCommon *drawEngineCommon_ = nullptr;

	// TODO: These should live in GPUCommonHW.
	FramebufferManagerCommon *framebufferManager_ = nullptr;
	TextureCacheCommon *textureCache_ = nullptr;

	bool flushOnParams_ = true;

	GraphicsContext *gfxCtx_;
	Draw::DrawContext *draw_ = nullptr;

	typedef std::list<int> DisplayListQueue;

	int nextListID;
	DisplayList dls[DisplayListMaxCount];
	DisplayList *currentList;
	DisplayListQueue dlQueue;

	bool interruptRunning = false;
	GPURunState gpuState = GPUSTATE_RUNNING;
	bool isbreak;  // This doesn't mean debugger breakpoints.
	u64 drawCompleteTicks;
	u64 busyTicks;

	void NoteVideoRange(u32 addr, u32 size);
	void DecimateVideos();
	std::vector<VideoInfo> videos_;

	int downcount;
	u64 startingTicks;
	u32 cycleLastPC;
	int cyclesExecuted;

	bool resumingFromDebugBreak_ = false;
	bool dumpNextFrame_ = false;
	bool dumpThisFrame_ = false;
	bool useFastRunLoop_ = false;
	bool interruptsEnabled_ = false;
	bool displayResized_ = false;
	bool renderResized_ = false;
	bool configChanged_ = false;
	DrawType lastDraw_ = DRAW_UNKNOWN;
	GEPrimitiveType lastPrim_ = GE_PRIM_INVALID;

	int vertexCost_ = 0;

	// No idea how big this buffer needs to be.
	enum {
		MAX_IMMBUFFER_SIZE = 32,
	};

	TransformedVertex immBuffer_[MAX_IMMBUFFER_SIZE];
	int immCount_ = 0;
	GEPrimitiveType immPrim_ = GE_PRIM_INVALID;
	uint32_t immFlags_ = 0;
	bool immFirstSent_ = false;

	uint32_t edramTranslation_ = 0x400;

	// When matrix data overflows, the CPU visible values wrap and bleed between matrices.
	// But this doesn't actually change the values used by rendering.
	// The CPU visible values affect the GPU when list contexts are restored.
	// Note: not maintained by all backends, here for save stating.
	union {
		struct {
			u32 bone[12 * 8];
			u32 world[12];
			u32 view[12];
			u32 proj[16];
			u32 tgen[12];
		};
		u32 all[12 * 8 + 12 + 12 + 16 + 12];
	} matrixVisible;

	std::string reportingPrimaryInfo_;
	std::string reportingFullInfo_;

	// Debugging state
	bool debugRecording_ = false;

	GPURecord::Recorder recorder_;
	GPUBreakpoints breakpoints_;

	GPUDebug::BreakNext breakNext_ = GPUDebug::BreakNext::NONE;
	int breakAtCount_ = -1;

	int primsLastFrame_ = 0;
	int primsThisFrame_ = 0;
	int thisFlipNum_ = 0;

	bool primAfterDraw_ = false;

	uint32_t skipPcOnce_ = 0;

	std::vector<std::pair<int, int>> restrictPrimRanges_;
	std::string restrictPrimRule_;

	// GE list thread (g_geAsyncLists).
	DLResult ProcessDLQueueBody();
	friend class GeListThread;
	// __GeTriggerSync / __GeTriggerInterrupt, kept for the emulation thread while on the list thread.
	void TriggerSync(GPUSyncType type, int id, u64 atTicks);
	bool TriggerInterrupt(int listid, u32 pc, u64 atTicks);
	// CPU-side notes (cache invalidation, memory copies) that came while the list thread ran: done
	// after it, before the next lists (the lists in flight were the GE's before these writes).
	enum class DeferredOp : u8 { INVALIDATE, COPY, SET, READBACK, WRITE_COLOR, VIDEO_COPY };
	bool AsyncDefer(DeferredOp op, u32 a, u32 b, int size, u32 extra);
	struct AsyncEffect {
		bool interrupt;
		GPUSyncType type;
		int id;
		u32 pc;
		u64 ticks;
	};
	struct AsyncCpuOp {
		DeferredOp op;
		u32 a, b;
		int size;
		u32 extra;
	};
	std::vector<AsyncEffect> asyncEffects_;
	std::vector<AsyncCpuOp> asyncCpuOps_;
	// List thread: its cycles so far, published for AsyncGate (each draw and jump).
	void PublishAsyncProgress() {
		if (asyncPublish_) {
			asyncProgress_.store(cyclesExecuted, std::memory_order_release);
			if (asyncGateWaiting_.load(std::memory_order_relaxed))
				NotifyAsyncGate();
		}
	}
	void NotifyAsyncGate();
	void TakeAsyncEffects();
	std::atomic<bool> asyncBusy_{false};
	std::atomic<int> asyncProgress_{0};
	std::atomic<bool> asyncGateWaiting_{false};
	bool asyncPublish_ = false;  // List thread only.
	u64 asyncStartTicks_ = 0;    // Emulation thread only.
	std::mutex asyncEffectsLock_;
	GeListThread *asyncThread_ = nullptr;

private:
	void DoExecuteCall(u32 target);
	void PopDLQueue();
	void CompleteFailedList(DisplayList &list);
};

// Video diagnostics: GE time per display list run (GPUCommon::ProcessDLQueue), printed and reset by
// GeListTimeStatsText() from sceMpeg's once-a-second line.
struct GeListTimeStats {
	int runs = 0;
	s64 totalUs = 0, maxUs = 0, backlogUs = 0, backlogMaxUs = 0, fillUs = 0, transferUs = 0;
};
extern GeListTimeStats g_geListTimeStats;
std::string GeListTimeStatsText();

