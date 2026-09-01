#ifndef RTS_WW3D2_NATIVEW3D2_H
#define RTS_WW3D2_NATIVEW3D2_H

#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"

// A narrow native WW3D entry point.  It owns the lifetime ordering between
// the facade and the resource registry; gameplay integration follows only as
// individual legacy classes are migrated to this target.
class NativeW3D2
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

private:
	NativeW3D2(const NativeW3D2 &);
	NativeW3D2 &operator=(const NativeW3D2 &);

	rts::render::NativeW3DRenderer m_renderer;
	rts::render::NativeW3DResourceHost m_resourceHost;
	rts::render::NativeW3DResources m_resources;
	bool m_borrowedBackend;
};

#endif
