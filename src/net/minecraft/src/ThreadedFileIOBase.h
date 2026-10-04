#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

class IThreadedFileIO;

// net.minecraft.src.ThreadedFileIOBase
class ThreadedFileIOBase
{
public:
	static ThreadedFileIOBase threadedIOInstance;

	ThreadedFileIOBase(const ThreadedFileIOBase &) = delete;
	ThreadedFileIOBase &operator=(const ThreadedFileIOBase &) = delete;
	~ThreadedFileIOBase();

	void queueIO(IThreadedFileIO *task);
	void waitForFinish();
	void cancelTask(IThreadedFileIO *task);
	// Drain the queue, then stop and join the worker. Idempotent, and safe
	// from any thread except the worker itself. The destructor performs only
	// the stop (its caller has already waited for its data); this variant is
	// for exit paths that cannot rely on the destructor running at all -- see
	// the 3DS atexit hook in main_3ds.cpp, whose runtime never walks
	// __libc_fini_array, so global destructors (including ~ThreadedFileIOBase)
	// do not execute and the worker would otherwise outlive the process heap.
	void shutdown();

private:
	ThreadedFileIOBase();
	void run();
	void processQueue();

	std::vector<IThreadedFileIO *> threadedIOQueue;
	std::unordered_set<IThreadedFileIO *> requeueRequested;
	IThreadedFileIO *activeTask;
	std::mutex queueMutex;
	std::condition_variable queueCondition;
	std::condition_variable finishCondition;
	std::thread worker;
	std::uint64_t writeQueuedCounter;
	std::uint64_t savedIOCounter;
	bool isThreadWaiting;
	bool stopping;
};
