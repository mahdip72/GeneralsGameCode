#include "Utility/CppMacros.h"
#include "nativew3d2.h"

#include "Renderer/RenderGameClient.h"
#include "Renderer/ThreadedRenderDevice.h"
#include "Renderer/LegacyAsyncFramePolicy.h"

#include "Renderer/LegacyBridgeValidation.h"
#include "dx8indexbuffer.h"
#include "nativew3dbufferowner.h"

#include <float.h>
#include <limits.h>
#include <new>
#include <string.h>
#include <assert.h>
#include <stdio.h>

namespace
{

bool IsFiniteGameFloat(float value)
{
	return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

bool IsValidGameCompare(unsigned int value)
{
	return value <= static_cast<unsigned int>(rts::render::RENDER_COMPARE_ALWAYS);
}

bool IsValidGameBlendFactor(unsigned int value)
{
	return value <= static_cast<unsigned int>(
		rts::render::RENDER_BLEND_INVERSE_DESTINATION_COLOR);
}

bool IsValidGameBlendOperation(unsigned int value)
{
	return value <= static_cast<unsigned int>(rts::render::RENDER_BLEND_MAXIMUM);
}

bool IsValidGameStencilOperation(unsigned int value)
{
	return value <= static_cast<unsigned int>(
		rts::render::RENDER_STENCIL_DECREMENT);
}

bool NormalizeGameStencilByte(unsigned int value, unsigned int *normalized)
{
	if (normalized == 0)
		return false;
	if (value <= 0xffU)
	{
		*normalized = value;
		return true;
	}

	// The historical title path sometimes widened an 8-bit stencil value by
	// replicating it into all four bytes.  Its shadow path also complements a
	// low-byte mask in a 32-bit unsigned expression, producing 0xffffffNN.
	// Accept only those two lossless encodings; unrelated wide values remain
	// invalid rather than being silently truncated.
	const unsigned int lowByte = value & 0xffU;
	const unsigned int repeatedByte = lowByte * 0x01010101U;
	const unsigned int signExtendedByte = 0xffffff00U | lowByte;
	if (value != repeatedByte && value != signExtendedByte)
		return false;
	*normalized = lowByte;
	return true;
}

bool IsValidGameShaderPath(const char *assetPath, char *normalized,
	size_t capacity)
{
	if (assetPath == 0 || normalized == 0 || capacity < 2)
		return false;
	size_t length = 0;
	for (; assetPath[length] != '\0'; ++length)
	{
		if (length + 1 >= capacity)
			return false;
		char character = assetPath[length];
		if (character == '/')
			character = '\\';
		if (character >= 'A' && character <= 'Z')
			character = static_cast<char>(character - 'A' + 'a');
		normalized[length] = character;
	}
	normalized[length] = '\0';
	return length != 0;
}

bool GameShaderPathEquals(const char *path, const char *expected)
{
	return path != 0 && expected != 0 && strcmp(path, expected) == 0;
}

rts::render::GpuHandle ToGameGpuHandle(
	const rts::render::GameRenderHandle &handle)
{
	return handle.index == 0 || handle.generation == 0 ?
		rts::render::GpuHandle() :
	rts::render::GpuHandle(handle.index, handle.generation);
}

bool IsWellFormedGameHandle(const rts::render::GameRenderHandle &handle)
{
	return (handle.index == 0U) == (handle.generation == 0U);
}

bool IsValidGameTopology(unsigned int value)
{
	return value <= static_cast<unsigned int>(
		rts::render::RENDER_PRIMITIVE_LINE_STRIP);
}

bool IsFiniteGameColor(const rts::render::RenderFloat4 &color)
{
	return IsFiniteGameFloat(color.x) && IsFiniteGameFloat(color.y) &&
		IsFiniteGameFloat(color.z) && IsFiniteGameFloat(color.w);
}

bool IsValidGameMaterialSource(unsigned int value)
{
	return value <= static_cast<unsigned int>(
		rts::render::RENDER_MATERIAL_SOURCE_COLOR2);
}

bool DecodeGameTextureArgument(unsigned int value,
	rts::render::RenderTextureArgument *argument, bool *complement,
	bool *alphaReplicate)
{
	const unsigned int modifiers =
		rts::render::GAME_TEXTURE_ARGUMENT_COMPLEMENT |
		rts::render::GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE;
	if (argument == 0 || complement == 0 || alphaReplicate == 0 ||
		(value & ~(0xffU | modifiers)) != 0U ||
		(value & 0xffU) > static_cast<unsigned int>(
			rts::render::RENDER_TEXTURE_ARG_TEMP))
	{
		return false;
	}
	*argument = static_cast<rts::render::RenderTextureArgument>(value & 0xffU);
	*complement = (value & rts::render::GAME_TEXTURE_ARGUMENT_COMPLEMENT) != 0;
	*alphaReplicate =
		(value & rts::render::GAME_TEXTURE_ARGUMENT_ALPHA_REPLICATE) != 0;
	return true;
}

bool IsValidGameClearDepth(float depth)
{
	return IsFiniteGameFloat(depth) && depth >= 0.0f && depth <= 1.0f;
}

bool CheckedGameSizeMultiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right > static_cast<size_t>(-1) / left))
		return false;
	*result = left * right;
	return true;
}

rts::render::RenderResult FirstNativeThreadedFailure(
	rts::render::RenderResult first, rts::render::RenderResult next)
{
	if (next == rts::render::RENDER_RESULT_DEVICE_REMOVED)
		return next;
	return first == rts::render::RENDER_RESULT_OK ? next : first;
}

