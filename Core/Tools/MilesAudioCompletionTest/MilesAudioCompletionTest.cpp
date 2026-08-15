#include "MilesAudioDevice/MilesAudioCompletionQueue.h"

#include <stdio.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

static int s_failures = 0;

static void check(bool condition, const char *message)
{
	if (!condition)
	{
		++s_failures;
		printf("FAIL: %s\n", message);
	}
}

static void testFifoAndGeneration()
{
	rts::AudioCompletionRing<8> queue;
	queue.reset(11);
	check(queue.tryPublish(10, 1), "first completion is accepted");
	check(queue.tryPublish(20, 2), "second completion is accepted");
	check(queue.tryPublish(30, 3), "third completion is accepted");

	rts::AudioCompletionRecord record;
	check(queue.tryPop(&record), "first completion is available");
	check(record.handle == 10 && record.type == 1 && record.generation == 11,
		"first completion preserves FIFO payload and generation");
	check(queue.tryPop(&record), "second completion is available");
	check(record.handle == 20 && record.type == 2 && record.generation == 11,
		"second completion preserves FIFO payload and generation");
	check(queue.tryPop(&record), "third completion is available");
	check(record.handle == 30 && record.type == 3 && record.generation == 11,
		"third completion preserves FIFO payload and generation");
	check(!queue.tryPop(&record), "empty queue does not produce a duplicate completion");
}

static void testOverflowAndRecovery()
{
	rts::AudioCompletionRing<2> queue;
	queue.reset(21);
	check(queue.tryPublish(1, 1), "first bounded slot is accepted");
	check(queue.tryPublish(2, 1), "second bounded slot is accepted");
	check(!queue.tryPublish(3, 1), "overflow rejects without waiting");
	check(queue.consumeOverflow(), "overflow recovery flag is observable");
	check(!queue.consumeOverflow(), "overflow recovery flag is consumed exactly once");

	rts::AudioCompletionRecord record;
	check(queue.tryPop(&record), "first bounded record drains");
	check(queue.tryPop(&record), "second bounded record drains");
	check(queue.tryPublish(4, 1), "queue accepts new work after draining overflow");
	check(queue.tryPop(&record) && record.handle == 4, "recovered record drains exactly once");
}

static void testResetDropsStaleGeneration()
{
	rts::AudioCompletionRing<4> queue;
	queue.reset(31);
	check(queue.tryPublish(100, 7), "old-generation completion is accepted before reset");
	queue.reset(32);

	rts::AudioCompletionRecord record;
	check(!queue.tryPop(&record), "reset clears stale completion records");
	check(queue.tryPublish(200, 8), "new-generation completion is accepted");
	check(queue.tryPop(&record), "new-generation completion drains");
	check(record.handle == 200 && record.generation == 32,
		"new-generation completion carries the new generation");
}

static void testSnapshotDoesNotChaseProducers()
{
	rts::AudioCompletionRing<4> queue;
	queue.reset(51);
	check(queue.tryPublish(1, 1), "snapshot seed completion is accepted");
	const unsigned long snapshot = queue.snapshot();
	check(queue.tryPublish(2, 1), "post-snapshot completion is accepted");

	rts::AudioCompletionRecord record;
	check(queue.tryPop(&record, snapshot), "snapshot drains its first completion");
	check(record.handle == 1, "snapshot first completion is correct");
	check(!queue.tryPop(&record, snapshot), "snapshot does not chase later producers");
	check(queue.tryPop(&record), "later completion remains for the next snapshot");
	check(record.handle == 2, "later completion is preserved");
}

static void testCloseRejectsAndReopens()
{
	rts::AudioCompletionRing<4> queue;
	queue.reset(61);
	queue.close();
	check(!queue.tryPublish(1, 1), "closed queue rejects callback publication");
	queue.reset(62);
	check(queue.tryPublish(2, 1), "reset queue reopens callback admission");

	rts::AudioCompletionRecord record;
	check(queue.tryPop(&record), "reopened queue drains a completion");
	check(record.handle == 2 && record.generation == 62,
		"reopened queue uses the new completion generation");
}

#if defined(_WIN32)
struct ProducerContext
{
	rts::AudioCompletionRing<64> *queue;
	unsigned producer;
	unsigned count;
	HANDLE start;
	unsigned accepted;
};

