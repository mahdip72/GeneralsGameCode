#ifndef RTS_WW3D2_NATIVEW3D2_H
#define RTS_WW3D2_NATIVEW3D2_H

#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameClientNative.h"
#include "nativew3dsorting.h"
#include "nativew3dline.h"

#include <vector>

// A narrow native WW3D entry point.  It owns the lifetime ordering between
// the facade and the resource registry; gameplay integration follows only as
// individual legacy classes are migrated to this target.  The aggregate is
// render-owner-thread affine: initialization, commands, shutdown, and
// destruction must all run on the thread that owns its resource state.
class NativeW3D2 : public rts::render::IGameRenderClientNativeOwner,
	public rts::render::NativeSortedGeometrySink
{
public:
	NativeW3D2();
	~NativeW3D2();

	rts::render::RenderResult Initialize(void *window,
		const rts::render::NativeW3DRendererDescriptor &descriptor);
	// Product cutover seam: borrow the bridge-owned backend instead of creating
	// a second device.  The bridge retains backend ownership and publishes a
	// replacement context after successful recovery.
	rts::render::RenderResult AttachBackend(
		rts::render::IRenderDevice *device,
		rts::render::IRenderContext *context);
	rts::render::RenderResult ReplaceBackendContext(
		rts::render::IRenderContext *context);
	rts::render::RenderResult DrainResourceCleanup(
		unsigned int maxCommands, unsigned int *drained);
	rts::render::RenderResult PublishThreadedCompletion(
		rts::render::NativeW3DSubmissionSequence submissionSequence,
		bool resourceFailure);
	rts::render::RenderResult RecoverDevice();
	rts::render::RenderResult Shutdown();
	bool IsAttachedToBorrowedBackend() const;
	rts::render::NativeW3DRenderer &Renderer();
	rts::render::NativeW3DResources &Resources();

	// IGameRenderClientNativeOwner implementation.  These methods are the
	// sole native GameEngineDevice command boundary; callers submit logical
	// state and opaque handles without constructing a second backend.
	virtual bool IsInitialized() const;
	virtual bool IsOperational() const;
	// Resource cleanup/reacquire callbacks run on the owner between lifecycle
	// fences. They may rebuild logical shaders while frame/state admission stays
	// closed; the capability gate consumes this narrow predicate separately.
	virtual bool IsRebuildingResources() const;
	virtual rts::render::GameRenderTargetKind ActiveRenderTargetKind() const;
	virtual rts::render::RenderResult ExecuteGameRenderCommand(
		const rts::render::GameRenderCommand &command);
	virtual rts::render::RenderResult SetGameViewport(
		const rts::render::RenderViewport &viewport);
	virtual rts::render::RenderResult ApplyGameShaderBits(
		unsigned int shaderBits);
	virtual rts::render::RenderResult SetGameRenderState(
		unsigned int state, unsigned int value);
	virtual void SetGameRenderCamera(void *camera);
	virtual rts::render::RenderResult SetGameRenderCameraSnapshot(
		const rts::render::GameCameraSnapshot &snapshot);
	virtual rts::render::RenderResult CreateGameShaderFromAsset(
		const char *assetPath, bool vertexShader, const void *declarationWords,
		unsigned int declarationWordCount, unsigned int usage,
		unsigned int *handle);
	virtual bool DeleteGameShader(bool vertexShader, unsigned int handle);
	virtual void SetGameVertexShader(unsigned int shaderOrFormat);
	virtual void SetGamePixelShader(unsigned int shader);
	virtual void SetGameLegacyVertexProgram(
		rts::render::RenderLegacyVertexProgram program);
	virtual void SetGameLegacyPixelProgram(
		rts::render::RenderLegacyPixelProgram program);
	virtual rts::render::RenderResult SubmitGameTriangles(
		const rts::render::LegacyLogicalState &state,
		const rts::render::NativeDrawPacket &packet);
	virtual rts::render::RenderResult QueueGameSortedTriangles(
		const rts::render::LegacyLogicalState &state,
		const rts::render::NativeDrawPacket &packet,
		const void *vertexData, size_t vertexBytes, const void *indexData,
		size_t indexBytes, const rts::render::GameBoundingSphere *sphere);
	virtual rts::render::RenderResult FlushGameSortedTriangles();
	virtual rts::render::RenderResult BeginGameDisplayIteration();
	virtual rts::render::RenderResult ResetGameRenderFrameResources(
		bool frameChanged);
	virtual bool SupportsPointSprites() const;
	virtual bool SupportsDot3() const;
	virtual bool SupportsZBias() const;
	virtual bool SupportsStencil() const;
	virtual bool SupportsNPatches() const;
	virtual bool IsGameTextureFormatSupported(
		rts::render::RenderFormat format) const;
	virtual rts::render::RenderResult SetGameFogState(
		const rts::render::LegacyFogConstants &fog);
	virtual rts::render::RenderResult SetGameLightState(unsigned int index,
		const rts::render::LegacyLightState &light);
	virtual bool IsDebugConsoleDisabled() const;
	virtual rts::render::RenderResult GetTextureFilterCapabilities(
		rts::render::GameTextureFilterCapabilities *capabilities) const;
	virtual unsigned int GetMaxTexturesPerPass() const;
	virtual rts::render::RenderResult InvalidateGameMeshRendererCache();
	virtual rts::render::RenderResult GetGameBackBufferInfo(
		rts::render::RenderBackBufferInfo *info) const;
	virtual rts::render::RenderResult QueueGameBackBufferCapture(
		const rts::render::RenderCaptureRequestDescriptor &descriptor,
		rts::render::RenderCaptureHandle *handle);
	virtual unsigned int CancelGameBackBufferCaptures(
		void *consumer, rts::render::RenderResult reason);
	virtual void RequestGameBackBufferCapture();
	virtual void RecordGameFailure(rts::render::RenderResult result);

	// NativeSortedGeometrySink implementation.  The sorter passes one
	// transient vertex/index image plus contiguous legacy state groups; this
	// method is the only place those CPU bytes become native resources.
	virtual rts::render::RenderResult SubmitNativeSortedBatch(
		const rts::render::NativeSortedDraw *draws,
		unsigned int drawCount, const void *vertexData, size_t vertexBytes,
		const void *indexData, size_t indexBytes,
		unsigned int *submittedDrawCount);

	// Target state is published by the existing bridge/native render-owner
	// lifecycle.  The hooks are intentionally neutral so the bridge can update
	// them without importing title types into this class.
	void SetActiveRenderTargetKind(
		rts::render::GameRenderTargetKind targetKind);
	void SetGameDebugConsoleDisabled(bool disabled);
	// The title ShaderClass owns the process-wide inversion bit.  The adapter
	// publishes that bit before applying a shader so the native pipeline keeps
	// the legacy winding convention without importing title headers here.
	virtual rts::render::RenderResult SetGameShaderCullInverted(
		bool inverted);
	virtual void SetGameCleanupHook(
		rts::render::GameRenderCleanupHook *hook);
	unsigned int DisplayIterationEpoch() const;

private:
	NativeW3D2(const NativeW3D2 &);
	NativeW3D2 &operator=(const NativeW3D2 &);

	rts::render::NativeW3DRenderer m_renderer;
	rts::render::NativeW3DResourceHost m_resourceHost;
	rts::render::NativeW3DResources m_resources;
	bool m_borrowedBackend;
	// Title-owned resource publication is a separate availability gate from
	// backend initialization.  Reset/resize/recovery clear it before releasing
	// game resources and restore it only after every resource is reacquired.
	bool m_gameResourcesOperational;
	// The submitter is an aggregate-owned service.  Its address stays stable
	// across backend-context replacement, while publication is bounded by the
	// NativeW3D2 attach/shutdown lifecycle below.
	rts::render::NativeLine3DRenderContext m_line3DContext;
	rts::render::GameRenderTargetKind m_activeRenderTargetKind;
	bool m_debugConsoleDisabled;
	rts::render::RenderFrameFailureLatch m_gameFailure;
	// Threaded completion is producer-owned state.  The completion sequence is
	// retained until the legacy boundary reports it, so a later Begin/Execute
	// call cannot mistake queue admission for execution success.
	rts::render::RenderFrameOutcome m_deferredFailure;
	rts::render::NativeW3DSubmissionSequence m_deferredFailureSequence;
	rts::render::NativeW3DSubmissionSequence m_recoveredFailureSequence;
	bool m_asyncResourceFailure;
	// Cleanup/reacquire hooks run on this owner thread between backend teardown
	// and publication. Resource recreation is admitted during that interval,
	// while frame/draw entry points continue to require IsOperational().
	bool m_rebuildingResources;
	rts::render::RenderCaptureQueue m_gameCaptureQueue;
	rts::render::RenderCaptureRequest m_gameCaptureRequest;
	unsigned int m_displayIterationEpoch;
	rts::render::NativeSortingRenderer m_nativeSortingRenderer;
	bool m_gameShaderCullInverted;
	rts::render::GameRenderCleanupHook *m_gameCleanupHook;
	bool m_gameCameraValid;
	rts::render::RenderMatrix4 m_gameCameraView;
	rts::render::RenderMatrix4 m_gameCameraProjection;
	rts::render::RenderViewport m_gameCameraViewport;
	float m_gameCameraNear;
	float m_gameCameraFar;
	// Synchronous command state mirrors the logical bindings made by the old
	// GameEngineDevice calls.  These are handles and POD layout values only; no
	// title object or backend pointer crosses the owner boundary.
	rts::render::GpuHandle m_gameVertexBuffer;
	rts::render::GpuHandle m_gameIndexBuffer;
	rts::render::GpuHandle m_gameTextures[
		rts::render::LEGACY_TEXTURE_STAGE_COUNT];
	unsigned int m_gameVertexStream;
	unsigned int m_gameVertexStride;
	unsigned int m_gameVertexOffset;
	unsigned int m_gameIndexOffset;
	int m_gameIndexBaseVertex;
	rts::render::RenderFormat m_gameIndexFormat;
	rts::render::RenderVertexFormat m_gameVertexFormat;
	rts::render::RenderVertexLayout m_gameVertexLayout;
	rts::render::RenderPrimitiveTopology m_gameTopology;
	bool m_gameVertexBound;
	bool m_gameIndexBound;
	std::vector<unsigned char> m_gameSortedVertexBytes;
	std::vector<unsigned char> m_gameSortedIndexBytes;
	unsigned int m_gameSortedVertexMinimum;
	unsigned int m_gameSortedVertexCount;
	unsigned int m_gameSortedVertexSourceOffset;
	unsigned int m_gameSortedIndexStart;
	unsigned int m_gameSortedIndexCount;

	struct NativeShaderEntry
	{
		NativeShaderEntry();
		bool live;
		bool vertexShader;
		rts::render::RenderLegacyPixelProgram pixelProgram;
		rts::render::RenderLegacyVertexProgram vertexProgram;
		unsigned int generation;
	};
	std::vector<NativeShaderEntry> m_gameShaders;

	bool FindGameShaderAsset(const char *assetPath, bool vertexShader,
		rts::render::RenderLegacyPixelProgram *pixelProgram,
		rts::render::RenderLegacyVertexProgram *vertexProgram) const;
	bool DecodeGameShaderHandle(bool vertexShader, unsigned int handle,
		NativeShaderEntry **entry);
	bool DecodeGameShaderHandle(bool vertexShader, unsigned int handle,
		const NativeShaderEntry **entry) const;
	rts::render::RenderResult PollThreadedCompletions(
		rts::render::NativeW3DSubmissionSequence wanted = 0,
		rts::render::ThreadedRenderFrameCompletion *matched = 0);
	void RememberThreadedFailure(
		const rts::render::RenderFrameOutcome &outcome,
		rts::render::NativeW3DSubmissionSequence sequence);
	rts::render::RenderResult ServiceThreadedCompletions();
	rts::render::RenderResult FenceThreadedRender();
	rts::render::RenderResult RecoverOwnedDevice();
	rts::render::RenderResult CancelOpenThreadedFrame(
		rts::render::RenderResult reason);
	bool CanRebuildResources() const;
	rts::render::RenderResult PrepareGameBackBufferCapture(
		rts::render::RenderBackBufferInfo *info,
		std::vector<unsigned char> *pixels, size_t *rowPitch,
		rts::render::RenderFormat *format);
	rts::render::RenderResult CompleteGameBackBufferCaptures(
		const rts::render::RenderBackBufferInfo &info, size_t rowPitch,
		rts::render::RenderFormat format, const std::vector<unsigned char> &pixels);
	rts::render::RenderResult FinishGameRenderFrame(bool capture,
		bool present);

	rts::render::RenderResult SubmitGamePacket(
		const rts::render::LegacyLogicalState &state,
		const rts::render::NativeDrawPacket &packet);
};

#endif