bool InvokeGameCleanupRelease(rts::render::GameRenderCleanupHook *hook)
{
	if (hook == 0)
		return true;
	try
	{
		hook->ReleaseResources();
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool InvokeGameCleanupReAcquire(rts::render::GameRenderCleanupHook *hook)
{
	if (hook == 0)
		return true;
	try
	{
		hook->ReAcquireResources();
		return true;
	}
	catch (...)
	{
		return false;
	}
}

} // namespace


NativeW3D2::NativeShaderEntry::NativeShaderEntry() : live(false),
	vertexShader(false),
	pixelProgram(rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION),
	vertexProgram(rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION),
	generation(1)
{
}

NativeW3D2::NativeW3D2() : m_resourceHost(256), m_resources(4096),
	m_borrowedBackend(false), m_gameResourcesOperational(false),
	m_line3DContext(&m_renderer, &m_resources),
	m_activeRenderTargetKind(rts::render::GAME_RENDER_TARGET_UNKNOWN),
	m_debugConsoleDisabled(false), m_gameFailure(), m_deferredFailure(),
	m_deferredFailureSequence(0), m_recoveredFailureSequence(0),
	m_asyncResourceFailure(false), m_rebuildingResources(false),
	m_gameCaptureQueue(8), m_gameCaptureRequest(),
	m_displayIterationEpoch(1), m_nativeSortingRenderer(),
	m_gameShaderCullInverted(false), m_gameCleanupHook(0),
	m_gameCameraValid(false),
	m_gameCameraView(), m_gameCameraProjection(), m_gameCameraViewport(),
	m_gameCameraNear(0.0f), m_gameCameraFar(1.0f), m_gameVertexBuffer(),
	m_gameIndexBuffer(), m_gameVertexStream(0), m_gameVertexStride(0),
	m_gameVertexOffset(0), m_gameIndexOffset(0), m_gameIndexBaseVertex(0),
	m_gameIndexFormat(rts::render::RENDER_FORMAT_R16_UINT),
	m_gameVertexFormat(rts::render::RENDER_VERTEX_POSITION3_COLOR),
	m_gameVertexLayout(),
	m_gameTopology(rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST),
	m_gameVertexBound(false), m_gameIndexBound(false),
	m_gameSortedVertexBytes(), m_gameSortedIndexBytes(),
	m_gameSortedVertexMinimum(0), m_gameSortedVertexCount(0),
	m_gameSortedVertexSourceOffset(0), m_gameSortedIndexStart(0),
	m_gameSortedIndexCount(0), m_gameShaders()
{
	for (unsigned int stage = 0;
		stage < rts::render::LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		m_gameTextures[stage] = rts::render::GpuHandle();
	}
}

NativeW3D2::~NativeW3D2()
{
	const rts::render::RenderResult shutdownResult = Shutdown();
	assert(shutdownResult == rts::render::RENDER_RESULT_OK);
	if (shutdownResult != rts::render::RENDER_RESULT_OK)
	{
		// An off-owner destructor cannot safely release the shared backend.  The
		// owner-thread contract makes this a programmer/lifecycle error; leave the
		// publication and resource state untouched in release builds as well.
		return;
	}
	if (rts::render::Get_Native_Line3D_Submitter() == &m_line3DContext)
	{
		rts::render::Set_Native_Line3D_Submitter(0);
	}
}

rts::render::RenderResult NativeW3D2::Initialize(void *window,
	const rts::render::NativeW3DRendererDescriptor &descriptor)
{
	if (rts::render::IsNativeGameRenderOwnerPinnedByCurrentThread() ||
		m_borrowedBackend)
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const rts::render::RenderResult result = m_renderer.Initialize(window,
		descriptor);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		return result;
	}
	const rts::render::RenderResult bindResult = m_resources.Bind(&m_renderer);
	if (bindResult != rts::render::RENDER_RESULT_OK)
	{
		m_renderer.Shutdown();
		return bindResult;
	}
	const rts::render::RenderResult bufferBindResult =
		rts::render::BindNativeW3DBufferResources(&m_resources);
	if (bufferBindResult != rts::render::RENDER_RESULT_OK)
	{
		m_resources.Shutdown();
		m_renderer.Shutdown();
		return bufferBindResult;
	}
	if (!m_gameCaptureQueue.bindOwnerThread())
	{
		rts::render::UnbindNativeW3DBufferResources(&m_resources);
		m_resources.Shutdown();
		m_renderer.Shutdown();
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_renderer.m_recoveryResources = &m_resources;
	m_deferredFailure = rts::render::RenderFrameOutcome();
	m_deferredFailureSequence = 0;
	m_recoveredFailureSequence = 0;
	m_asyncResourceFailure = false;
	m_rebuildingResources = false;
	m_activeRenderTargetKind = rts::render::GAME_RENDER_TARGET_BACK_BUFFER;
	m_gameResourcesOperational = true;
	// Publish directly through the scope held by this transition. Calling the
	// convenience setter here would attempt a nested lifecycle acquisition,
	// which is deliberately rejected while lifecycle state is being published.
	rts::render::NativeGameRenderOwnerLifecycleScope ownerLifecycleScope;
	if (!ownerLifecycleScope.IsAcquired())
	{
		rts::render::UnbindNativeW3DBufferResources(&m_resources);
		m_resources.Shutdown();
		m_renderer.Shutdown();
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	ownerLifecycleScope.Publish(this);
	rts::render::NativeLine3DSubmitterLifecycleScope lineLifecycleScope;
	lineLifecycleScope.Publish(&m_line3DContext);
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::AttachBackend(
	rts::render::IRenderDevice *device, rts::render::IRenderContext *context)
{
	if (rts::render::IsNativeGameRenderOwnerPinnedByCurrentThread() ||
		m_borrowedBackend || m_renderer.IsInitialized())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	rts::render::RenderResult result = m_resourceHost.Attach(device, context);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		return result;
	}
	result = m_renderer.AttachBorrowedState(m_resourceHost.State());
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_resourceHost.Detach();
		return result;
	}
	result = m_resources.BindHost(&m_resourceHost);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_renderer.DetachBorrowedState();
		m_resourceHost.Detach();
		return result;
	}
	result = rts::render::BindNativeW3DBufferResources(&m_resources);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_resources.Shutdown();
		m_renderer.DetachBorrowedState();
		m_resourceHost.Detach();
		return result;
	}
	if (!m_gameCaptureQueue.bindOwnerThread())
	{
		rts::render::UnbindNativeW3DBufferResources(&m_resources);
		m_resources.Shutdown();
		m_renderer.DetachBorrowedState();
		m_resourceHost.Detach();
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_borrowedBackend = true;
	m_deferredFailure = rts::render::RenderFrameOutcome();
	m_deferredFailureSequence = 0;
	m_recoveredFailureSequence = 0;
	m_asyncResourceFailure = false;
	m_rebuildingResources = false;
	m_activeRenderTargetKind = rts::render::GAME_RENDER_TARGET_BACK_BUFFER;
	m_gameResourcesOperational = true;
	// See Initialize: publish directly through the already-acquired lifecycle
	// scope so owner publication cannot be mistaken for recursive teardown.
	rts::render::NativeGameRenderOwnerLifecycleScope ownerLifecycleScope;
	if (!ownerLifecycleScope.IsAcquired())
	{
		rts::render::UnbindNativeW3DBufferResources(&m_resources);
		m_resources.Shutdown();
		m_renderer.DetachBorrowedState();
		m_resourceHost.Detach();
		m_borrowedBackend = false;
		m_gameResourcesOperational = false;
		m_activeRenderTargetKind =
			rts::render::GAME_RENDER_TARGET_UNKNOWN;
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	ownerLifecycleScope.Publish(this);
	rts::render::NativeLine3DSubmitterLifecycleScope lineLifecycleScope;
	lineLifecycleScope.Publish(&m_line3DContext);
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::ReplaceBackendContext(
	rts::render::IRenderContext *context)
{
	if (rts::render::IsNativeGameRenderOwnerPinnedByCurrentThread() ||
		!m_borrowedBackend)
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	// The context and resource epochs are one lifecycle boundary for both
	// GameEngineDevice commands and Line3D. Hold both publication gates while
	// replacing the backend pointer; otherwise a render could pin this owner
	// between context replacement and its publication update.
	rts::render::NativeGameRenderOwnerLifecycleScope ownerLifecycleScope;
	if (!ownerLifecycleScope.IsAcquired())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	const bool wasOwnerPublished = ownerLifecycleScope.Get() == this;
	if (wasOwnerPublished)
		ownerLifecycleScope.Publish(0);
	rts::render::NativeLine3DSubmitterLifecycleScope lifecycleScope;
	const bool wasPublished = lifecycleScope.Get() == &m_line3DContext;
	if (wasPublished)
	{
		lifecycleScope.Publish(0);
	}
	const rts::render::RenderResult result =
		m_resourceHost.ReplaceContext(context);
	if (wasPublished)
	{
		// ReplaceContext either leaves the old context usable on failure or
		// installs the supplied context on success.  In both cases the aggregate
		// service remains the owner after this quiescent transition.
		lifecycleScope.Publish(&m_line3DContext);
	}
	if (wasOwnerPublished)
		ownerLifecycleScope.Publish(this);
	return result;
}

rts::render::RenderResult NativeW3D2::DrainResourceCleanup(
	unsigned int maxCommands, unsigned int *drained)
{
	return !m_borrowedBackend ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
		m_resourceHost.DrainCleanup(maxCommands, drained);
}

rts::render::RenderResult NativeW3D2::PublishThreadedCompletion(
	rts::render::NativeW3DSubmissionSequence submissionSequence,
	bool resourceFailure)
{
	// Both the owned native product and the legacy bridge may use the same
	// publication path.  The registry deliberately accepts a completion after
	// the threaded device has reported removal so failed optimistic ranges can
	// be invalidated before recovery; checking IsInitialized() here would drop
	// that authority transition.
	if (!m_renderer.HasBackendState() || !m_resources.IsOwnerThread())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	return m_resources.PublishThreadedCompletion(submissionSequence,
		resourceFailure);
}

void NativeW3D2::RememberThreadedFailure(
	const rts::render::RenderFrameOutcome &outcome,
	rts::render::NativeW3DSubmissionSequence sequence)
{
	if (outcome.result() == rts::render::RENDER_RESULT_OK)
		return;
	if (m_deferredFailureSequence == 0 ||
		rts::render::ShouldReplaceLegacyAsyncFrameFailure(m_deferredFailure,
			outcome, m_recoveredFailureSequence == m_deferredFailureSequence))
	{
		m_deferredFailure = outcome;
		m_deferredFailureSequence = sequence;
		m_recoveredFailureSequence = 0;
	}
}

rts::render::RenderResult NativeW3D2::PollThreadedCompletions(
	rts::render::NativeW3DSubmissionSequence wanted,
	rts::render::ThreadedRenderFrameCompletion *matched)
{
	using namespace rts::render;
	if (!m_renderer.IsThreaded())
		return RENDER_RESULT_OK;
	if (!m_resources.IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (matched != 0)
		*matched = ThreadedRenderFrameCompletion();
	RenderResult result = RENDER_RESULT_OK;
	ThreadedRenderFrameCompletion completed;
	while (m_renderer.PollThreadedCompletion(&completed))
	{
		const RenderResult publication = PublishThreadedCompletion(
			completed.sequence, completed.resourceFailure);
		if (publication != RENDER_RESULT_OK)
		{
			m_asyncResourceFailure = true;
			result = FirstNativeThreadedFailure(result, publication);
			RenderFrameOutcome publicationFailure;
			publicationFailure.recordCommandFailure(publication);
			publicationFailure.setOperational(m_renderer.IsBackendOperational());
			RememberThreadedFailure(publicationFailure, completed.sequence);
		}
		if (completed.resourceFailure)
			m_asyncResourceFailure = true;
		if (matched != 0 && wanted != 0 &&
			completed.sequence == wanted)
			*matched = completed;
		if (completed.result != RENDER_RESULT_OK)
			RememberThreadedFailure(completed.outcome, completed.sequence);
	}
	return result;
}

rts::render::RenderResult NativeW3D2::FenceThreadedRender()
{
	if (!m_renderer.IsThreaded())
		return rts::render::RENDER_RESULT_OK;
	const rts::render::RenderResult drainResult = m_renderer.DrainThreaded();
	const rts::render::RenderResult publicationResult =
		PollThreadedCompletions();
	return FirstNativeThreadedFailure(drainResult, publicationResult);
}

rts::render::RenderResult NativeW3D2::CancelOpenThreadedFrame(
	rts::render::RenderResult reason)
{
	if (!m_renderer.IsFrameOpen())
		return rts::render::RENDER_RESULT_OK;
	if (m_renderer.IsThreaded())
		return m_renderer.CancelThreadedFrame(reason);
	const rts::render::RenderResult endResult = m_renderer.EndFrame(false);
	return endResult == rts::render::RENDER_RESULT_OK ? reason : endResult;
}

bool NativeW3D2::CanRebuildResources() const
{
	return m_rebuildingResources && m_resources.IsOwnerThread() &&
		m_renderer.HasBackendState() && m_renderer.IsBackendOperational() &&
		!m_renderer.IsFrameOpen();
}

rts::render::RenderResult NativeW3D2::ServiceThreadedCompletions()
{
	using namespace rts::render;
	if (!m_renderer.IsThreaded())
		return RENDER_RESULT_OK;
	if (!m_resources.IsOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	RenderResult result = PollThreadedCompletions();
	// Resource-only packets have no frame completion. If their owner-side fence
	// reports a removal, retain a synthetic outcome so recovery is still driven
	// by the next render-owner boundary.
	if (!m_renderer.IsBackendOperational() &&
		(m_deferredFailureSequence == 0 ||
			(m_recoveredFailureSequence == m_deferredFailureSequence &&
				m_deferredFailure.recoveryResult() == RENDER_RESULT_OK)))
	{
		const RenderResult stalledResult = m_renderer.DrainThreaded();
		const RenderResult publicationResult = PollThreadedCompletions();
		result = FirstNativeThreadedFailure(result, stalledResult);
		result = FirstNativeThreadedFailure(result, publicationResult);
		if (stalledResult != RENDER_RESULT_OK ||
			!m_renderer.IsBackendOperational())
		{
			RenderFrameOutcome stalledOutcome;
			// A resource-only packet has no completion mailbox record. The fence
			// normally carries its failure, but a backend can become non-operational
			// while returning OK from the control packet (for example after an
			// asynchronous device-loss transition). Keep that transition visible to
			// the aggregate instead of allowing the next draw to fail closed without
			// entering recovery.
			const RenderResult failure = stalledResult != RENDER_RESULT_OK ?
				stalledResult : RENDER_RESULT_DEVICE_REMOVED;
			stalledOutcome.recordCommandFailure(failure);
			stalledOutcome.setOperational(false);
			RememberThreadedFailure(stalledOutcome,
				static_cast<NativeW3DSubmissionSequence>(
					~static_cast<NativeW3DSubmissionSequence>(0)));
			m_asyncResourceFailure = true;
		}
	}
	if (m_deferredFailureSequence != 0 &&
		(m_deferredFailure.hasDeviceRemoval() ||
			!m_renderer.IsBackendOperational()) &&
		m_recoveredFailureSequence != m_deferredFailureSequence)
	{
		const RenderResult cancelResult = CancelOpenThreadedFrame(
			RENDER_RESULT_DEVICE_REMOVED);
		if (cancelResult != RENDER_RESULT_OK &&
			cancelResult != RENDER_RESULT_INVALID_ARGUMENT)
			result = FirstNativeThreadedFailure(result, cancelResult);
		const RenderResult fenceResult = FenceThreadedRender();
		result = FirstNativeThreadedFailure(result, fenceResult);
		const RenderResult recoveryResult =
			m_renderer.CanRecoverDevice() ? RecoverOwnedDevice() :
			RENDER_RESULT_INVALID_ARGUMENT;
		m_deferredFailure.recordRecovery(recoveryResult);
		m_deferredFailure.setOperational(
			m_renderer.IsBackendOperational());
		m_recoveredFailureSequence = m_deferredFailureSequence;
		if (recoveryResult != RENDER_RESULT_OK)
			result = FirstNativeThreadedFailure(result, recoveryResult);
	}
	if (m_asyncResourceFailure && m_renderer.IsBackendOperational())
	{
		// PublishThreadedCompletion already invalidates the exact failed ranges.
		// Cancel any producer work which was assembled after that completion and
		// retain one aggregate failure for the next display boundary.
		const RenderResult cancelResult = CancelOpenThreadedFrame(
			RENDER_RESULT_FAILED);
		if (cancelResult != RENDER_RESULT_OK &&
			cancelResult != RENDER_RESULT_INVALID_ARGUMENT)
			result = FirstNativeThreadedFailure(result, cancelResult);
		const RenderResult fenceResult = FenceThreadedRender();
		result = FirstNativeThreadedFailure(result, fenceResult);
		RenderFrameOutcome resourceFailure;
		resourceFailure.recordCommandFailure(RENDER_RESULT_FAILED);
		resourceFailure.setOperational(true);
		RememberThreadedFailure(resourceFailure,
			static_cast<NativeW3DSubmissionSequence>(
				~static_cast<NativeW3DSubmissionSequence>(0)));
		m_asyncResourceFailure = false;
	}
	return result;
}

rts::render::RenderResult NativeW3D2::RecoverDevice()
{
	if (rts::render::IsNativeGameRenderOwnerPinnedByCurrentThread() ||
		m_borrowedBackend || !m_resources.IsOwnerThread() ||
		!m_renderer.CanRecoverDevice())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	// Hold the lifecycle authority over the complete recovery transaction,
	// including the pre-recovery frame fence. A concurrent shutdown/replace
	// must not detach the aggregate between this admission check and the
	// cleanup/reacquire callbacks below.
	rts::render::NativeGameRenderOwnerLifecycleScope lifecycleScope;
	if (!lifecycleScope.IsAcquired())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	// A producer frame may still be open when an asynchronous completion reports
	// removal. Seal it as a failed non-visible frame before the lifecycle fence;
	// otherwise the threaded backend retains its reserved completion slot and a
	// later recovery/shutdown can never quiesce the owner.
	if (m_renderer.IsThreaded() && m_renderer.IsFrameOpen())
	{
		const rts::render::RenderResult cancelResult =
			CancelOpenThreadedFrame(rts::render::RENDER_RESULT_DEVICE_REMOVED);
		if (cancelResult != rts::render::RENDER_RESULT_OK)
			return cancelResult;
	}
	if (m_renderer.IsThreaded())
	{
		const rts::render::RenderResult fenceResult = FenceThreadedRender();
		if (fenceResult != rts::render::RENDER_RESULT_OK &&
			m_renderer.IsBackendOperational())
			return fenceResult;
	}
	return RecoverOwnedDevice();
}

rts::render::RenderResult NativeW3D2::RecoverOwnedDevice()
{
	if (m_borrowedBackend || !m_resources.IsOwnerThread() ||
		!m_renderer.CanRecoverDevice())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	m_gameResourcesOperational = false;
	m_activeRenderTargetKind = rts::render::GAME_RENDER_TARGET_UNKNOWN;
	m_rebuildingResources = true;
	if (m_gameCaptureQueue.bindOwnerThread())
	{
		m_gameCaptureQueue.cancelCurrent(rts::render::RENDER_RESULT_DEVICE_REMOVED);
		m_gameCaptureRequest.clear();
	}
	if (!InvokeGameCleanupRelease(m_gameCleanupHook))
	{
		m_rebuildingResources = false;
		m_gameCaptureRequest.clear();
		RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		return rts::render::RENDER_RESULT_FAILED;
	}
	const rts::render::RenderResult result = m_renderer.RecoverDevice();
	// ReAcquire is deliberately called only after the native backend has
	// completed recovery.  A failed recovery tears down the owner, so calling
	// into a retained game resource hook at that point would recreate objects
	// against a dead device. The rebuild flag admits only resource recreation;
	// frame/draw entry points still require m_gameResourcesOperational.
	if (result == rts::render::RENDER_RESULT_OK &&
		!InvokeGameCleanupReAcquire(m_gameCleanupHook))
	{
		m_rebuildingResources = false;
		m_gameResourcesOperational = false;
		m_activeRenderTargetKind = rts::render::GAME_RENDER_TARGET_UNKNOWN;
		if (m_gameCaptureQueue.bindOwnerThread())
		{
			m_gameCaptureQueue.cancelCurrent(rts::render::RENDER_RESULT_FAILED);
			m_gameCaptureRequest.clear();
		}
		RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		return rts::render::RENDER_RESULT_FAILED;
	}
	m_rebuildingResources = false;
	if (result == rts::render::RENDER_RESULT_OK)
	{
		// Native D3D11 recovery recreates and binds the swap-chain back buffer;
		// any prior render-to-texture binding is no longer active. The title must
		// issue a fresh SET_RENDER_TARGET command before using a texture target.
		m_activeRenderTargetKind = rts::render::GAME_RENDER_TARGET_BACK_BUFFER;
		m_gameResourcesOperational = true;
		m_asyncResourceFailure = false;
	}
	else
	{
		m_gameResourcesOperational = false;
		m_activeRenderTargetKind = rts::render::GAME_RENDER_TARGET_UNKNOWN;
	}
	if (result != rts::render::RENDER_RESULT_OK)
	{
		RecordGameFailure(result);
	}
	return result;
}

rts::render::RenderResult NativeW3D2::Shutdown()
{
	if (rts::render::IsNativeGameRenderOwnerPinnedByCurrentThread())
	{
		// A command scope pins this aggregate through its complete virtual call.
		// Teardown from a callback would otherwise clear the very owner that is
		// still executing and leave the caller dereferencing detached state.
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_renderer.HasBackendState() && !m_resources.IsOwnerThread())
	{
		// Do not bind/cancel/unpublish anything from an arbitrary thread.  The
		// resource state and all backend interfaces are owned by the render thread.
		fputs("NativeW3D2::Shutdown called off owner thread\n", stderr);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_borrowedBackend && m_renderer.IsFrameOpen())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	// Pin the complete drain/callback/teardown transaction. Queries may
	// recurse on this thread, but lifecycle mutation must wait or be rejected.
	rts::render::NativeGameRenderOwnerLifecycleScope ownerLifecycleScope;
	if (!ownerLifecycleScope.IsAcquired())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	if (!m_borrowedBackend && m_renderer.IsThreaded())
	{
		// Owned threaded shutdown must close the producer packet and wait for the
		// owner before resource metadata is detached.  A device-removal result is
		// retained for diagnostics, but does not justify abandoning accepted
		// packets or leaving the worker alive behind a destroyed aggregate.
		const rts::render::RenderResult cancelResult =
			CancelOpenThreadedFrame(rts::render::RENDER_RESULT_FAILED);
		if (cancelResult != rts::render::RENDER_RESULT_OK &&
			cancelResult != rts::render::RENDER_RESULT_INVALID_ARGUMENT)
			m_gameFailure.record(cancelResult);
		const rts::render::RenderResult fenceResult = FenceThreadedRender();
		if (fenceResult != rts::render::RENDER_RESULT_OK)
			m_gameFailure.record(fenceResult);
	}
	// Cancel retained callbacks while the still-published owner is pinned.
	if (m_gameCaptureQueue.bindOwnerThread())
		m_gameCaptureQueue.cancelCurrent(rts::render::RENDER_RESULT_FAILED);
	// Unpublish before any backend/resource teardown and keep the publication
	// gate held across the entire transition. Native wrappers either complete
	// before this scope or observe the null owner; they cannot dereference a
	// detached aggregate.
	if (ownerLifecycleScope.Get() == this)
		ownerLifecycleScope.Publish(0);
	m_activeRenderTargetKind = rts::render::GAME_RENDER_TARGET_UNKNOWN;
	m_gameResourcesOperational = false;
	m_nativeSortingRenderer.Clear();
	m_gameFailure.reset();
	m_deferredFailure = rts::render::RenderFrameOutcome();
	m_deferredFailureSequence = 0;
	m_recoveredFailureSequence = 0;
	m_asyncResourceFailure = false;
	m_rebuildingResources = false;
	// Quiesce every Line3D caller before touching either the resource table or
	// its backend context.  The gate is held across the complete transition, so
	// a new render cannot observe a published context while resources are being
	// destroyed.  Drain registered per-line caches first; their handles then
	// become empty and remain reclaimable by Line3D objects destroyed later.
	rts::render::NativeLine3DSubmitterLifecycleScope lifecycleScope;
	const bool wasPublished = lifecycleScope.Get() == &m_line3DContext;
	if (wasPublished)
	{
		lifecycleScope.Publish(0);
	}
	m_line3DContext.DrainLine3D();
	const rts::render::RenderResult unbindResult =
		rts::render::UnbindNativeW3DBufferResources(&m_resources);
	if (unbindResult != rts::render::RENDER_RESULT_OK)
		return unbindResult;
	const rts::render::RenderResult resourcesResult = m_resources.Shutdown();
	if (resourcesResult != rts::render::RENDER_RESULT_OK)
	{
		// The table is still live after a failed shutdown. Restore the binding so
		// dynamic-buffer owners retain a valid retry path, while keeping the
		// public owner unpublished until the caller retries teardown.
		rts::render::BindNativeW3DBufferResources(&m_resources);
		return resourcesResult;
	}
	m_renderer.m_recoveryResources = 0;
	if (m_borrowedBackend)
	{
		const rts::render::RenderResult hostResult = m_resourceHost.Detach();
		if (hostResult == rts::render::RENDER_RESULT_OK)
		{
			const rts::render::RenderResult rendererResult =
				m_renderer.DetachBorrowedState();
			if (rendererResult != rts::render::RENDER_RESULT_OK)
			{
				return rendererResult;
			}
			m_borrowedBackend = false;
		}
		return hostResult;
	}
	return m_renderer.Shutdown();
}

bool NativeW3D2::IsInitialized() const
{
	return m_renderer.IsInitialized() || IsAttachedToBorrowedBackend();
}

bool NativeW3D2::IsOperational() const
{
	return IsInitialized() && m_gameResourcesOperational &&
		m_activeRenderTargetKind !=
		rts::render::GAME_RENDER_TARGET_UNKNOWN;
}

bool NativeW3D2::IsRebuildingResources() const
{
	// The title cleanup hook spans two phases: ReleaseResources runs while the
	// old backend may already be unavailable, then ReAcquireResources runs after
	// recovery. Expose the owner-thread lifecycle window to logical resource
	// deletion in both phases; actual native creation remains guarded by the
	// stricter CanRebuildResources() predicate below.
	return m_rebuildingResources && m_resources.IsOwnerThread();
}

rts::render::GameRenderTargetKind NativeW3D2::ActiveRenderTargetKind() const
{
	return m_activeRenderTargetKind;
}

rts::render::RenderResult NativeW3D2::FinishGameRenderFrame(bool capture,
	bool present)
{
	using namespace rts::render;
	if (!m_renderer.IsFrameOpen())
	{
		const RenderResult result = RENDER_RESULT_INVALID_ARGUMENT;
		if (capture && m_gameCaptureQueue.bindOwnerThread())
			m_gameCaptureQueue.cancelCurrent(result);
		if (capture)
			m_gameCaptureRequest.clear();
		RecordGameFailure(result);
		return result;
	}

	// D3D11 capture maps a staging copy only after the immediate frame has
	// ended. Keep teardown, readback, and presentation as three explicit owner
	// operations so a failed stage can cancel the outstanding callbacks without
	// presenting a partially completed frame. A non-visible frame must never
	// consume a visible capture request.
	const bool requestedCapture = capture && present &&
		m_activeRenderTargetKind == GAME_RENDER_TARGET_BACK_BUFFER &&
		m_gameCaptureQueue.bindOwnerThread() &&
		m_gameCaptureQueue.pendingCount() != 0U;
	const RenderResult endResult = m_renderer.EndFrame(false);
	if (endResult != RENDER_RESULT_OK)
	{
		// NativeW3DRenderer seals a failed threaded frame itself. Calling
		// FinalizeEndedFrame again here would submit an empty second packet (and
		// can hide the original failure behind INVALID_ARGUMENT). Keep callback
		// cancellation tied to the first failure and leave the facade in lockstep.
		RecordGameFailure(endResult);
		if (capture && m_gameCaptureQueue.bindOwnerThread())
			m_gameCaptureQueue.cancelCurrent(endResult);
		if (capture)
			m_gameCaptureRequest.clear();
		return endResult;
	}

	RenderBackBufferInfo captureInfo;
	std::vector<unsigned char> capturePixels;
	size_t captureRowPitch = 0;
	RenderFormat captureFormat = RENDER_FORMAT_UNKNOWN;
	if (requestedCapture)
	{
		const RenderResult captureResult = PrepareGameBackBufferCapture(
			&captureInfo, &capturePixels, &captureRowPitch, &captureFormat);
		if (captureResult != RENDER_RESULT_OK)
		{
			// PrepareGameBackBufferCapture cancels the queue before returning a
			// readback failure. Latch it at the aggregate boundary so the title
			// adapter cannot mistake a failed capture for a visible frame.
			RecordGameFailure(captureResult);
			const RenderResult finalizeResult =
				m_renderer.FinalizeEndedFrame(false);
			if (finalizeResult != RENDER_RESULT_OK)
				RecordGameFailure(finalizeResult);
			return captureResult;
		}
	}

	// Queue admission is not execution success in the parallel owner. Keep the
	// sequence around the final submission and fence only when a capture needs
	// the actual presented-frame completion. Ordinary frames remain overlapped;
	// SYNC_RENDERER and the next owner boundary poll non-blockingly.
	const uint64_t previousSubmission =
		m_renderer.LastThreadedSubmissionSequence();
	const RenderResult finalizeResult = m_renderer.FinalizeEndedFrame(present);
	if (finalizeResult != RENDER_RESULT_OK)
	{
		if (requestedCapture && m_gameCaptureQueue.bindOwnerThread())
			m_gameCaptureQueue.cancelCurrent(finalizeResult);
		RecordGameFailure(finalizeResult);
		return finalizeResult;
	}

	if (!requestedCapture)
		return RENDER_RESULT_OK;

	RenderResult completionResult = RENDER_RESULT_OK;
	bool wasPresented = present;
	if (m_renderer.IsThreaded())
	{
		// Capture readback is a deliberate CPU owner fence. Drain the final
		// packet, then poll through the aggregate so failed resource ranges and
		// the matching presentation outcome are published before callbacks run.
		const RenderResult drainResult = m_renderer.DrainThreaded();
		ThreadedRenderFrameCompletion completion;
		const NativeW3DSubmissionSequence submission =
			m_renderer.LastThreadedSubmissionSequence();
		const RenderResult pollResult = PollThreadedCompletions(submission,
			&completion);
		completionResult = FirstNativeThreadedFailure(drainResult, pollResult);
		if (submission == 0 || submission == previousSubmission ||
			completion.sequence != submission)
		{
			completionResult = FirstNativeThreadedFailure(completionResult,
				RENDER_RESULT_FAILED);
		}
		else if (completion.result != RENDER_RESULT_OK)
		{
			completionResult = FirstNativeThreadedFailure(completionResult,
				completion.result);
		}
		wasPresented = completion.result == RENDER_RESULT_OK &&
			completion.presented && completion.operational;
	}
	if (completionResult != RENDER_RESULT_OK || !wasPresented)
	{
		const RenderResult cancellation = completionResult != RENDER_RESULT_OK ?
			completionResult : RENDER_RESULT_FAILED;
		if (m_gameCaptureQueue.bindOwnerThread())
			m_gameCaptureQueue.cancelCurrent(cancellation);
		RecordGameFailure(cancellation);
		return cancellation;
	}

	const RenderResult captureResult = CompleteGameBackBufferCaptures(
		captureInfo, captureRowPitch, captureFormat, capturePixels);
	if (captureResult != RENDER_RESULT_OK)
		RecordGameFailure(captureResult);
	return captureResult;
}

rts::render::RenderResult NativeW3D2::ExecuteGameRenderCommand(
	const rts::render::GameRenderCommand &command)
{
	using namespace rts::render;
	// Completion publication is an owner-boundary operation. Service before the
	// operational gate so an async removal can recover the owned device instead
	// of being hidden by IsInitialized()/IsOperational() returning false.
	const RenderResult serviceResult = ServiceThreadedCompletions();
	if (serviceResult != RENDER_RESULT_OK &&
		!m_renderer.IsBackendOperational())
	{
		RecordGameFailure(serviceResult);
		return serviceResult;
	}

	if (command.type == GAME_RENDER_COMMAND_INVALID ||
		!IsWellFormedGameHandle(command.resource0) ||
		!IsWellFormedGameHandle(command.resource1) ||
		!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	if (command.type == GAME_RENDER_COMMAND_SET_DEVICE_BY_NAME ||
		command.type == GAME_RENDER_COMMAND_SET_DEVICE_BY_INDEX ||
		command.type == GAME_RENDER_COMMAND_SET_ANY_DEVICE ||
		command.type == GAME_RENDER_COMMAND_SET_NEXT_DEVICE ||
		command.type == GAME_RENDER_COMMAND_TOGGLE_WINDOWED)
	{
		// Device enumeration and adapter switching belong to the bootstrap
		// owner.  NativeW3D2 borrows or owns one already-selected device and
		// cannot truthfully fabricate an alternate device behind this command.
		RecordGameFailure(RENDER_RESULT_UNSUPPORTED);
		return RENDER_RESULT_UNSUPPORTED;
	}

	const GpuHandle resource0 = ToGameGpuHandle(command.resource0);
	const GpuHandle resource1 = ToGameGpuHandle(command.resource1);

	switch (command.type)
	{
	case GAME_RENDER_COMMAND_SET_TEXTURE:
		if (command.value0 >= LEGACY_TEXTURE_STAGE_COUNT)
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (resource0.isValid() && !m_resources.IsValid(resource0))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_gameTextures[command.value0] = resource0;
		if (!TrackLegacyTexturePresence(command.value0, resource0.isValid()))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_MATERIAL:
		if (command.input == 0 || command.inputBytes !=
			sizeof(LegacyVertexMaterialState))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		{
			const LegacyVertexMaterialState &material =
				*static_cast<const LegacyVertexMaterialState *>(command.input);
			if ((material.textureStageResetMask &
					~((1U << LEGACY_TEXTURE_STAGE_COUNT) - 1U)) != 0U ||
				!IsValidGameMaterialSource(material.ambientMaterialSource) ||
				!IsValidGameMaterialSource(material.diffuseMaterialSource) ||
				!IsValidGameMaterialSource(material.emissiveMaterialSource) ||
				!IsFiniteGameColor(material.material.diffuse) ||
				!IsFiniteGameColor(material.material.ambient) ||
				!IsFiniteGameColor(material.material.specular) ||
				!IsFiniteGameColor(material.material.emissive) ||
				!IsFiniteGameFloat(material.material.specularPower))
			{
				RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			for (unsigned int stage = 0;
				stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
			{
				if (material.textureCoordinateIndex[stage] >=
					LEGACY_TEXTURE_STAGE_COUNT)
				{
					RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
					return RENDER_RESULT_INVALID_ARGUMENT;
				}
			}
			LegacyLogicalState logical;
			if (!GetTrackedLegacyLogicalState(&logical))
				logical = LegacyLogicalState();
			logical.pipeline.lightingEnable = material.lightingEnable;
			logical.pipeline.ambientMaterialSource =
				material.ambientMaterialSource;
			logical.pipeline.diffuseMaterialSource =
				material.diffuseMaterialSource;
			logical.pipeline.emissiveMaterialSource =
				material.emissiveMaterialSource;
			logical.constants.material = material.material;
			for (unsigned int stage = 0;
				stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
			{
				if ((material.textureStageResetMask & (1U << stage)) != 0U)
				{
					LegacyTextureStageState &stageState =
						logical.pipeline.textureStages[stage];
					stageState = LegacyTextureStageState();
					stageState.textureCoordinateIndex =
						material.textureCoordinateIndex[stage];
				}
			}
			TrackLegacyPipelineState(logical.pipeline);
			TrackLegacyMaterial(material.material);
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_LIGHT:
		if (command.input == 0 || command.inputBytes != sizeof(LegacyLightState))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return SetGameLightState(command.value0,
			*static_cast<const LegacyLightState *>(command.input));

	case GAME_RENDER_COMMAND_SET_TRANSFORM:
		if (command.value0 >= LEGACY_TRANSFORM_COUNT || command.input == 0 ||
			command.inputBytes != sizeof(RenderMatrix4))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (!GetTrackedLegacyPipelineState(0))
			SeedTrackedLegacyPipelineState();
		if (!TrackLegacyTransform(
				static_cast<LegacyTransformSlot>(command.value0),
				static_cast<const RenderMatrix4 *>(command.input)->values))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_GET_TRANSFORM:
		if (command.value0 >= LEGACY_TRANSFORM_COUNT || command.output == 0 ||
			command.outputBytes != sizeof(RenderMatrix4))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		{
			LegacyLogicalState logical;
			if (!GetTrackedLegacyLogicalState(&logical))
			{
				RecordGameFailure(RENDER_RESULT_FAILED);
				return RENDER_RESULT_FAILED;
			}
			const RenderMatrix4 *source = 0;
			switch (command.value0)
			{
			case LEGACY_TRANSFORM_WORLD: source = &logical.constants.world; break;
			case LEGACY_TRANSFORM_VIEW: source = &logical.constants.view; break;
			case LEGACY_TRANSFORM_PROJECTION:
				source = &logical.constants.projection; break;
			default:
				source = &logical.constants.textureTransforms[
					command.value0 - LEGACY_TRANSFORM_TEXTURE0];
				break;
			}
			*static_cast<RenderMatrix4 *>(command.output) = *source;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS:
		if (command.input == 0 || command.inputBytes != sizeof(RenderMatrix4) ||
			!IsFiniteGameFloat(command.float0) ||
			!IsFiniteGameFloat(command.float1) || command.float0 < 0.0f ||
			command.float1 < command.float0)
			goto invalid_command;
		{
			RenderMatrix4 projection =
				*static_cast<const RenderMatrix4 *>(command.input);
			for (unsigned int index = 0; index < 16; ++index)
			{
				if (!IsFiniteGameFloat(projection.values[index]))
					goto invalid_command;
			}
			LegacyLogicalState logical;
			if (!GetTrackedLegacyLogicalState(&logical))
				logical = LegacyLogicalState();
			if (!SupportsZBias() && command.float0 != command.float1)
			{
				const float depthRange = command.float1 - command.float0;
				if (depthRange == 0.0f || !IsFiniteGameFloat(depthRange))
					goto invalid_command;
				const float bias = static_cast<float>(
					logical.pipeline.rasterizer.depthBias) * (1.0f / 16.0f) /
					depthRange;
				if (!IsFiniteGameFloat(bias))
					goto invalid_command;
				projection.values[10] -= bias * projection.values[14];
				if (!IsFiniteGameFloat(projection.values[10]))
					goto invalid_command;
			}
			if (!TrackLegacyTransform(LEGACY_TRANSFORM_PROJECTION,
				projection.values))
				goto invalid_command;
			m_gameCameraProjection = projection;
			m_gameCameraNear = command.float0;
			m_gameCameraFar = command.float1;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_TEXTURE_STAGE_STATE:
		if (command.value0 >= LEGACY_TEXTURE_STAGE_COUNT || command.value1 >
			static_cast<unsigned int>(GAME_TEXTURE_STAGE_MAX_ANISOTROPY))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		{
			LegacyLogicalState logical;
			if (!GetTrackedLegacyLogicalState(&logical))
				logical = LegacyLogicalState();
			LegacyTextureStageState &stage =
				logical.pipeline.textureStages[command.value0];
			RenderTextureArgument argument;
			bool complement = false;
			bool alphaReplicate = false;
			switch (command.value1)
			{
			case GAME_TEXTURE_STAGE_COLOR_ARGUMENT0:
				if (!DecodeGameTextureArgument(command.value2, &argument,
					&complement, &alphaReplicate)) goto invalid_command;
				stage.colorArgument0 = argument;
				stage.colorArgument0Complement = complement;
				stage.colorArgument0AlphaReplicate = alphaReplicate;
				break;
			case GAME_TEXTURE_STAGE_COLOR_ARGUMENT1:
				if (!DecodeGameTextureArgument(command.value2, &argument,
					&complement, &alphaReplicate)) goto invalid_command;
				stage.colorArgument1 = argument;
				stage.colorArgument1Complement = complement;
				stage.colorArgument1AlphaReplicate = alphaReplicate;
				break;
			case GAME_TEXTURE_STAGE_COLOR_ARGUMENT2:
				if (!DecodeGameTextureArgument(command.value2, &argument,
					&complement, &alphaReplicate)) goto invalid_command;
				stage.colorArgument2 = argument;
				stage.colorArgument2Complement = complement;
				stage.colorArgument2AlphaReplicate = alphaReplicate;
				break;
			case GAME_TEXTURE_STAGE_ALPHA_ARGUMENT0:
				if (!DecodeGameTextureArgument(command.value2, &argument,
					&complement, &alphaReplicate)) goto invalid_command;
				stage.alphaArgument0 = argument;
				stage.alphaArgument0Complement = complement;
				stage.alphaArgument0AlphaReplicate = alphaReplicate;
				break;
			case GAME_TEXTURE_STAGE_ALPHA_ARGUMENT1:
				if (!DecodeGameTextureArgument(command.value2, &argument,
					&complement, &alphaReplicate)) goto invalid_command;
				stage.alphaArgument1 = argument;
				stage.alphaArgument1Complement = complement;
				stage.alphaArgument1AlphaReplicate = alphaReplicate;
				break;
			case GAME_TEXTURE_STAGE_ALPHA_ARGUMENT2:
				if (!DecodeGameTextureArgument(command.value2, &argument,
					&complement, &alphaReplicate)) goto invalid_command;
				stage.alphaArgument2 = argument;
				stage.alphaArgument2Complement = complement;
				stage.alphaArgument2AlphaReplicate = alphaReplicate;
				break;
			case GAME_TEXTURE_STAGE_COLOR_OPERATION:
				if (command.value2 > static_cast<unsigned int>(
					RENDER_TEXTURE_OP_LINEAR_INTERPOLATE)) goto invalid_command;
				stage.colorOperation = static_cast<RenderTextureOperation>(
					command.value2);
				break;
			case GAME_TEXTURE_STAGE_ALPHA_OPERATION:
				if (command.value2 > static_cast<unsigned int>(
					RENDER_TEXTURE_OP_LINEAR_INTERPOLATE)) goto invalid_command;
				stage.alphaOperation = static_cast<RenderTextureOperation>(
					command.value2);
				break;
			case GAME_TEXTURE_STAGE_ADDRESS_U:
			case GAME_TEXTURE_STAGE_ADDRESS_V:
			case GAME_TEXTURE_STAGE_ADDRESS_W:
				if (command.value2 > static_cast<unsigned int>(
					RENDER_TEXTURE_ADDRESS_BORDER)) goto invalid_command;
				if (command.value1 == GAME_TEXTURE_STAGE_ADDRESS_U)
					stage.sampler.addressU = static_cast<RenderTextureAddressMode>(
						command.value2);
				else if (command.value1 == GAME_TEXTURE_STAGE_ADDRESS_V)
					stage.sampler.addressV = static_cast<RenderTextureAddressMode>(
						command.value2);
				else
					stage.sampler.addressW = static_cast<RenderTextureAddressMode>(
						command.value2);
				break;
			case GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER:
			case GAME_TEXTURE_STAGE_MINIFICATION_FILTER:
			case GAME_TEXTURE_STAGE_MIP_FILTER:
				if (command.value2 > static_cast<unsigned int>(
					RENDER_TEXTURE_FILTER_ANISOTROPIC)) goto invalid_command;
				if (command.value1 == GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER)
					stage.sampler.magnification = static_cast<RenderTextureFilter>(
						command.value2);
				else if (command.value1 == GAME_TEXTURE_STAGE_MINIFICATION_FILTER)
					stage.sampler.minification = static_cast<RenderTextureFilter>(
						command.value2);
				else
					stage.sampler.mipmapping = static_cast<RenderTextureFilter>(
						command.value2);
				break;
			case GAME_TEXTURE_STAGE_COORDINATE_INDEX:
				if ((command.value2 & ~0x000300ffU) != 0U ||
					(command.value2 & 0xffU) >= LEGACY_TEXTURE_STAGE_COUNT)
					goto invalid_command;
				stage.textureCoordinateIndex = command.value2 & 0xffU;
				stage.cameraSpaceNormal = (command.value2 &
					GAME_TEXTURE_COORDINATE_CAMERA_NORMAL) != 0U;
				stage.cameraSpacePosition = (command.value2 &
					GAME_TEXTURE_COORDINATE_CAMERA_POSITION) != 0U;
				stage.cameraSpaceReflectionVector = (command.value2 &
					GAME_TEXTURE_COORDINATE_CAMERA_REFLECTION) != 0U;
				break;
			case GAME_TEXTURE_STAGE_TRANSFORM_FLAGS:
				if ((command.value2 & ~0x000001ffU) != 0U ||
					!IsLegacyProjectedTextureTransformValid(command.value2 & 0xffU,
						(command.value2 & GAME_TEXTURE_TRANSFORM_PROJECTED) != 0U))
					goto invalid_command;
				stage.textureTransformCount = command.value2 & 0xffU;
				stage.textureTransformEnable = stage.textureTransformCount != 0U;
				stage.projectedCoordinates = (command.value2 &
					GAME_TEXTURE_TRANSFORM_PROJECTED) != 0U;
				break;
			case GAME_TEXTURE_STAGE_MAX_ANISOTROPY:
				if (command.value2 == 0U)
					goto invalid_command;
				{
					GameTextureFilterCapabilities capabilities;
					if (GetTextureFilterCapabilities(&capabilities) !=
						RENDER_RESULT_OK || command.value2 >
						capabilities.maxAnisotropy)
					{
						RecordGameFailure(RENDER_RESULT_UNSUPPORTED);
						return RENDER_RESULT_UNSUPPORTED;
					}
				}
				stage.sampler.maximumAnisotropy = command.value2;
				break;
			default:
				goto invalid_command;
			}
			if (!TrackLegacyTextureStage(command.value0, stage))
				goto invalid_command;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_TEXTURE_BUMP_ENVIRONMENT:
		if (command.value0 >= LEGACY_TEXTURE_STAGE_COUNT ||
			!IsFiniteGameFloat(command.float0) ||
			!IsFiniteGameFloat(command.float1) ||
			!IsFiniteGameFloat(command.float2) ||
			!IsFiniteGameFloat(command.float3))
		{
			RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		{
			LegacyTextureStageState stage;
			if (!GetTrackedLegacyTextureStage(command.value0, &stage))
				stage = LegacyTextureStageState();
			stage.bumpEnvironmentMatrix00 = command.float0;
			stage.bumpEnvironmentMatrix01 = command.float1;
			stage.bumpEnvironmentMatrix10 = command.float2;
			stage.bumpEnvironmentMatrix11 = command.float3;
			if (!TrackLegacyTextureStage(command.value0, stage))
				goto invalid_command;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_VERTEX_BUFFER:
		if (resource0.isValid() && command.value3 != 0U)
		{
			RecordGameFailure(RENDER_RESULT_UNSUPPORTED);
			return RENDER_RESULT_UNSUPPORTED;
		}
		if (!resource0.isValid())
		{
			// A null handle with no input explicitly unbinds the immediate
			// stream.  With input, this is the sorted-stream form: copy the
			// selected source range while the caller owns the bytes, then use
			// that owner copy for the later sorted draw.  Do all validation and
			// allocation before replacing the previous binding.
			if (command.input != 0 || command.inputBytes != 0U)
			{
				size_t expectedBytes = 0;
				size_t expectedOffset = 0;
				RenderVertexLayout sortedLayout;
				if (command.input == 0 || command.inputBytes == 0U ||
					command.value1 == 0U || command.value3 == 0U ||
					command.value2 > 65535U || command.value3 > 65535U ||
					command.value3 > 65536U - command.value2 ||
					!CheckedGameSizeMultiply(static_cast<size_t>(command.value3),
						static_cast<size_t>(command.value1), &expectedBytes) ||
					!CheckedGameSizeMultiply(static_cast<size_t>(command.value2),
						static_cast<size_t>(command.value1), &expectedOffset) ||
					expectedOffset > static_cast<size_t>(UINT_MAX) ||
					command.value4 != static_cast<unsigned int>(expectedOffset) ||
					command.inputBytes != expectedBytes ||
					!DecodeLegacyFvfVertexLayout(command.value0, command.value1,
						&sortedLayout))
					goto invalid_command;
				try
				{
					std::vector<unsigned char> copied(expectedBytes);
					memcpy(copied.data(), command.input, expectedBytes);
					m_gameSortedVertexBytes.swap(copied);
				}
				catch (const std::bad_alloc &)
				{
					RecordGameFailure(RENDER_RESULT_OUT_OF_MEMORY);
					return RENDER_RESULT_OUT_OF_MEMORY;
				}
				catch (...)
				{
					RecordGameFailure(RENDER_RESULT_FAILED);
					return RENDER_RESULT_FAILED;
				}
				m_gameSortedVertexMinimum = command.value2;
				m_gameSortedVertexCount = command.value3;
				m_gameSortedVertexSourceOffset = command.value4;
				m_gameVertexBuffer = GpuHandle();
				m_gameVertexBound = false;
				m_gameVertexStride = command.value1;
				m_gameVertexOffset = 0;
				m_gameVertexStream = 0;
				m_gameVertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
				m_gameVertexLayout = sortedLayout;
				return RENDER_RESULT_OK;
			}
			m_gameVertexBuffer = GpuHandle();
			m_gameVertexBound = false;
			m_gameVertexStride = 0;
			m_gameVertexOffset = 0;
			m_gameVertexLayout = RenderVertexLayout();
			m_gameSortedVertexBytes.clear();
			m_gameSortedVertexMinimum = 0;
			m_gameSortedVertexCount = 0;
			m_gameSortedVertexSourceOffset = 0;
			return RENDER_RESULT_OK;
		}
		{
			if (command.input != 0 || command.inputBytes != 0U)
				goto invalid_command;
			NativeW3DBufferDescription description;
			if (!m_resources.IsValid(resource0) ||
				m_resources.DescribeBuffer(resource0, &description) !=
				RENDER_RESULT_OK || (description.descriptor.binding &
				RENDER_BUFFER_VERTEX) == 0U)
				goto invalid_command;
			const unsigned int stride = command.value1 != 0U ? command.value1 :
				description.descriptor.stride;
			if (command.value0 == 0U || stride == 0U ||
				!DecodeLegacyFvfVertexLayout(command.value0, stride,
					&m_gameVertexLayout))
				goto invalid_command;
			m_gameVertexBuffer = resource0;
			m_gameVertexStride = stride;
			m_gameVertexOffset = command.value2;
			m_gameVertexStream = command.value3;
			m_gameVertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
			m_gameVertexBound = true;
			m_gameSortedVertexBytes.clear();
			m_gameSortedVertexMinimum = 0;
			m_gameSortedVertexCount = 0;
			m_gameSortedVertexSourceOffset = 0;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_INDEX_BUFFER:
		if (!resource0.isValid())
		{
			// As with the vertex command, a null handle plus input selects the
			// bounded CPU image used by deferred triangle sorting.  The image
			// retains absolute R16 source indices; the selected start/count are
			// retained separately for exact draw validation.
			if (command.input != 0 || command.inputBytes != 0U)
			{
				size_t expectedBytes = 0;
				if (command.input == 0 || command.inputBytes == 0U ||
					command.value0 != RENDER_FORMAT_R16_UINT ||
					command.value2 == 0U || command.value1 > 65535U ||
					command.value2 > 65535U ||
					command.value2 > 65536U - command.value1 ||
					!CheckedGameSizeMultiply(static_cast<size_t>(command.value2),
						sizeof(unsigned short), &expectedBytes) ||
					command.inputBytes != expectedBytes)
					goto invalid_command;
				try
				{
					std::vector<unsigned char> copied(expectedBytes);
					memcpy(copied.data(), command.input, expectedBytes);
					m_gameSortedIndexBytes.swap(copied);
				}
				catch (const std::bad_alloc &)
				{
					RecordGameFailure(RENDER_RESULT_OUT_OF_MEMORY);
					return RENDER_RESULT_OUT_OF_MEMORY;
				}
				catch (...)
				{
					RecordGameFailure(RENDER_RESULT_FAILED);
					return RENDER_RESULT_FAILED;
				}
				m_gameSortedIndexStart = command.value1;
				m_gameSortedIndexCount = command.value2;
				m_gameIndexBuffer = GpuHandle();
				m_gameIndexBound = false;
				m_gameIndexOffset = 0;
				m_gameIndexBaseVertex = 0;
				m_gameIndexFormat = RENDER_FORMAT_R16_UINT;
				return RENDER_RESULT_OK;
			}
			m_gameIndexBuffer = GpuHandle();
			m_gameIndexBound = false;
			m_gameIndexOffset = 0;
			m_gameIndexBaseVertex = 0;
			m_gameSortedIndexBytes.clear();
			m_gameSortedIndexStart = 0;
			m_gameSortedIndexCount = 0;
			return RENDER_RESULT_OK;
		}
		if (command.input != 0 || command.inputBytes != 0U)
			goto invalid_command;
		if ((command.value0 != RENDER_FORMAT_R16_UINT &&
			command.value0 != RENDER_FORMAT_R32_UINT) ||
			!m_resources.IsValid(resource0))
			goto invalid_command;
		{
			NativeW3DBufferDescription description;
			if (m_resources.DescribeBuffer(resource0, &description) !=
				RENDER_RESULT_OK || (description.descriptor.binding &
				RENDER_BUFFER_INDEX) == 0U)
				goto invalid_command;
			const unsigned int alignment = command.value0 == RENDER_FORMAT_R16_UINT ?
				2U : 4U;
			if ((command.value1 % alignment) != 0U)
				goto invalid_command;
			m_gameIndexBuffer = resource0;
			m_gameIndexFormat = static_cast<RenderFormat>(command.value0);
			m_gameIndexOffset = command.value1;
			m_gameIndexBaseVertex = command.signedValue0;
			m_gameIndexBound = true;
			m_gameSortedIndexBytes.clear();
			m_gameSortedIndexStart = 0;
			m_gameSortedIndexCount = 0;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_INDEX_BUFFER_OFFSET:
		if (command.value0 > static_cast<unsigned int>(INT_MAX))
			goto invalid_command;
		m_gameIndexBaseVertex = static_cast<int>(command.value0);
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_APPLY_RENDER_STATE_CHANGES:
		if (!GetTrackedLegacyPipelineState(0))
			SeedTrackedLegacyPipelineState();
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_INVALIDATE_RENDER_STATE_CACHE:
		// Native draws bind the complete logical state on every submission, so
		// invalidating the cache means reseeding the serialized defaults rather
		// than carrying a stale backend state into the next draw.
		SeedTrackedLegacyPipelineState();
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_SET_VERTEX_SHADER_CONSTANTS:
	case GAME_RENDER_COMMAND_SET_PIXEL_SHADER_CONSTANTS:
		{
			const unsigned int maximum = command.type ==
				GAME_RENDER_COMMAND_SET_VERTEX_SHADER_CONSTANTS ?
				LEGACY_VERTEX_CONSTANT_COUNT : LEGACY_PIXEL_CONSTANT_COUNT;
			size_t expectedBytes = 0;
			if (command.value1 == 0U || command.value0 >= maximum ||
				command.value1 > maximum - command.value0 || command.input == 0 ||
				!CheckedGameSizeMultiply(static_cast<size_t>(command.value1),
					sizeof(RenderFloat4), &expectedBytes) ||
				command.inputBytes != expectedBytes)
				goto invalid_command;
			if (!GetTrackedLegacyPipelineState(0))
				SeedTrackedLegacyPipelineState();
			const float *values = static_cast<const float *>(command.input);
			const bool tracked = command.type ==
				GAME_RENDER_COMMAND_SET_VERTEX_SHADER_CONSTANTS ?
				TrackLegacyVertexShaderConstants(command.value0, values,
					command.value1) : TrackLegacyPixelShaderConstants(command.value0,
					values, command.value1);
			if (!tracked)
				goto invalid_command;
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_DRAW_TRIANGLES:
	case GAME_RENDER_COMMAND_DRAW_STRIP:
		if (!m_gameVertexBound || !m_gameIndexBound ||
			command.value3 == 0U)
			goto invalid_command;
		{
			unsigned int indexCount = command.type ==
				GAME_RENDER_COMMAND_DRAW_TRIANGLES ? 0U : command.value1;
			if (command.type == GAME_RENDER_COMMAND_DRAW_TRIANGLES)
			{
				size_t indexCountSize = 0;
				if (!CheckedGameSizeMultiply(static_cast<size_t>(command.value1),
					3U, &indexCountSize) || indexCountSize > UINT_MAX)
					goto invalid_command;
				indexCount = static_cast<unsigned int>(indexCountSize);
			}
			if (indexCount == 0U)
				goto invalid_command;
			LegacyLogicalState state;
			if (!GetTrackedLegacyLogicalState(&state))
				goto invalid_command;
			NativeDrawPacket packet;
			packet.vertexBuffer = m_gameVertexBuffer;
			packet.indexBuffer = m_gameIndexBuffer;
			packet.vertexStride = m_gameVertexStride;
			packet.vertexOffset = m_gameVertexOffset;
			packet.indexOffset = m_gameIndexOffset;
			packet.indexFormat = m_gameIndexFormat;
			packet.vertexFormat = m_gameVertexFormat;
			packet.vertexLayout = m_gameVertexLayout;
			packet.topology = command.type == GAME_RENDER_COMMAND_DRAW_TRIANGLES ?
				RENDER_PRIMITIVE_TRIANGLE_LIST : RENDER_PRIMITIVE_TRIANGLE_STRIP;
			packet.texturePresenceMask = state.texturePresenceMask;
			packet.vertexCount = command.value3;
			packet.startVertex = command.value2;
			packet.indexCount = indexCount;
			packet.startIndex = command.value0;
			packet.minimumVertexIndex = command.value2;
			packet.baseVertex = m_gameIndexBaseVertex;
			packet.indexed = true;
			for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
				++stage)
				packet.textures[stage] = m_gameTextures[stage];
			const RenderResult result = SubmitGamePacket(state, packet);
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_DRAW_SORTED_TRIANGLES:
		if (m_gameSortedVertexBytes.empty() || m_gameSortedIndexBytes.empty() ||
			command.value1 == 0U || command.value2 > 65535U ||
			command.value3 == 0U || command.value3 > 65535U ||
			command.value2 > 65536U - command.value3 ||
			m_gameSortedVertexMinimum > 65535U ||
			m_gameSortedVertexCount == 0U ||
			m_gameSortedVertexMinimum >
				65536U - m_gameSortedVertexCount ||
			command.value2 < m_gameSortedVertexMinimum ||
			command.value2 - m_gameSortedVertexMinimum >
				m_gameSortedVertexCount ||
			command.value3 > m_gameSortedVertexCount -
				(command.value2 - m_gameSortedVertexMinimum) ||
			m_gameVertexStride == 0U)
			goto invalid_command;
		{
			size_t indexCountSize = 0;
			size_t vertexByteOffset = 0;
			size_t requestedSourceOffset = 0;
			size_t boundSourceOffset = 0;
			unsigned int relativeIndexStart = 0;
			const unsigned int relativeVertexStart = command.value2 -
				m_gameSortedVertexMinimum;
			if (!CheckedGameSizeMultiply(static_cast<size_t>(command.value1),
				3U, &indexCountSize) || indexCountSize == 0U ||
				indexCountSize > 65535U || indexCountSize >
				m_gameSortedIndexCount || command.value0 <
				m_gameSortedIndexStart)
				goto invalid_command;
			relativeIndexStart = command.value0 - m_gameSortedIndexStart;
			if (relativeIndexStart > m_gameSortedIndexCount ||
				indexCountSize > m_gameSortedIndexCount - relativeIndexStart ||
				!CheckedGameSizeMultiply(static_cast<size_t>(relativeVertexStart),
					static_cast<size_t>(m_gameVertexStride), &vertexByteOffset) ||
				!CheckedGameSizeMultiply(static_cast<size_t>(command.value2),
					static_cast<size_t>(m_gameVertexStride),
					&requestedSourceOffset) ||
				!CheckedGameSizeMultiply(static_cast<size_t>(
					m_gameSortedVertexMinimum), static_cast<size_t>(
					m_gameVertexStride), &boundSourceOffset) ||
				boundSourceOffset != static_cast<size_t>(
					m_gameSortedVertexSourceOffset) ||
				requestedSourceOffset < boundSourceOffset ||
				requestedSourceOffset - boundSourceOffset != vertexByteOffset ||
				vertexByteOffset > m_gameSortedVertexBytes.size() ||
				static_cast<size_t>(command.value3) >
					(m_gameSortedVertexBytes.size() - vertexByteOffset) /
					static_cast<size_t>(m_gameVertexStride) ||
				m_gameSortedIndexBytes.size() != static_cast<size_t>(
					m_gameSortedIndexCount) * sizeof(unsigned short) ||
				m_gameSortedVertexBytes.size() != static_cast<size_t>(
					m_gameSortedVertexCount) * m_gameVertexStride)
				goto invalid_command;
			LegacyLogicalState state;
			if (!GetTrackedLegacyLogicalState(&state))
				goto invalid_command;
			NativeDrawPacket packet;
			packet.vertexBuffer = GpuHandle();
			packet.indexBuffer = GpuHandle();
			packet.vertexStride = m_gameVertexStride;
			packet.vertexOffset = 0;
			packet.indexOffset = 0;
			packet.indexFormat = RENDER_FORMAT_R16_UINT;
			packet.vertexFormat = m_gameVertexFormat;
			packet.vertexLayout = m_gameVertexLayout;
			packet.topology = RENDER_PRIMITIVE_TRIANGLE_LIST;
			packet.texturePresenceMask = state.texturePresenceMask;
			packet.vertexCount = command.value3;
			packet.startVertex = 0;
			packet.indexCount = static_cast<unsigned int>(indexCountSize);
			packet.startIndex = command.value0;
			packet.minimumVertexIndex = command.value2;
			packet.baseVertex = 0;
			packet.indexed = true;
			for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
				++stage)
				packet.textures[stage] = m_gameTextures[stage];
			const RenderResult result = QueueGameSortedTriangles(state, packet,
				m_gameSortedVertexBytes.data() + vertexByteOffset,
				static_cast<size_t>(command.value3) * m_gameVertexStride,
				m_gameSortedIndexBytes.data() + static_cast<size_t>(
					relativeIndexStart) * sizeof(unsigned short),
				indexCountSize * sizeof(unsigned short),
				command.boundingSphere);
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_DRAW_PRIMITIVE_UP:
		if (command.input == 0 || command.inputBytes == 0U ||
			command.value2 == 0U || !IsValidGameTopology(command.value0) ||
			command.value0 == GAME_PRIMITIVE_POINT_LIST || command.value3 == 0U)
			goto invalid_command;
		{
			unsigned int vertexCount = 0;
			switch (command.value0)
			{
			case GAME_PRIMITIVE_TRIANGLE_LIST:
				if (command.value1 > UINT_MAX / 3U) goto invalid_command;
				vertexCount = command.value1 * 3U; break;
			case GAME_PRIMITIVE_TRIANGLE_STRIP:
				if (command.value1 > UINT_MAX - 2U) goto invalid_command;
				vertexCount = command.value1 + 2U; break;
			case GAME_PRIMITIVE_LINE_LIST:
				if (command.value1 > UINT_MAX / 2U) goto invalid_command;
				vertexCount = command.value1 * 2U; break;
			case GAME_PRIMITIVE_LINE_STRIP:
				if (command.value1 > UINT_MAX - 1U) goto invalid_command;
				vertexCount = command.value1 + 1U; break;
			default: goto invalid_command;
			}
			size_t expectedBytes = 0;
			if (vertexCount == 0U || !CheckedGameSizeMultiply(
				static_cast<size_t>(vertexCount), command.value2,
				&expectedBytes) || command.inputBytes != expectedBytes)
				goto invalid_command;
			RenderVertexLayout layout;
			if (!DecodeLegacyFvfVertexLayout(command.value3, command.value2,
				&layout))
				goto invalid_command;
			LegacyLogicalState state;
			if (!GetTrackedLegacyLogicalState(&state))
				goto invalid_command;
			BufferDescriptor descriptor;
			descriptor.byteCount = expectedBytes;
			descriptor.stride = command.value2;
			descriptor.binding = RENDER_BUFFER_VERTEX;
			descriptor.usage = RENDER_USAGE_DYNAMIC;
			GpuHandle temporary;
			RenderResult result = m_resources.CreateBuffer(descriptor,
				command.input, expectedBytes, &temporary);
			if (result != RENDER_RESULT_OK)
				return result;
			NativeDrawPacket packet;
			packet.vertexBuffer = temporary;
			packet.vertexStride = command.value2;
			packet.vertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
			packet.vertexLayout = layout;
			packet.topology = static_cast<RenderPrimitiveTopology>(command.value0);
			packet.texturePresenceMask = state.texturePresenceMask;
			packet.vertexCount = vertexCount;
			for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
				++stage)
				packet.textures[stage] = m_gameTextures[stage];
			result = SubmitGamePacket(state, packet);
			if (!m_resources.Destroy(temporary) && result == RENDER_RESULT_OK)
				result = RENDER_RESULT_FAILED;
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_SET_RENDER_TARGET:
		if (command.input == 0 || command.inputBytes !=
			sizeof(RenderTargetBinding))
			goto invalid_command;
		{
			const RenderTargetBinding &binding =
				*static_cast<const RenderTargetBinding *>(command.input);
			if (binding.hasColor && !binding.useBackBufferColor &&
				(!binding.color.resource.isValid() ||
					!m_resources.IsValid(binding.color.resource)))
				goto invalid_command;
			if (binding.hasDepth && !binding.useBackBufferDepth &&
				(!binding.depth.resource.isValid() ||
					!m_resources.IsValid(binding.depth.resource)))
				goto invalid_command;
			const RenderResult result = m_renderer.SetRenderTargetsExternal(binding);
			if (result != RENDER_RESULT_OK)
			{
				RecordGameFailure(result);
				return result;
			}
			m_activeRenderTargetKind = binding.hasColor &&
				!binding.useBackBufferColor ? GAME_RENDER_TARGET_TEXTURE :
				GAME_RENDER_TARGET_BACK_BUFFER;
			return RENDER_RESULT_OK;
		}

	case GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE:
	case GAME_RENDER_COMMAND_ACQUIRE_COPIED_TEXTURE_CONTENT:
		if (!resource0.isValid() || !m_resources.IsValid(resource0))
			goto invalid_command;
		{
			NativeW3DGpuContentLease localLease;
			NativeW3DGpuContentLease *lease = &localLease;
			if (command.output != 0)
			{
				if (command.outputBytes != sizeof(NativeW3DGpuContentLease))
					goto invalid_command;
				lease = static_cast<NativeW3DGpuContentLease *>(command.output);
			}
			const RenderResult result = command.type ==
				GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE ?
				m_resources.CopyActiveColorTargetToTexture(resource0, lease) :
				m_resources.AcquireGpuContentLease(resource0, lease);
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_BEGIN_RENDER:
		if (command.value0 & ~(RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH |
			RENDER_CLEAR_STENCIL) || !IsFiniteGameFloat(command.float0) ||
			!IsFiniteGameFloat(command.float1) || !IsFiniteGameFloat(command.float2) ||
			!IsFiniteGameFloat(command.float3) || !IsValidGameClearDepth(command.float4))
			goto invalid_command;
		{
			const RenderResult result = m_renderer.BeginFrame();
			if (result != RENDER_RESULT_OK)
			{
				RecordGameFailure(result);
				return result;
			}
			if (command.value0 != 0U)
			{
				const RenderResult clearResult = m_renderer.ClearExternal(command.value0,
					RenderFloat4(command.float0, command.float1, command.float2,
						command.float3), command.float4, command.value1);
				if (clearResult != RENDER_RESULT_OK)
				{
					m_renderer.RecordFrameFailure(clearResult);
					// EndFrame closes and seals a failed threaded recording. Do not
					// finalize a second time here; that would enqueue an empty packet
					// and replace the clear failure with INVALID_ARGUMENT.
					const RenderResult endResult = m_renderer.EndFrame(false);
					if (endResult != RENDER_RESULT_OK)
						RecordGameFailure(endResult);
					RecordGameFailure(clearResult);
					return clearResult;
				}
			}
		}
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_CLEAR_RENDER_TARGETS:
		if ((command.value0 & ~(RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH |
			RENDER_CLEAR_STENCIL)) != 0U || !IsFiniteGameFloat(command.float0) ||
			!IsFiniteGameFloat(command.float1) || !IsFiniteGameFloat(command.float2) ||
			!IsFiniteGameFloat(command.float3) || !IsValidGameClearDepth(command.float4))
			goto invalid_command;
		{
			const RenderResult result = m_renderer.ClearExternal(command.value0,
				RenderFloat4(command.float0, command.float1, command.float2,
					command.float3), command.float4, command.value1);
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_SET_AMBIENT_COLOR:
		if (!IsFiniteGameFloat(command.float0) ||
			!IsFiniteGameFloat(command.float1) || !IsFiniteGameFloat(command.float2) ||
			!IsFiniteGameFloat(command.float3))
			goto invalid_command;
		TrackLegacyGlobalAmbient(RenderFloat4(command.float0, command.float1,
			command.float2, command.float3));
		return RENDER_RESULT_OK;

	case GAME_RENDER_COMMAND_END_RENDER:
		return FinishGameRenderFrame(command.value0 != 0U,
			command.value0 != 0U);

	case GAME_RENDER_COMMAND_FLIP_RENDERER:
		if (!m_renderer.IsFrameOpen())
			goto invalid_command;
		return FinishGameRenderFrame(true, true);

	case GAME_RENDER_COMMAND_SET_RESOLUTION:
		if (command.value0 == 0U || command.value1 == 0U ||
			command.value2 != 0U && command.value2 != 1U)
			goto invalid_command;
		{
			if (m_borrowedBackend)
			{
				RecordGameFailure(RENDER_RESULT_UNSUPPORTED);
				return RENDER_RESULT_UNSUPPORTED;
			}
			if (m_renderer.IsFrameOpen())
			{
				RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			// Resize invalidates every title-owned default/present resource. Keep
			// the aggregate non-operational until the complete release/resize/
			// reacquire transaction has succeeded; callers must not render against
			// a partially restored resource graph.
			// The command boundary normally supplies the owner pin. Direct callers
			// acquire the lifecycle scope here so cleanup callbacks can query the
			// owner but cannot recursively tear it down.
			const bool ownerAlreadyPinned =
				rts::render::IsNativeGameRenderOwnerPinnedByCurrentThread();
			rts::render::NativeGameRenderOwnerLifecycleScope resizeLifecycleScope;
			if (!ownerAlreadyPinned && !resizeLifecycleScope.IsAcquired())
				return RENDER_RESULT_INVALID_ARGUMENT;
			m_rebuildingResources = true;
			m_gameResourcesOperational = false;
			m_activeRenderTargetKind = GAME_RENDER_TARGET_UNKNOWN;
			if (m_gameCaptureQueue.bindOwnerThread())
			{
				m_gameCaptureQueue.cancelCurrent(RENDER_RESULT_DEVICE_REMOVED);
				m_gameCaptureRequest.clear();
			}
			if (!InvokeGameCleanupRelease(m_gameCleanupHook))
			{
				m_rebuildingResources = false;
				m_gameCaptureRequest.clear();
				RecordGameFailure(RENDER_RESULT_FAILED);
				return RENDER_RESULT_FAILED;
			}
			RenderResult result = m_renderer.Resize(command.value0,
				command.value1);
			if (result == RENDER_RESULT_OK &&
				!InvokeGameCleanupReAcquire(m_gameCleanupHook))
			{
				result = RENDER_RESULT_FAILED;
				m_gameResourcesOperational = false;
				m_activeRenderTargetKind = GAME_RENDER_TARGET_UNKNOWN;
				if (m_gameCaptureQueue.bindOwnerThread())
				{
					m_gameCaptureQueue.cancelCurrent(result);
					m_gameCaptureRequest.clear();
				}
			}
			else if (result == RENDER_RESULT_OK)
			{
				// Resize rebinds the native swap-chain back buffer. A previous
				// texture target cannot remain active across that backend transition;
				// the title must explicitly select it again after reacquire.
				m_activeRenderTargetKind = GAME_RENDER_TARGET_BACK_BUFFER;
				m_gameResourcesOperational = true;
			}
			else
			{
				m_gameResourcesOperational = false;
				m_activeRenderTargetKind = GAME_RENDER_TARGET_UNKNOWN;
			}
			m_rebuildingResources = false;
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_CAPTURE_BACKBUFFER:
		if (command.output == 0 || command.outputBytes == 0U ||
			command.value0 == 0U)
			goto invalid_command;
		{
			RenderFormat format = RENDER_FORMAT_UNKNOWN;
			RenderFormat *formatOutput = &format;
			if (command.secondaryOutput != 0)
			{
				if (command.secondaryOutputBytes != sizeof(RenderFormat))
					goto invalid_command;
				formatOutput = static_cast<RenderFormat *>(command.secondaryOutput);
			}
			const RenderResult result = m_renderer.CaptureBackBuffer(command.output,
				command.outputBytes, command.value0, formatOutput);
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_GET_BACKBUFFER_INFO:
		if (command.output == 0 || command.outputBytes != sizeof(RenderBackBufferInfo))
			goto invalid_command;
		{
			const RenderResult result = m_renderer.GetBackBufferInfo(
				static_cast<RenderBackBufferInfo *>(command.output));
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_SET_SWAP_INTERVAL:
		if (command.signedLongValue < 0 ||
			command.signedLongValue > RENDER_SWAP_INTERVAL_MAX)
			goto invalid_command;
		{
			const RenderResult result = m_renderer.SetSwapInterval(
				static_cast<unsigned int>(command.signedLongValue));
			if (result != RENDER_RESULT_OK) RecordGameFailure(result);
			return result;
		}
	case GAME_RENDER_COMMAND_GET_SWAP_INTERVAL:
		if (command.output == 0 || command.outputBytes != sizeof(long))
			goto invalid_command;
		{
			unsigned int interval = 0;
			const RenderResult result = m_renderer.GetSwapInterval(&interval);
			if (result == RENDER_RESULT_OK)
				*static_cast<long *>(command.output) = static_cast<long>(interval);
			else
				RecordGameFailure(result);
			return result;
		}
	case GAME_RENDER_COMMAND_SET_GAMMA:
		if (!IsFiniteGameFloat(command.float0) ||
			!IsFiniteGameFloat(command.float1) ||
			!IsFiniteGameFloat(command.float2) || command.value0 > 1U)
			goto invalid_command;
		{
			// WW3D's four-argument API always used the legacy useLimit=true
			// default. Calibration is separate metadata, never the limit flag.
			const RenderResult result = m_renderer.SetGamma(command.float0,
				command.float1, command.float2, command.value0 != 0U, true);
			if (result != RENDER_RESULT_OK) RecordGameFailure(result);
			return result;
		}

	case GAME_RENDER_COMMAND_GET_RESOLUTION:
	case GAME_RENDER_COMMAND_GET_TARGET_RESOLUTION:
	case GAME_RENDER_COMMAND_GET_DEVICE_INDEX:
	case GAME_RENDER_COMMAND_GET_DEVICE_NAME:
	case GAME_RENDER_COMMAND_GET_DEVICE_COUNT:
	case GAME_RENDER_COMMAND_GET_DEVICE_DESC:
	case GAME_RENDER_COMMAND_GET_TEXTURE_BITDEPTH:
	case GAME_RENDER_COMMAND_GET_MSAA_MODE:
		// The concrete device-selection and display policy values remain owned by
		// the bootstrap adapter. They are intentionally not guessed from the
		// current swap-chain dimensions here.
		RecordGameFailure(RENDER_RESULT_UNSUPPORTED);
		return RENDER_RESULT_UNSUPPORTED;

	case GAME_RENDER_COMMAND_SYNC_RENDERER:
		// WW3D sync is a timing/observation command, not a GPU fence. Poll
		// completions that are already available, but preserve overlap and leave
		// explicit owner fences to capture, resize, recovery, and shutdown.
		(void)command.value0;
		{
			const RenderResult result = PollThreadedCompletions();
			if (result != RENDER_RESULT_OK)
				RecordGameFailure(result);
			return result;
		}
	case GAME_RENDER_COMMAND_SET_TEXTURE_BITDEPTH:
	case GAME_RENDER_COMMAND_SET_MSAA_MODE:
	case GAME_RENDER_COMMAND_SET_DEVICE_BY_NAME:
	case GAME_RENDER_COMMAND_SET_DEVICE_BY_INDEX:
	case GAME_RENDER_COMMAND_SET_ANY_DEVICE:
	case GAME_RENDER_COMMAND_SET_NEXT_DEVICE:
	case GAME_RENDER_COMMAND_TOGGLE_WINDOWED:
		RecordGameFailure(RENDER_RESULT_UNSUPPORTED);
		return RENDER_RESULT_UNSUPPORTED;

	default:
		break;
	}

invalid_command:
	RecordGameFailure(RENDER_RESULT_INVALID_ARGUMENT);
	return RENDER_RESULT_INVALID_ARGUMENT;
}

bool NativeW3D2::FindGameShaderAsset(const char *assetPath,
	bool vertexShader, rts::render::RenderLegacyPixelProgram *pixelProgram,
	rts::render::RenderLegacyVertexProgram *vertexProgram) const
{
	if (pixelProgram == 0 || vertexProgram == 0)
		return false;
	*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION;
	*vertexProgram = rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION;
	char path[256];
	if (!IsValidGameShaderPath(assetPath, path, sizeof(path)))
		return false;

	if (vertexShader)
	{
		if (GameShaderPathEquals(path, "shaders\\trees.vso"))
		{
			*vertexProgram = rts::render::RENDER_LEGACY_VERTEX_TREES;
			return true;
		}
		if (GameShaderPathEquals(path, "shaders\\wave.vso"))
		{
			*vertexProgram = rts::render::RENDER_LEGACY_VERTEX_WATER_SEA;
			return true;
		}
		return false;
	}

	if (GameShaderPathEquals(path, "shaders\\monochrome.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_MONOCHROME;
	else if (GameShaderPathEquals(path, "shaders\\terrain.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_TERRAIN_BASE;
	else if (GameShaderPathEquals(path, "shaders\\terrainnoise.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_TERRAIN_NOISE;
	else if (GameShaderPathEquals(path, "shaders\\terrainnoise2.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_TERRAIN_NOISE2;
	else if (GameShaderPathEquals(path, "shaders\\roadnoise2.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_ROAD_NOISE2;
	else if (GameShaderPathEquals(path, "shaders\\fterrain.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE;
	else if (GameShaderPathEquals(path, "shaders\\fterrain0.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE0;
	else if (GameShaderPathEquals(path, "shaders\\fterrainnoise.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE;
	else if (GameShaderPathEquals(path, "shaders\\fterrainnoise2.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2;
	else if (GameShaderPathEquals(path, "shaders\\trees.pso"))
	{
		// The historical tree pixel asset is paired with the native tree
		// vertex program, while its fixed-function color path is selected by
		// the neutral pipeline.  Keep the asset in the whitelist without
		// inventing a second translated pixel program.
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION;
	}
	else if (GameShaderPathEquals(path, "shaders\\wave.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_WATER_SEA;
	else if (GameShaderPathEquals(path, "builtin\\water\\river"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_WATER_RIVER;
	else if (GameShaderPathEquals(path, "builtin\\water\\reflection") ||
		GameShaderPathEquals(path, "builtin\\water\\trapezoid"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_WATER_FLAT;
	else if (GameShaderPathEquals(path, "builtin\\profiler\\swizzle") ||
		GameShaderPathEquals(path, "builtin\\profiler\\swizzle.pso"))
		*pixelProgram = rts::render::RENDER_LEGACY_PIXEL_PROFILER_SWIZZLE;
	else
		return false;
	return true;
}

bool NativeW3D2::DecodeGameShaderHandle(bool vertexShader,
	unsigned int handle, NativeShaderEntry **entry)
{
	if (entry == 0)
		return false;
	*entry = 0;
	const unsigned int slotToken = handle & 0xffffU;
	const unsigned int generation = (handle >> 16) & 0xffffU;
	if (slotToken == 0 || generation == 0)
		return false;
	const size_t slot = static_cast<size_t>(slotToken - 1U);
	if (slot >= m_gameShaders.size())
		return false;
	NativeShaderEntry &candidate = m_gameShaders[slot];
	if (!candidate.live || candidate.vertexShader != vertexShader ||
		candidate.generation != generation)
		return false;
	*entry = &candidate;
	return true;
}

bool NativeW3D2::DecodeGameShaderHandle(bool vertexShader,
	unsigned int handle, const NativeShaderEntry **entry) const
{
	if (entry == 0)
		return false;
	*entry = 0;
	const unsigned int slotToken = handle & 0xffffU;
	const unsigned int generation = (handle >> 16) & 0xffffU;
	if (slotToken == 0 || generation == 0)
		return false;
	const size_t slot = static_cast<size_t>(slotToken - 1U);
	if (slot >= m_gameShaders.size())
		return false;
	const NativeShaderEntry &candidate = m_gameShaders[slot];
	if (!candidate.live || candidate.vertexShader != vertexShader ||
		candidate.generation != generation)
		return false;
	*entry = &candidate;
	return true;
}

rts::render::RenderResult NativeW3D2::ApplyGameShaderBits(
	unsigned int shaderBits)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	rts::render::LegacyPipelineState decoded;
	if (!rts::render::DecodeLegacyShaderBits(shaderBits, &decoded))
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	// TrackLegacyShaderBits deliberately merges only the shader-owned subset,
	// retaining material/stencil/fill/depth-bias state published by callers.
	rts::render::TrackLegacyShaderBits(shaderBits);
	rts::render::LegacyPipelineState current;
	if (!rts::render::GetTrackedLegacyPipelineState(&current))
	{
		RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		return rts::render::RENDER_RESULT_FAILED;
	}
	// D3D11's front-face convention is the inverse of ShaderClass's legacy
	// culling-inversion bit.  The title adapter publishes that bit before this
	// call; Core remains independent of title headers.
	current.rasterizer.frontCounterClockwise = !m_gameShaderCullInverted;
	rts::render::TrackLegacyPipelineState(current);
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::SetGameRenderState(
	unsigned int state, unsigned int value)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}

	rts::render::LegacyLogicalState logical;
	if (!rts::render::GetTrackedLegacyLogicalState(&logical))
		logical = rts::render::LegacyLogicalState();
	unsigned int normalizedStencilByte = 0;
	bool publishFog = false;
	bool publishAmbient = false;
	switch (state)
	{
	case rts::render::GAME_RENDER_STATE_ALPHA_BLEND_ENABLE:
		logical.pipeline.blend.blendEnable = value != 0;
		break;
	case rts::render::GAME_RENDER_STATE_SOURCE_BLEND:
		if (!IsValidGameBlendFactor(value))
			goto invalid_state;
		logical.pipeline.blend.sourceColor =
			static_cast<rts::render::RenderBlendFactor>(value);
		logical.pipeline.blend.sourceAlpha =
			static_cast<rts::render::RenderBlendFactor>(value);
		break;
	case rts::render::GAME_RENDER_STATE_DESTINATION_BLEND:
		if (!IsValidGameBlendFactor(value))
			goto invalid_state;
		logical.pipeline.blend.destinationColor =
			static_cast<rts::render::RenderBlendFactor>(value);
		logical.pipeline.blend.destinationAlpha =
			static_cast<rts::render::RenderBlendFactor>(value);
		break;
	case rts::render::GAME_RENDER_STATE_COLOR_WRITE_MASK:
		if ((value & ~0x0fU) != 0U)
			goto invalid_state;
		logical.pipeline.blend.colorWriteMask = value;
		break;
	case rts::render::GAME_RENDER_STATE_DEPTH_ENABLE:
		logical.pipeline.depthStencil.depthEnable = value != 0;
		break;
	case rts::render::GAME_RENDER_STATE_DEPTH_WRITE:
		logical.pipeline.depthStencil.depthWrite = value != 0;
		break;
	case rts::render::GAME_RENDER_STATE_DEPTH_FUNCTION:
		if (!IsValidGameCompare(value))
			goto invalid_state;
		logical.pipeline.depthStencil.depthFunction =
			static_cast<rts::render::RenderCompareFunction>(value);
		break;
	case rts::render::GAME_RENDER_STATE_ALPHA_TEST_ENABLE:
		logical.pipeline.alphaTestEnable = value != 0;
		break;
	case rts::render::GAME_RENDER_STATE_ALPHA_FUNCTION:
		if (!IsValidGameCompare(value))
			goto invalid_state;
		logical.pipeline.alphaFunction =
			static_cast<rts::render::RenderCompareFunction>(value);
		break;
	case rts::render::GAME_RENDER_STATE_ALPHA_REFERENCE:
		if (value > 0xffU)
			goto invalid_state;
		logical.pipeline.alphaReference = value;
		break;
	case rts::render::GAME_RENDER_STATE_TEXTURE_FACTOR:
		logical.pipeline.textureFactor = value;
		break;
	case rts::render::GAME_RENDER_STATE_LIGHTING:
		logical.pipeline.lightingEnable = value != 0;
		break;
	case rts::render::GAME_RENDER_STATE_NORMALIZE_NORMALS:
		logical.pipeline.normalizeNormals = value != 0;
		break;
	case rts::render::GAME_RENDER_STATE_Z_BIAS:
		if (value > static_cast<unsigned int>(INT_MAX))
			goto invalid_state;
		logical.pipeline.rasterizer.depthBias = static_cast<int>(value);
		break;
	case rts::render::GAME_RENDER_STATE_BLEND_OPERATION:
		if (!IsValidGameBlendOperation(value))
			goto invalid_state;
		logical.pipeline.blend.colorOperation =
			static_cast<rts::render::RenderBlendOperation>(value);
		logical.pipeline.blend.alphaOperation =
			static_cast<rts::render::RenderBlendOperation>(value);
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_ENABLE:
		logical.pipeline.depthStencil.stencilEnable = value != 0;
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_FUNCTION:
		if (!IsValidGameCompare(value))
			goto invalid_state;
		logical.pipeline.depthStencil.stencilFunction =
			static_cast<rts::render::RenderCompareFunction>(value);
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_REFERENCE:
		if (!NormalizeGameStencilByte(value, &normalizedStencilByte))
			goto invalid_state;
		logical.pipeline.depthStencil.stencilReference = normalizedStencilByte;
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_READ_MASK:
		if (!NormalizeGameStencilByte(value, &normalizedStencilByte))
			goto invalid_state;
		logical.pipeline.depthStencil.stencilReadMask = normalizedStencilByte;
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_WRITE_MASK:
		if (!NormalizeGameStencilByte(value, &normalizedStencilByte))
			goto invalid_state;
		logical.pipeline.depthStencil.stencilWriteMask = normalizedStencilByte;
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_FAIL_OPERATION:
		if (!IsValidGameStencilOperation(value))
			goto invalid_state;
		logical.pipeline.depthStencil.stencilFail =
			static_cast<rts::render::RenderStencilOperation>(value);
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_DEPTH_FAIL_OPERATION:
		if (!IsValidGameStencilOperation(value))
			goto invalid_state;
		logical.pipeline.depthStencil.stencilDepthFail =
			static_cast<rts::render::RenderStencilOperation>(value);
		break;
	case rts::render::GAME_RENDER_STATE_STENCIL_PASS_OPERATION:
		if (!IsValidGameStencilOperation(value))
			goto invalid_state;
		logical.pipeline.depthStencil.stencilPass =
			static_cast<rts::render::RenderStencilOperation>(value);
		break;
	case rts::render::GAME_RENDER_STATE_CULL_MODE:
		switch (value)
		{
		case rts::render::GAME_RENDER_CULL_NONE:
			logical.pipeline.rasterizer.cullMode =
				rts::render::RENDER_CULL_NONE;
			break;
		case rts::render::GAME_RENDER_CULL_CLOCKWISE:
			logical.pipeline.rasterizer.cullMode =
				rts::render::RENDER_CULL_BACK;
			logical.pipeline.rasterizer.frontCounterClockwise = false;
			break;
		case rts::render::GAME_RENDER_CULL_COUNTER_CLOCKWISE:
			logical.pipeline.rasterizer.cullMode =
				rts::render::RENDER_CULL_BACK;
			logical.pipeline.rasterizer.frontCounterClockwise = true;
			break;
		default:
			goto invalid_state;
		}
		break;
	case rts::render::GAME_RENDER_STATE_SHADE_MODE:
		// The native fixed-function translation is Gouraud/interpolated. Flat
		// shading needs a provoking-vertex contract that is not present in the
		// neutral input layout, so reject it rather than silently changing it.
		if (value == rts::render::GAME_RENDER_SHADE_FLAT)
		{
			RecordGameFailure(rts::render::RENDER_RESULT_UNSUPPORTED);
			return rts::render::RENDER_RESULT_UNSUPPORTED;
		}
		if (value != rts::render::GAME_RENDER_SHADE_GOURAUD)
			goto invalid_state;
		break;
	case rts::render::GAME_RENDER_STATE_FOG_ENABLE:
		logical.constants.fog.enabled = value != 0;
		logical.pipeline.fogMode = value != 0 ?
			(logical.pipeline.fogMode == rts::render::RENDER_FOG_DISABLED ?
				rts::render::RENDER_FOG_LINEAR : logical.pipeline.fogMode) :
			rts::render::RENDER_FOG_DISABLED;
		publishFog = true;
		break;
	case rts::render::GAME_RENDER_STATE_FILL_MODE:
		if (value == rts::render::GAME_RENDER_FILL_POINT)
		{
			RecordGameFailure(rts::render::RENDER_RESULT_UNSUPPORTED);
			return rts::render::RENDER_RESULT_UNSUPPORTED;
		}
		if (value == rts::render::GAME_RENDER_FILL_WIREFRAME)
			logical.pipeline.rasterizer.fillMode =
				rts::render::RENDER_FILL_WIREFRAME;
		else if (value == rts::render::GAME_RENDER_FILL_SOLID)
			logical.pipeline.rasterizer.fillMode = rts::render::RENDER_FILL_SOLID;
		else
			goto invalid_state;
		break;
	case rts::render::GAME_RENDER_STATE_AMBIENT_COLOR:
		logical.constants.globalAmbient =
			rts::render::DecodeLegacyAmbientColor(value);
		publishAmbient = true;
		break;
	case rts::render::GAME_RENDER_STATE_POINT_SPRITE_ENABLE:
	case rts::render::GAME_RENDER_STATE_POINT_SCALE_ENABLE:
	case rts::render::GAME_RENDER_STATE_POINT_SIZE:
	case rts::render::GAME_RENDER_STATE_POINT_SIZE_MIN:
	case rts::render::GAME_RENDER_STATE_POINT_SIZE_MAX:
	case rts::render::GAME_RENDER_STATE_POINT_SCALE_A:
	case rts::render::GAME_RENDER_STATE_POINT_SCALE_B:
	case rts::render::GAME_RENDER_STATE_POINT_SCALE_C:
		RecordGameFailure(rts::render::RENDER_RESULT_UNSUPPORTED);
		return rts::render::RENDER_RESULT_UNSUPPORTED;
	default:
		goto invalid_state;
	}

	rts::render::TrackLegacyPipelineState(logical.pipeline);
	if (publishFog)
		rts::render::TrackLegacyFog(logical.constants.fog);
	if (publishAmbient)
		rts::render::TrackLegacyGlobalAmbient(logical.constants.globalAmbient);
	return rts::render::RENDER_RESULT_OK;

invalid_state:
	RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
	return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
}

void NativeW3D2::SetGameRenderCamera(void *camera)
{
	// CameraClass extraction belongs to the paired title adapter.  Keeping this
	// owner hook explicit prevents an opaque pointer from being retained or
	// interpreted in the shared native layer.
	(void)camera;
	RecordGameFailure(rts::render::RENDER_RESULT_UNSUPPORTED);
}

rts::render::RenderResult NativeW3D2::SetGameRenderCameraSnapshot(
	const rts::render::GameCameraSnapshot &snapshot)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	if (snapshot.viewport.x < 0.0f || snapshot.viewport.y < 0.0f ||
		snapshot.viewport.width < 0.0f || snapshot.viewport.height < 0.0f ||
		snapshot.viewport.minimumDepth < 0.0f ||
		snapshot.viewport.minimumDepth > 1.0f ||
		snapshot.viewport.maximumDepth < 0.0f ||
		snapshot.viewport.maximumDepth > 1.0f ||
		snapshot.viewport.minimumDepth > snapshot.viewport.maximumDepth ||
		!IsFiniteGameFloat(snapshot.viewport.x) ||
		!IsFiniteGameFloat(snapshot.viewport.y) ||
		!IsFiniteGameFloat(snapshot.viewport.width) ||
		!IsFiniteGameFloat(snapshot.viewport.height) ||
		!IsFiniteGameFloat(snapshot.viewport.minimumDepth) ||
		!IsFiniteGameFloat(snapshot.viewport.maximumDepth) ||
		!IsFiniteGameFloat(snapshot.zNear) ||
		!IsFiniteGameFloat(snapshot.zFar) || snapshot.zNear < 0.0f ||
		snapshot.zFar < snapshot.zNear)
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (!IsFiniteGameFloat(snapshot.view.values[index]) ||
			!IsFiniteGameFloat(snapshot.projection.values[index]))
		{
			RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
			return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		}
	}
	const rts::render::RenderResult viewportResult =
		SetGameViewport(snapshot.viewport);
	if (viewportResult != rts::render::RENDER_RESULT_OK)
	{
		RecordGameFailure(viewportResult);
		return viewportResult;
	}
	if (!rts::render::TrackLegacyTransform(
		rts::render::LEGACY_TRANSFORM_VIEW, snapshot.view.values) ||
		!rts::render::TrackLegacyTransform(
			rts::render::LEGACY_TRANSFORM_PROJECTION,
		snapshot.projection.values))
	{
		RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		return rts::render::RENDER_RESULT_FAILED;
	}
	m_gameCameraView = snapshot.view;
	m_gameCameraProjection = snapshot.projection;
	m_gameCameraViewport = snapshot.viewport;
	m_gameCameraNear = snapshot.zNear;
	m_gameCameraFar = snapshot.zFar;
	m_gameCameraValid = true;
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::CreateGameShaderFromAsset(
	const char *assetPath, bool vertexShader, const void *declarationWords,
	unsigned int declarationWordCount, unsigned int usage,
	unsigned int *handle)
{
	(void)declarationWords;
	(void)declarationWordCount;
	(void)usage;
	if (handle == 0)
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	if ((!IsOperational() && !CanRebuildResources()) ||
		!m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	rts::render::RenderLegacyPixelProgram pixelProgram;
	rts::render::RenderLegacyVertexProgram vertexProgram;
	if (!FindGameShaderAsset(assetPath, vertexShader, &pixelProgram,
		&vertexProgram))
	{
		RecordGameFailure(rts::render::RENDER_RESULT_UNSUPPORTED);
		return rts::render::RENDER_RESULT_UNSUPPORTED;
	}

	size_t slot = 0;
	bool found = false;
	for (; slot < m_gameShaders.size(); ++slot)
	{
		if (!m_gameShaders[slot].live && m_gameShaders[slot].generation != 0)
		{
			found = true;
			break;
		}
	}
	if (!found)
	{
		// The low handle word stores slot+1. Retire slots whose generation
		// exhausted its 16-bit wire representation instead of allowing a stale
		// handle to become valid after wraparound.
		if (m_gameShaders.size() >= 0xffffU)
		{
			RecordGameFailure(rts::render::RENDER_RESULT_OUT_OF_MEMORY);
			return rts::render::RENDER_RESULT_OUT_OF_MEMORY;
		}
		try
		{
			m_gameShaders.push_back(NativeShaderEntry());
		}
		catch (const std::bad_alloc &)
		{
			RecordGameFailure(rts::render::RENDER_RESULT_OUT_OF_MEMORY);
			return rts::render::RENDER_RESULT_OUT_OF_MEMORY;
		}
		catch (...)
		{
			RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
			return rts::render::RENDER_RESULT_FAILED;
		}
		slot = m_gameShaders.size() - 1;
	}
	NativeShaderEntry &entry = m_gameShaders[slot];
	if (entry.generation == 0 || entry.generation > 0xffffU)
	{
		RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		return rts::render::RENDER_RESULT_FAILED;
	}
	entry.live = true;
	entry.vertexShader = vertexShader;
	entry.pixelProgram = pixelProgram;
	entry.vertexProgram = vertexProgram;
	*handle = (entry.generation << 16) |
		static_cast<unsigned int>(slot + 1U);
	return rts::render::RENDER_RESULT_OK;
}

bool NativeW3D2::DeleteGameShader(bool vertexShader, unsigned int handle)
{
	if ((!IsOperational() && !IsRebuildingResources()) ||
		!m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return false;
	}
	NativeShaderEntry *entry = 0;
	if (!DecodeGameShaderHandle(vertexShader, handle, &entry))
		return false;
	entry->live = false;
	if (entry->generation == 0xffffU)
		entry->generation = 0;
	else
		++entry->generation;
	return true;
}

void NativeW3D2::SetGameVertexShader(unsigned int shaderOrFormat)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (shaderOrFormat == 0)
	{
		rts::render::TrackLegacyVertexProgram(
			rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION);
		return;
	}
	if (shaderOrFormat <= 0xffffU)
	{
		if (rts::render::LegacyFvfVertexSize(shaderOrFormat) == 0)
			RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		// FVF selection is consumed by NativeDrawPacket's declaration. The
		// owner validates it here but leaves the logical vertex program fixed.
		return;
	}
	const NativeShaderEntry *entry = 0;
	if (!DecodeGameShaderHandle(true, shaderOrFormat, &entry))
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	rts::render::TrackLegacyVertexProgram(entry->vertexProgram);
}

void NativeW3D2::SetGamePixelShader(unsigned int shader)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	if (shader == 0)
	{
		rts::render::TrackLegacyPixelProgram(
			rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION);
		return;
	}
	const NativeShaderEntry *entry = 0;
	if (!DecodeGameShaderHandle(false, shader, &entry))
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	rts::render::TrackLegacyPixelProgram(entry->pixelProgram);
}

void NativeW3D2::SetGameLegacyVertexProgram(
	rts::render::RenderLegacyVertexProgram program)
{
	if (!IsOperational() || !m_resources.IsOwnerThread() ||
		program < rts::render::RENDER_LEGACY_VERTEX_FIXED_FUNCTION ||
		program > rts::render::RENDER_LEGACY_VERTEX_WATER_SEA)
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	rts::render::TrackLegacyVertexProgram(program);
}

void NativeW3D2::SetGameLegacyPixelProgram(
	rts::render::RenderLegacyPixelProgram program)
{
	if (!IsOperational() || !m_resources.IsOwnerThread() ||
		program < rts::render::RENDER_LEGACY_PIXEL_FIXED_FUNCTION ||
		program > rts::render::RENDER_LEGACY_PIXEL_PROFILER_SWIZZLE)
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	rts::render::TrackLegacyPixelProgram(program);
}

rts::render::RenderResult NativeW3D2::SetGameViewport(
	const rts::render::RenderViewport &viewport)
{
	if (!IsOperational())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	return m_renderer.IsFrameOpen() ? m_renderer.SetViewport(viewport) :
		m_renderer.SetViewportExternal(viewport);
}

rts::render::RenderResult NativeW3D2::SubmitGamePacket(
	const rts::render::LegacyLogicalState &state,
	const rts::render::NativeDrawPacket &packet)
{
	if (!IsOperational())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	return m_renderer.IsFrameOpen() ? m_renderer.Submit(m_resources, state,
		packet) : m_renderer.SubmitExternal(m_resources, state, packet);
}

rts::render::RenderResult NativeW3D2::SubmitGameTriangles(
	const rts::render::LegacyLogicalState &state,
	const rts::render::NativeDrawPacket &packet)
{
	const rts::render::RenderResult result = SubmitGamePacket(state, packet);
	if (result != rts::render::RENDER_RESULT_OK)
		RecordGameFailure(result);
	return result;
}

rts::render::RenderResult NativeW3D2::QueueGameSortedTriangles(
	const rts::render::LegacyLogicalState &state,
	const rts::render::NativeDrawPacket &packet, const void *vertexData,
	size_t vertexBytes, const void *indexData, size_t indexBytes,
	const rts::render::GameBoundingSphere *sphere)
{
	if (!IsOperational())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const rts::render::RenderResult result = m_nativeSortingRenderer.Queue(state,
		packet, vertexData, vertexBytes, indexData, indexBytes, sphere);
	if (result != rts::render::RENDER_RESULT_OK)
		RecordGameFailure(result);
	return result;
}

rts::render::RenderResult NativeW3D2::FlushGameSortedTriangles()
{
	if (!IsOperational())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const rts::render::RenderResult result =
		m_nativeSortingRenderer.Flush(*this);
	if (result != rts::render::RENDER_RESULT_OK)
		RecordGameFailure(result);
	return result;
}

rts::render::RenderResult NativeW3D2::SubmitNativeSortedBatch(
	const rts::render::NativeSortedDraw *draws, unsigned int drawCount,
	const void *vertexData, size_t vertexBytes, const void *indexData,
	size_t indexBytes, unsigned int *submittedDrawCount)
{
	if (submittedDrawCount != 0)
		*submittedDrawCount = 0;
	if (!IsOperational() || draws == 0 || drawCount == 0 ||
		vertexData == 0 || vertexBytes == 0 || indexData == 0 ||
		indexBytes == 0 || submittedDrawCount == 0)
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;

	const rts::render::NativeDrawPacket &firstPacket = draws[0].packet;
	if (firstPacket.vertexStride == 0 ||
		vertexBytes % firstPacket.vertexStride != 0)
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	if (indexBytes % sizeof(unsigned short) != 0 ||
		indexBytes / sizeof(unsigned short) > 65535U)
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;

	for (unsigned int drawIndex = 0; drawIndex < drawCount; ++drawIndex)
	{
		const rts::render::NativeDrawPacket &packet = draws[drawIndex].packet;
		if (packet.vertexStride != firstPacket.vertexStride ||
			packet.vertexFormat != firstPacket.vertexFormat ||
			packet.topology != firstPacket.topology ||
			packet.indexFormat != firstPacket.indexFormat ||
			!packet.indexed || packet.vertexCount == 0 ||
			packet.indexCount == 0 || (packet.indexCount % 3) != 0 ||
			packet.startIndex > indexBytes / sizeof(unsigned short) ||
			packet.indexCount > indexBytes / sizeof(unsigned short) -
				packet.startIndex ||
			packet.vertexOffset % packet.vertexStride != 0 ||
			packet.vertexOffset > vertexBytes ||
			packet.vertexCount > (vertexBytes - packet.vertexOffset) /
				packet.vertexStride)
			return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}

	rts::render::BufferDescriptor vertexDescriptor;
	vertexDescriptor.byteCount = vertexBytes;
	vertexDescriptor.stride = firstPacket.vertexStride;
	vertexDescriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
	vertexDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle vertexHandle;
	rts::render::RenderResult result = m_resources.CreateBuffer(vertexDescriptor,
		vertexData, vertexBytes, &vertexHandle);
	if (result != rts::render::RENDER_RESULT_OK)
		return result;

	rts::render::BufferDescriptor indexDescriptor;
	indexDescriptor.byteCount = indexBytes;
	indexDescriptor.stride = sizeof(unsigned short);
	indexDescriptor.binding = rts::render::RENDER_BUFFER_INDEX;
	indexDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle indexHandle;
	result = m_resources.CreateBuffer(indexDescriptor, indexData, indexBytes,
		&indexHandle);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		// The index allocation failed, but the vertex allocation still owns a
		// native handle.  Always attempt that rollback and retain the original
		// allocation result as the primary error.  A cleanup refusal is latched
		// so the caller cannot mistake a partially rolled-back batch for success.
		if (!m_resources.Destroy(vertexHandle))
			RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		return result;
	}

	for (unsigned int drawIndex = 0; drawIndex < drawCount; ++drawIndex)
	{
		rts::render::NativeDrawPacket packet = draws[drawIndex].packet;
		packet.vertexBuffer = vertexHandle;
		packet.indexBuffer = indexHandle;
		packet.indexOffset = 0;
		packet.startVertex = 0;
		packet.minimumVertexIndex = 0;
		packet.baseVertex = 0;
		result = SubmitGamePacket(draws[drawIndex].state, packet);
		if (result != rts::render::RENDER_RESULT_OK)
			break;
		++*submittedDrawCount;
	}

	// Both temporary handles must be released even when the first destruction
	// refuses.  NativeW3DResources keeps a refused handle visible for owner
	// shutdown/retry, so dropping the second attempt would leak a live native
	// allocation and hide the authority failure from the frame result.
	const bool indexDestroyed = m_resources.Destroy(indexHandle);
	const bool vertexDestroyed = m_resources.Destroy(vertexHandle);
	if (!indexDestroyed || !vertexDestroyed)
	{
		RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		if (result == rts::render::RENDER_RESULT_OK)
			result = rts::render::RENDER_RESULT_FAILED;
	}
	return result;
}

rts::render::RenderResult NativeW3D2::BeginGameDisplayIteration()
{
	const rts::render::RenderResult serviceResult =
		ServiceThreadedCompletions();
	if (serviceResult != rts::render::RENDER_RESULT_OK &&
		!m_renderer.IsBackendOperational())
		return serviceResult;
	if (!IsOperational() || !m_resources.IsOwnerThread())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	m_displayIterationEpoch = rts::render::Advance_D3D11_Display_Epoch(
		m_displayIterationEpoch);
	m_gameFailure.reset();
	if (m_deferredFailureSequence != 0)
	{
		const rts::render::RenderResult deferredResult =
			m_deferredFailure.result();
		if (!m_deferredFailure.hasDeviceRemoval() ||
			m_recoveredFailureSequence == m_deferredFailureSequence)
		{
			m_deferredFailure = rts::render::RenderFrameOutcome();
			m_deferredFailureSequence = 0;
			m_recoveredFailureSequence = 0;
		}
		if (deferredResult != rts::render::RENDER_RESULT_OK)
			m_gameFailure.record(deferredResult);
		return deferredResult;
	}
	return serviceResult;
}

rts::render::RenderResult NativeW3D2::ResetGameRenderFrameResources(
	bool frameChanged)
{
	const rts::render::RenderResult serviceResult =
		ServiceThreadedCompletions();
	if (serviceResult != rts::render::RENDER_RESULT_OK &&
		!m_renderer.IsBackendOperational())
		return serviceResult;
	if (!IsOperational() || !m_resources.IsOwnerThread())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	const rts::render::RenderResult result =
		rts::render::Reset_Native_W3D_Buffer_Allocators(frameChanged);
	if (result != rts::render::RENDER_RESULT_OK)
		RecordGameFailure(result);
	return result;
}

bool NativeW3D2::SupportsPointSprites() const
{
	return false;
}

bool NativeW3D2::SupportsDot3() const
{
	return IsOperational();
}

bool NativeW3D2::SupportsZBias() const
{
	return false;
}

bool NativeW3D2::SupportsStencil() const
{
	return IsOperational();
}

bool NativeW3D2::SupportsNPatches() const
{
	return false;
}

bool NativeW3D2::IsGameTextureFormatSupported(
	rts::render::RenderFormat format) const
{
	if (!IsOperational() || !m_resources.IsOwnerThread() ||
		format == rts::render::RENDER_FORMAT_UNKNOWN)
	{
		return false;
	}

	// Probe the same resource creation path used by native texture uploads.
	// This asks the active device for a shader-resource-compatible texture and
	// therefore cannot report support solely because a CPU source conversion
	// exists.  The temporary handle is destroyed before returning; a failed
	// destroy is treated as a failed capability query rather than leaking a
	// resource or claiming support.
	rts::render::TextureDescriptor descriptor;
	descriptor.width = 1;
	descriptor.height = 1;
	descriptor.mipCount = 1;
	descriptor.arrayCount = 1;
	descriptor.dimension = rts::render::RENDER_TEXTURE_2D;
	descriptor.format = format;
	descriptor.binding = rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
	descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	rts::render::GpuHandle handle;
	NativeW3D2 *owner = const_cast<NativeW3D2 *>(this);
	const rts::render::RenderResult createResult = owner->m_resources.CreateTexture(
		descriptor, 0, 0, &handle);
	if (createResult != rts::render::RENDER_RESULT_OK)
	{
		return false;
	}
	if (!owner->m_resources.Destroy(handle))
	{
		owner->RecordGameFailure(rts::render::RENDER_RESULT_FAILED);
		return false;
	}
	return true;
}

rts::render::RenderResult NativeW3D2::SetGameFogState(
	const rts::render::LegacyFogConstants &fog)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	rts::render::TrackLegacyFog(fog);
	return rts::render::RENDER_RESULT_OK;
}

rts::render::RenderResult NativeW3D2::SetGameLightState(unsigned int index,
	const rts::render::LegacyLightState &light)
{
	if (!IsOperational() || !m_resources.IsOwnerThread() ||
		!rts::render::TrackLegacyLight(index, light))
	{
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	return rts::render::RENDER_RESULT_OK;
}

bool NativeW3D2::IsDebugConsoleDisabled() const
{
	return m_debugConsoleDisabled ||
		rts::render::GetGameDebugRenderStats().disableConsole;
}

rts::render::RenderResult NativeW3D2::GetTextureFilterCapabilities(
	rts::render::GameTextureFilterCapabilities *capabilities) const
{
	if (capabilities == 0)
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	*capabilities = rts::render::GameTextureFilterCapabilities();
	if (!IsOperational() || !m_resources.IsOwnerThread())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	rts::render::RenderTextureFilterCapabilities nativeCapabilities;
	const rts::render::RenderResult result =
		m_renderer.GetTextureFilterCapabilities(&nativeCapabilities);
	if (result != rts::render::RENDER_RESULT_OK)
		return result;
	capabilities->supportsPoint = nativeCapabilities.supportsPoint;
	capabilities->supportsLinear = nativeCapabilities.supportsLinear;
	capabilities->supportsAnisotropic = nativeCapabilities.supportsAnisotropic;
	capabilities->maxAnisotropy = nativeCapabilities.maxAnisotropy;
	return rts::render::RENDER_RESULT_OK;
}

unsigned int NativeW3D2::GetMaxTexturesPerPass() const
{
	return IsOperational() ? rts::render::LEGACY_TEXTURE_STAGE_COUNT : 0;
}

rts::render::RenderResult NativeW3D2::InvalidateGameMeshRendererCache()
{
	return FlushGameSortedTriangles();
}

rts::render::RenderResult NativeW3D2::GetGameBackBufferInfo(
	rts::render::RenderBackBufferInfo *info) const
{
	if (info == 0 || !IsOperational())
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	return m_renderer.GetBackBufferInfo(info);
}

rts::render::RenderResult NativeW3D2::QueueGameBackBufferCapture(
	const rts::render::RenderCaptureRequestDescriptor &descriptor,
	rts::render::RenderCaptureHandle *handle)
{
	if (handle != 0)
		*handle = rts::render::RenderCaptureHandle();
	if (!IsOperational() || !m_resources.IsOwnerThread() ||
		!m_gameCaptureQueue.bindOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	const rts::render::RenderResult result = m_gameCaptureQueue.enqueue(
		descriptor, handle);
	if (result != rts::render::RENDER_RESULT_OK)
		RecordGameFailure(result);
	return result;
}

unsigned int NativeW3D2::CancelGameBackBufferCaptures(
	void *consumer, rts::render::RenderResult reason)
{
	if (!IsOperational() || !m_resources.IsOwnerThread() ||
		!m_gameCaptureQueue.bindOwnerThread())
	{
		if (IsOperational())
			RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return 0;
	}
	return m_gameCaptureQueue.cancelConsumer(consumer, reason);
}

void NativeW3D2::RequestGameBackBufferCapture()
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	m_gameCaptureRequest.request();
}

rts::render::RenderResult NativeW3D2::PrepareGameBackBufferCapture(
	rts::render::RenderBackBufferInfo *info,
	std::vector<unsigned char> *pixels, size_t *rowPitch,
	rts::render::RenderFormat *format)
{
	using namespace rts::render;
	if (info == 0 || pixels == 0 || rowPitch == 0 || format == 0 ||
		!m_resources.IsOwnerThread() || !m_gameCaptureQueue.bindOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	*info = RenderBackBufferInfo();
	pixels->clear();
	*rowPitch = 0;
	*format = RENDER_FORMAT_UNKNOWN;
	if (!IsOperational())
	{
		const RenderResult reason = m_renderer.IsBackendOperational() ?
			RENDER_RESULT_INVALID_ARGUMENT : RENDER_RESULT_DEVICE_REMOVED;
		m_gameCaptureQueue.cancelCurrent(reason);
		m_gameCaptureRequest.clear();
		return reason;
	}
	if (m_gameCaptureQueue.pendingCount() == 0U)
	{
		m_gameCaptureRequest.clear();
		return RENDER_RESULT_OK;
	}
	RenderResult result = m_renderer.GetBackBufferInfo(info);
	if (result != RENDER_RESULT_OK)
	{
		m_gameCaptureQueue.cancelCurrent(result);
		m_gameCaptureRequest.clear();
		return result;
	}
	if (info->width == 0U || info->height == 0U ||
		(info->format != RENDER_FORMAT_R8G8B8A8_UNORM &&
			info->format != RENDER_FORMAT_B8G8R8A8_UNORM))
	{
		m_gameCaptureQueue.cancelCurrent(RENDER_RESULT_UNSUPPORTED);
		m_gameCaptureRequest.clear();
		return RENDER_RESULT_UNSUPPORTED;
	}
	size_t pixelBytes = 0;
	if (!CheckedGameSizeMultiply(static_cast<size_t>(info->width), 4U,
		rowPitch) || !CheckedGameSizeMultiply(*rowPitch,
		static_cast<size_t>(info->height), &pixelBytes) || pixelBytes == 0U)
	{
		m_gameCaptureQueue.cancelCurrent(RENDER_RESULT_INVALID_ARGUMENT);
		m_gameCaptureRequest.clear();
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	try
	{
		pixels->resize(pixelBytes);
		RenderFormat capturedFormat = RENDER_FORMAT_UNKNOWN;
		result = m_renderer.CaptureBackBuffer(pixels->data(), pixelBytes,
			*rowPitch, &capturedFormat);
		if (result == RENDER_RESULT_OK)
		{
			if (capturedFormat != RENDER_FORMAT_R8G8B8A8_UNORM &&
				capturedFormat != RENDER_FORMAT_B8G8R8A8_UNORM)
				result = RENDER_RESULT_UNSUPPORTED;
			else
				*format = capturedFormat;
		}
	}
	catch (const std::bad_alloc &)
	{
		result = RENDER_RESULT_OUT_OF_MEMORY;
	}
	catch (...)
	{
		result = RENDER_RESULT_FAILED;
	}
	if (result != RENDER_RESULT_OK)
		m_gameCaptureQueue.cancelCurrent(result);
	if (result != RENDER_RESULT_OK)
	{
		pixels->clear();
		*rowPitch = 0;
		*format = RENDER_FORMAT_UNKNOWN;
	}
	if (result != RENDER_RESULT_OK)
		m_gameCaptureRequest.clear();
	return result;
}

rts::render::RenderResult NativeW3D2::CompleteGameBackBufferCaptures(
	const rts::render::RenderBackBufferInfo &info, size_t rowPitch,
	rts::render::RenderFormat format,
	const std::vector<unsigned char> &pixels)
{
	using namespace rts::render;
	if (!m_resources.IsOwnerThread() || !m_gameCaptureQueue.bindOwnerThread())
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (m_gameCaptureQueue.pendingCount() == 0U)
	{
		m_gameCaptureRequest.clear();
		return RENDER_RESULT_OK;
	}
	const RenderResult result = m_gameCaptureQueue.completeVisible(info.width,
		info.height, rowPitch, format, pixels.data(), pixels.size());
	if (result != RENDER_RESULT_OK)
		m_gameCaptureQueue.cancelCurrent(result);
	m_gameCaptureRequest.clear();
	return result;
}

void NativeW3D2::SetActiveRenderTargetKind(
	rts::render::GameRenderTargetKind targetKind)
{
	m_activeRenderTargetKind = targetKind;
}

void NativeW3D2::SetGameDebugConsoleDisabled(bool disabled)
{
	m_debugConsoleDisabled = disabled;
}

rts::render::RenderResult NativeW3D2::SetGameShaderCullInverted(bool inverted)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_gameShaderCullInverted = inverted;
	rts::render::LegacyPipelineState state;
	if (rts::render::GetTrackedLegacyPipelineState(&state))
	{
		state.rasterizer.frontCounterClockwise = !inverted;
		rts::render::TrackLegacyPipelineState(state);
	}
	return rts::render::RENDER_RESULT_OK;
}

void NativeW3D2::SetGameCleanupHook(
	rts::render::GameRenderCleanupHook *hook)
{
	if (!IsOperational() || !m_resources.IsOwnerThread())
	{
		RecordGameFailure(rts::render::RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	// The hook is intentionally retained by the owner across reset/resize;
	// callers must unregister it before destroying the implementing object.
	m_gameCleanupHook = hook;
}

unsigned int NativeW3D2::DisplayIterationEpoch() const
{
	return m_displayIterationEpoch;
}

void NativeW3D2::RecordGameFailure(rts::render::RenderResult result)
{
	if (result != rts::render::RENDER_RESULT_OK)
	{
		m_gameFailure.record(result);
		m_renderer.RecordFrameFailure(result);
	}
}

bool NativeW3D2::IsAttachedToBorrowedBackend() const
{
	return m_borrowedBackend && m_resourceHost.IsAttached();
}

rts::render::NativeW3DRenderer &NativeW3D2::Renderer()
{
	return m_renderer;
}

rts::render::NativeW3DResources &NativeW3D2::Resources()
{
	return m_resources;
}