static unsigned __stdcall producerEntry(void *context)
{
	ProducerContext *producer = static_cast<ProducerContext *>(context);
	WaitForSingleObject(producer->start, INFINITE);
	for (unsigned i = 0; i < producer->count; ++i)
	{
		if (producer->queue->tryPublish(producer->producer * 1000 + i, producer->producer))
		{
			++producer->accepted;
		}
	}
	return 0;
}
#else
struct ProducerContext
{
	rts::AudioCompletionRing<64> *queue;
	unsigned producer;
	unsigned count;
	pthread_barrier_t *start;
	unsigned accepted;
};

static void *producerEntry(void *context)
{
	ProducerContext *producer = static_cast<ProducerContext *>(context);
	pthread_barrier_wait(producer->start);
	for (unsigned i = 0; i < producer->count; ++i)
	{
		if (producer->queue->tryPublish(producer->producer * 1000 + i, producer->producer))
		{
			++producer->accepted;
		}
	}
	return 0;
}
#endif

static void testConcurrentProducers()
{
	rts::AudioCompletionRing<64> queue;
	queue.reset(41);
	const unsigned producerCount = 4;
	const unsigned recordsPerProducer = 8;

#if defined(_WIN32)
	HANDLE start = CreateEvent(0, TRUE, FALSE, 0);
	HANDLE threads[producerCount];
	ProducerContext contexts[producerCount];
	for (unsigned i = 0; i < producerCount; ++i)
	{
		contexts[i].queue = &queue;
		contexts[i].producer = i + 1;
		contexts[i].count = recordsPerProducer;
		contexts[i].start = start;
		contexts[i].accepted = 0;
		threads[i] = reinterpret_cast<HANDLE>(_beginthreadex(0, 0, producerEntry, &contexts[i], 0, 0));
		check(threads[i] != 0, "producer thread starts");
	}
	SetEvent(start);
	WaitForMultipleObjects(producerCount, threads, TRUE, INFINITE);
	for (unsigned i = 0; i < producerCount; ++i)
	{
		CloseHandle(threads[i]);
	}
	CloseHandle(start);
#else
	pthread_barrier_t start;
	pthread_barrier_init(&start, 0, producerCount);
	pthread_t threads[producerCount];
	ProducerContext contexts[producerCount];
	for (unsigned i = 0; i < producerCount; ++i)
	{
		contexts[i].queue = &queue;
		contexts[i].producer = i + 1;
		contexts[i].count = recordsPerProducer;
		contexts[i].start = &start;
		contexts[i].accepted = 0;
		check(pthread_create(&threads[i], 0, producerEntry, &contexts[i]) == 0,
			"producer thread starts");
	}
	for (unsigned i = 0; i < producerCount; ++i)
	{
		pthread_join(threads[i], 0);
	}
	pthread_barrier_destroy(&start);
#endif

	bool seen[producerCount * recordsPerProducer] = { false };
	unsigned drained = 0;
	rts::AudioCompletionRecord record;
	while (queue.tryPop(&record))
	{
		const unsigned producer = record.type;
		const unsigned index = record.handle - producer * 1000;
		check(producer >= 1 && producer <= producerCount && index < recordsPerProducer,
			"concurrent record payload is valid");
		if (producer >= 1 && producer <= producerCount && index < recordsPerProducer)
		{
			const unsigned slot = (producer - 1) * recordsPerProducer + index;
			check(!seen[slot], "concurrent record is delivered exactly once");
			seen[slot] = true;
		}
		++drained;
	}
	check(drained == producerCount * recordsPerProducer, "all concurrent records drain");
	check(!queue.consumeOverflow(), "concurrent producers do not overflow capacity");
	for (unsigned i = 0; i < producerCount; ++i)
	{
		check(contexts[i].accepted == recordsPerProducer,
			"concurrent producer completion is accepted");
	}
}

int main()
{
	testFifoAndGeneration();
	testOverflowAndRecovery();
	testResetDropsStaleGeneration();
	testSnapshotDoesNotChaseProducers();
	testCloseRejectsAndReopens();
	testConcurrentProducers();
	if (s_failures != 0)
	{
		printf("%d audio completion test(s) failed.\n", s_failures);
		return 1;
	}
	printf("All audio completion tests passed.\n");
	return 0;
}
