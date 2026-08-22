#include "Renderer/NativeW3DOwnerQueue.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <new>

namespace rts
{
namespace render
{
namespace
{
#ifdef _WIN32
typedef unsigned long QueueThreadId;

QueueThreadId CurrentQueueThreadId()
{
	return static_cast<QueueThreadId>(GetCurrentThreadId());
}

bool SameQueueThread(QueueThreadId left, QueueThreadId right)
{
	return left == right;
}
#else
typedef pthread_t QueueThreadId;

QueueThreadId CurrentQueueThreadId()
{
	return pthread_self();
}

bool SameQueueThread(QueueThreadId left, QueueThreadId right)
{
	return pthread_equal(left, right) != 0;
}
#endif

class QueueLock
{
public:
	QueueLock() : m_initialized(false)
	{
#ifdef _WIN32
		InitializeCriticalSection(&m_lock);
		m_initialized = true;
#else
		m_initialized = pthread_mutex_init(&m_lock, 0) == 0;
#endif
	}

	~QueueLock()
	{
		if (!m_initialized)
		{
			return;
		}
#ifdef _WIN32
		DeleteCriticalSection(&m_lock);
#else
		pthread_mutex_destroy(&m_lock);
#endif
	}

	bool IsInitialized() const
	{
		return m_initialized;
	}

	void Lock()
	{
#ifdef _WIN32
		EnterCriticalSection(&m_lock);
#else
		pthread_mutex_lock(&m_lock);
#endif
	}

	void Unlock()
	{
#ifdef _WIN32
		LeaveCriticalSection(&m_lock);
#else
		pthread_mutex_unlock(&m_lock);
#endif
	}

private:
	QueueLock(const QueueLock &);
	QueueLock &operator=(const QueueLock &);

#ifdef _WIN32
	CRITICAL_SECTION m_lock;
#else
	pthread_mutex_t m_lock;
#endif
	bool m_initialized;
};

class ScopedQueueLock
{
public:
	explicit ScopedQueueLock(QueueLock &lock) : m_lock(lock)
	{
		m_lock.Lock();
	}

	~ScopedQueueLock()
	{
		m_lock.Unlock();
	}

private:
	ScopedQueueLock(const ScopedQueueLock &);
	ScopedQueueLock &operator=(const ScopedQueueLock &);

	QueueLock &m_lock;
};
}

struct NativeW3DOwnerQueue::Impl
{
	struct Entry
	{
		NativeW3DOwnerCommand command;
		void *context;
	};

	Impl(unsigned int requestedCapacity) :
		entries(0),
		capacity(requestedCapacity),
		head(0),
		tail(0),
		count(0),
		ownerBound(false),
		owner()
	{
		if (capacity != 0)
		{
			entries = new (std::nothrow) Entry[capacity];
			if (entries == 0)
			{
				capacity = 0;
			}
		}
	}

	~Impl()
	{
		delete[] entries;
	}

	Entry *entries;
	unsigned int capacity;
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	bool ownerBound;
	QueueThreadId owner;
	QueueLock lock;
};

NativeW3DOwnerQueue::NativeW3DOwnerQueue(unsigned int capacity) :
	m_impl(new (std::nothrow) Impl(capacity))
{
}

NativeW3DOwnerQueue::~NativeW3DOwnerQueue()
{
	if (m_impl == 0)
	{
		return;
	}
	if (m_impl->lock.IsInitialized())
	{
		ScopedQueueLock lock(m_impl->lock);
		m_impl->count = 0;
		m_impl->head = 0;
		m_impl->tail = 0;
	}
	delete m_impl;
	m_impl = 0;
}

RenderResult NativeW3DOwnerQueue::BindOwner()
{
	if (m_impl == 0 || m_impl->capacity == 0 || !m_impl->lock.IsInitialized())
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}

	const QueueThreadId currentThread = CurrentQueueThreadId();
	ScopedQueueLock lock(m_impl->lock);
	if (!m_impl->ownerBound)
	{
		m_impl->owner = currentThread;
		m_impl->ownerBound = true;
		return RENDER_RESULT_OK;
	}
	return SameQueueThread(m_impl->owner, currentThread) ?
		RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
}

RenderResult NativeW3DOwnerQueue::Enqueue(NativeW3DOwnerCommand command,
	void *context)
{
	if (command == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_impl == 0 || m_impl->capacity == 0 || !m_impl->lock.IsInitialized())
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}

	ScopedQueueLock lock(m_impl->lock);
	if (!m_impl->ownerBound)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_impl->count == m_impl->capacity)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}

	m_impl->entries[m_impl->tail].command = command;
	m_impl->entries[m_impl->tail].context = context;
	m_impl->tail = (m_impl->tail + 1) % m_impl->capacity;
	++m_impl->count;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DOwnerQueue::Drain(unsigned int maxCommands,
	unsigned int *drained)
{
	if (drained == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*drained = 0;
	if (m_impl == 0 || !m_impl->lock.IsInitialized())
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}

	const QueueThreadId currentThread = CurrentQueueThreadId();
	unsigned int budget = 0;
	{
		ScopedQueueLock lock(m_impl->lock);
		if (!m_impl->ownerBound ||
			!SameQueueThread(m_impl->owner, currentThread))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		budget = m_impl->count;
		if (maxCommands != 0 && maxCommands < budget)
		{
			budget = maxCommands;
		}
	}

	RenderResult result = RENDER_RESULT_OK;
	while (*drained < budget)
	{
		NativeW3DOwnerCommand command = 0;
		void *context = 0;
		{
			ScopedQueueLock lock(m_impl->lock);
			if (m_impl->count == 0)
			{
				break;
			}
			command = m_impl->entries[m_impl->head].command;
			context = m_impl->entries[m_impl->head].context;
			m_impl->entries[m_impl->head].command = 0;
			m_impl->entries[m_impl->head].context = 0;
			m_impl->head = (m_impl->head + 1) % m_impl->capacity;
			--m_impl->count;
		}

		try
		{
			command(context);
		}
		catch (...)
		{
			result = RENDER_RESULT_FAILED;
		}
		++*drained;
	}
	return result;
}

bool NativeW3DOwnerQueue::IsOwnerThread() const
{
	if (m_impl == 0 || !m_impl->lock.IsInitialized())
	{
		return false;
	}
	const QueueThreadId currentThread = CurrentQueueThreadId();
	ScopedQueueLock lock(m_impl->lock);
	return m_impl->ownerBound && SameQueueThread(m_impl->owner, currentThread);
}

bool NativeW3DOwnerQueue::IsBound() const
{
	if (m_impl == 0 || !m_impl->lock.IsInitialized())
	{
		return false;
	}
	ScopedQueueLock lock(m_impl->lock);
	return m_impl->ownerBound;
}

unsigned int NativeW3DOwnerQueue::Capacity() const
{
	return m_impl == 0 ? 0 : m_impl->capacity;
}

unsigned int NativeW3DOwnerQueue::Size() const
{
	if (m_impl == 0 || !m_impl->lock.IsInitialized())
	{
		return 0;
	}
	ScopedQueueLock lock(m_impl->lock);
	return m_impl->count;
}
}
}
