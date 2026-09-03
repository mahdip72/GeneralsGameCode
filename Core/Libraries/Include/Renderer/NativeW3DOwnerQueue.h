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
typedef void (*NativeW3DOwnerContextRelease)(void *context);

// A caller-owned, allocation-free queue entry for terminal cleanup that must
// not be lost when the bounded packet queue is full.  One entry may be queued
// at a time; it stays queued if its callback throws so the owner can retry it.
class NativeW3DOwnerFallbackEntry
{
public:
	NativeW3DOwnerFallbackEntry();
	~NativeW3DOwnerFallbackEntry();
	bool IsQueued() const;

private:
	friend class NativeW3DOwnerQueue;
	NativeW3DOwnerFallbackEntry(const NativeW3DOwnerFallbackEntry &);
	NativeW3DOwnerFallbackEntry &operator=(
		const NativeW3DOwnerFallbackEntry &);

	NativeW3DOwnerCommand m_command;
	NativeW3DOwnerContextRelease m_release;
	void *m_context;
	NativeW3DOwnerFallbackEntry *m_next;
	bool m_queued;
};

// The queue never stores a caller-owned/game-owned pointer directly.  Wrap
// callback state in an opaque token and keep one reference for every accepted
// queue entry.  The token's context is visible only to the command while the
// queue owns that reference; the optional release callback runs once when the
// final reference is released.  The API intentionally uses AddRef/Release
// rather than C++11 smart pointers so the legacy C++98 lane can share it.
class NativeW3DOwnerToken
{
public:
	static NativeW3DOwnerToken *Create(void *context,
		NativeW3DOwnerContextRelease release = 0);

	void AddRef();
	void Release();

private:
	friend class NativeW3DOwnerQueue;

	NativeW3DOwnerToken();
	~NativeW3DOwnerToken();
	NativeW3DOwnerToken(const NativeW3DOwnerToken &);
	NativeW3DOwnerToken &operator=(const NativeW3DOwnerToken &);

	void *Context() const;

	struct Impl;
	Impl *m_impl;
};

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
	// have been bound first.  The queue retains the opaque token until the
	// callback has run (or the pending entry is discarded).  Queue entries
	// therefore never contain a caller-owned/game-owned pointer.
	RenderResult Enqueue(NativeW3DOwnerCommand command,
		NativeW3DOwnerToken *token);
	// Terminal resource-table teardown uses an entry embedded in the table.
	// This path performs no allocation and is not limited by packet capacity;
	// its total size is bounded by the registered resource-table count.
	RenderResult EnqueueFallback(NativeW3DOwnerCommand command, void *context,
		NativeW3DOwnerContextRelease release,
		NativeW3DOwnerFallbackEntry *entry);

	// Closing is owner-only and atomic with respect to producers.  Accepted
	// entries stay drainable, while later producers are rejected before they
	// can retain a backend-lifetime token.
	RenderResult Close();

	// Only the bound owner may drain.  drained must be non-null and receives the
	// number of callbacks removed from the queue. Ordinary callback exceptions
	// are contained and removed; a failing terminal fallback remains queued and
	// is not counted so owner cleanup can retry without orphaning live handles.
	RenderResult Drain(unsigned int maxCommands, unsigned int *drained);

	bool IsOwnerThread() const;
	bool IsBound() const;
	bool IsAccepting() const;
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
