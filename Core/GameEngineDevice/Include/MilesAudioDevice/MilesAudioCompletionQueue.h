#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <Utility/interlocked_adapter.h>
#else
#include <sched.h>
#endif

namespace rts
{

struct AudioCompletionRecord
{
	unsigned handle;
	unsigned type;
	unsigned generation;
};

namespace AudioCompletionQueueDetail
{

inline long load(volatile long *value)
{
#if defined(_WIN32)
	return InterlockedCompareExchange(value, 0, 0);
#else
	return __sync_val_compare_and_swap(value, 0, 0);
#endif
}

inline long exchange(volatile long *value, long replacement)
{
#if defined(_WIN32)
	return InterlockedExchange(value, replacement);
#else
	return __sync_lock_test_and_set(value, replacement);
#endif
}

inline long increment(volatile long *value)
{
#if defined(_WIN32)
	return InterlockedIncrement(value);
#else
	return __sync_add_and_fetch(value, 1);
#endif
}

inline long decrement(volatile long *value)
{
#if defined(_WIN32)
	return InterlockedDecrement(value);
#else
	return __sync_sub_and_fetch(value, 1);
#endif
}

inline long compareExchange(volatile long *value, long replacement, long expected)
{
#if defined(_WIN32)
	return InterlockedCompareExchange(value, replacement, expected);
#else
	return __sync_val_compare_and_swap(value, expected, replacement);
#endif
}

inline void memoryBarrier()
{
#if defined(_WIN32)
	#if defined(_MSC_VER) && _MSC_VER < 1300
		volatile long barrier = 0;
		InterlockedExchange(&barrier, 0);
	#else
	MemoryBarrier();
	#endif
#else
	__sync_synchronize();
#endif
}

inline void yield()
{
#if defined(_WIN32)
	Sleep(0);
#else
	sched_yield();
#endif
}

} // namespace AudioCompletionQueueDetail

// A fixed-capacity MPSC queue intended for real-time callbacks. Producers never
// allocate or wait on a mutex. A producer may reserve a slot before publishing
// it; the owner drains strictly in reservation order and therefore waits only
// by returning to its next update if a producer is preempted mid-publish.
template <unsigned Capacity>
class AudioCompletionRing
{
public:
	AudioCompletionRing()
		: m_write(0),
		  m_read(0),
		  m_accepting(0),
		  m_publishers(0),
		  m_generation(0),
		  m_overflow(0)
	{
		for (unsigned i = 0; i < Capacity; ++i)
		{
			m_slots[i].ready = 0;
			m_slots[i].record.handle = 0;
			m_slots[i].record.type = 0;
			m_slots[i].record.generation = 0;
		}
	}

	void reset(unsigned generation)
	{
		close();
		clear();
		AudioCompletionQueueDetail::exchange(&m_generation, static_cast<long>(generation));
		AudioCompletionQueueDetail::memoryBarrier();
		AudioCompletionQueueDetail::exchange(&m_accepting, 1);
	}

	void close()
	{
		AudioCompletionQueueDetail::exchange(&m_accepting, 0);
		while (AudioCompletionQueueDetail::load(&m_publishers) != 0)
		{
			AudioCompletionQueueDetail::yield();
		}
	}

	void clear()
	{
		while (AudioCompletionQueueDetail::load(&m_publishers) != 0)
		{
			AudioCompletionQueueDetail::yield();
		}

		const long write = AudioCompletionQueueDetail::load(&m_write);
		for (unsigned i = 0; i < Capacity; ++i)
		{
			AudioCompletionQueueDetail::exchange(&m_slots[i].ready, 0);
		}
		AudioCompletionQueueDetail::exchange(&m_read, write);
		AudioCompletionQueueDetail::exchange(&m_overflow, 0);
	}

	bool tryPublish(unsigned handle, unsigned type)
	{
		if (AudioCompletionQueueDetail::load(&m_accepting) == 0)
		{
			return false;
		}

		AudioCompletionQueueDetail::increment(&m_publishers);
		if (AudioCompletionQueueDetail::load(&m_accepting) == 0)
		{
			AudioCompletionQueueDetail::decrement(&m_publishers);
			return false;
		}

		long position = AudioCompletionQueueDetail::load(&m_write);
		for (;;)
		{
			const unsigned long distance = static_cast<unsigned long>(
				position - AudioCompletionQueueDetail::load(&m_read));
			if (distance >= Capacity)
			{
				AudioCompletionQueueDetail::exchange(&m_overflow, 1);
				AudioCompletionQueueDetail::decrement(&m_publishers);
				return false;
			}

			if (AudioCompletionQueueDetail::compareExchange(&m_write, position + 1, position) == position)
			{
				break;
			}
			position = AudioCompletionQueueDetail::load(&m_write);
		}

		Slot &slot = m_slots[static_cast<unsigned long>(position) % Capacity];
		slot.record.handle = handle;
		slot.record.type = type;
		slot.record.generation = static_cast<unsigned>(AudioCompletionQueueDetail::load(&m_generation));
		AudioCompletionQueueDetail::memoryBarrier();
		AudioCompletionQueueDetail::exchange(&slot.ready, 1);
		AudioCompletionQueueDetail::decrement(&m_publishers);
		return true;
	}

	bool tryPop(AudioCompletionRecord *record)
	{
		return tryPop(record, snapshot());
	}

	unsigned long snapshot() const
	{
		return static_cast<unsigned long>(AudioCompletionQueueDetail::load(const_cast<volatile long *>(&m_write)));
	}

	bool tryPop(AudioCompletionRecord *record, unsigned long snapshotPosition)
	{
		if (record == 0)
		{
			return false;
		}

		const long position = AudioCompletionQueueDetail::load(&m_read);
		if (static_cast<unsigned long>(position) == snapshotPosition)
		{
			return false;
		}

		Slot &slot = m_slots[static_cast<unsigned long>(position) % Capacity];
		if (AudioCompletionQueueDetail::load(&slot.ready) == 0)
		{
			return false;
		}

		AudioCompletionQueueDetail::memoryBarrier();
		*record = slot.record;
		AudioCompletionQueueDetail::exchange(&slot.ready, 0);
		AudioCompletionQueueDetail::exchange(&m_read, position + 1);
		return true;
	}

	bool consumeOverflow()
	{
		return AudioCompletionQueueDetail::exchange(&m_overflow, 0) != 0;
	}

	unsigned currentGeneration() const
	{
		return static_cast<unsigned>(AudioCompletionQueueDetail::load(
			const_cast<volatile long *>(&m_generation)));
	}

private:
	struct Slot
	{
		volatile long ready;
		AudioCompletionRecord record;
	};

	volatile long m_write;
	volatile long m_read;
	volatile long m_accepting;
	volatile long m_publishers;
	volatile long m_generation;
	volatile long m_overflow;
	Slot m_slots[Capacity];
};

} // namespace rts
