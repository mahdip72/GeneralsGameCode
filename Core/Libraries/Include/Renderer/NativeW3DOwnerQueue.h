#ifndef RTS_RENDERER_NATIVEW3DOWNERQUEUE_H
#define RTS_RENDERER_NATIVEW3DOWNERQUEUE_H

#include "Renderer/RendererDevice.h"

namespace rts
{
namespace render
{
// Commands are deliberately represented by a C-style callback and context so
// the queue remains usable by the VS6/C++98 compatibility lane.  Callbacks
// execute on the thread that successfully bound the queue as its owner.
typedef void (*NativeW3DOwnerCommand)(void *context);

class NativeW3DOwnerQueue
{
public:
	// A zero maxCommands value in Drain means "the commands present when the
	// drain began".  Commands enqueued by a callback are left for the next
	// drain, which prevents a re-entrant producer from starving the owner.
	explicit NativeW3DOwnerQueue(unsigned int capacity = 256);
	~NativeW3DOwnerQueue();

	// Binding is idempotent for the current thread and rejected for another
	// thread once an owner has been established.
	RenderResult BindOwner();

	// Enqueue is safe for producers on other threads, but requires an owner to
	// have been bound first.  The command is copied into the bounded queue; the
	// context remains owned by the caller until the callback has run.
	RenderResult Enqueue(NativeW3DOwnerCommand command, void *context);

	// Only the bound owner may drain.  drained must be non-null and receives the
	// number of callbacks removed from the queue, including callbacks that fail
	// through a C++ exception.  Callback exceptions are contained and reported
	// as RENDER_RESULT_FAILED after the remaining selected commands run.
	RenderResult Drain(unsigned int maxCommands, unsigned int *drained);

	bool IsOwnerThread() const;
	bool IsBound() const;
	unsigned int Capacity() const;
	unsigned int Size() const;

private:
	NativeW3DOwnerQueue(const NativeW3DOwnerQueue &);
	NativeW3DOwnerQueue &operator=(const NativeW3DOwnerQueue &);

	struct Impl;
	Impl *m_impl;
};
}
}

#endif
