/*
===========================================================================
Job system, see q_jobs.h.

Synchronisation: one loop descriptor (fn, ctx, count) guarded by a mutex, a generation number
that changes for every loop, and two atomics per loop: the next unclaimed index and the number of
finished indices. A worker enters a loop under the mutex (so it reads a complete descriptor) and
counts itself in "busy"; the main thread only sets up a new loop while it holds the mutex and no
worker is still inside the previous one. Workers spin for a moment after a loop (a frame runs
several loops back to back), then sleep on a condition variable.
===========================================================================
*/
#include "q_shared.h"
#include "qcommon.h"
#include "q_jobs.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>

#if defined( _MSC_VER ) && ( defined( _M_IX86 ) || defined( _M_X64 ) )
#include <intrin.h>
#define JOBS_PAUSE() _mm_pause()
#elif ( defined( __i386__ ) || defined( __x86_64__ ) )
#define JOBS_PAUSE() __asm__ __volatile__( "pause" )	// (rep nop: any x86, no SSE2 needed)
#elif defined( __aarch64__ ) || defined( __arm__ )
#define JOBS_PAUSE() __asm__ __volatile__( "yield" )
#else
#define JOBS_PAUSE() ( (void)0 )
#endif

static cvar_t *com_jobs = NULL;
static cvar_t *com_jobsSpin = NULL;

static thread_local int jobThreadIndex = 0;

namespace {

struct JobPool {
	std::thread					workers[JOBS_MAX_THREADS];
	int							numWorkers = 0;
	std::mutex					m;
	std::condition_variable		cvWork;
	bool						quit = false;

	// the current loop (written under m)
	jobFunc_t					fn = nullptr;
	void						*ctx = nullptr;
	int							count = 0;
	std::atomic<unsigned>		generation{ 0 };
	std::atomic<int>			next{ 0 };
	std::atomic<int>			done{ 0 };
	std::atomic<int>			busy{ 0 };		// workers inside a loop
	std::atomic<int>			active{ 0 };	// main thread inside Com_JobsParallelFor
	std::atomic<int>			spinUs{ 100 };
	unsigned long long			loops = 0;
};

JobPool pool;

void RunItems( jobFunc_t fn, void *ctx, int count, int thread ) {
	for ( ;; ) {
		const int i = pool.next.fetch_add( 1, std::memory_order_relaxed );
		if ( i >= count ) {
			return;
		}
		fn( ctx, i, thread );
		pool.done.fetch_add( 1, std::memory_order_acq_rel );
	}
}

void WorkerMain( int index ) {
	jobThreadIndex = index;
	unsigned seen = pool.generation.load( std::memory_order_acquire );
	for ( ;; ) {
		// wait for a new loop: spin first, then sleep
		{
			const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds( pool.spinUs.load( std::memory_order_relaxed ) );
			int k = 0;
			while ( pool.generation.load( std::memory_order_acquire ) == seen && !pool.quit ) {
				JOBS_PAUSE();
				if ( ( ++k & 63 ) == 0 && std::chrono::steady_clock::now() >= spinUntil ) {
					break;
				}
			}
		}
		jobFunc_t fn;
		void *ctx;
		int count;
		{
			std::unique_lock<std::mutex> lk( pool.m );
			pool.cvWork.wait( lk, [&] { return pool.quit || pool.generation.load( std::memory_order_acquire ) != seen; } );
			if ( pool.quit ) {
				return;
			}
			seen = pool.generation.load( std::memory_order_acquire );
			fn = pool.fn;
			ctx = pool.ctx;
			count = pool.count;
			pool.busy.fetch_add( 1, std::memory_order_acq_rel );
		}
		RunItems( fn, ctx, count, index );
		pool.busy.fetch_sub( 1, std::memory_order_acq_rel );
	}
}

void StopWorkers( void ) {
	if ( !pool.numWorkers ) {
		return;
	}
	{
		std::lock_guard<std::mutex> lk( pool.m );
		pool.quit = true;
	}
	pool.cvWork.notify_all();
	for ( int i = 0; i < pool.numWorkers; i++ ) {
		if ( pool.workers[i].joinable() ) {
			pool.workers[i].join();
		}
	}
	pool.numWorkers = 0;
	pool.quit = false;
}

void StartWorkers( int n ) {
	if ( n > JOBS_MAX_THREADS - 1 ) {
		n = JOBS_MAX_THREADS - 1;
	}
	for ( int i = 0; i < n; i++ ) {
		pool.workers[i] = std::thread( WorkerMain, i + 1 );
	}
	pool.numWorkers = n;
}

int WantedWorkers( void ) {
	int n = com_jobs ? com_jobs->integer : 0;
	if ( n < 0 ) {
		const unsigned hw = std::thread::hardware_concurrency();
		n = hw > 1 ? (int)hw - 1 : 0;
	}
	if ( n > JOBS_MAX_THREADS - 1 ) {
		n = JOBS_MAX_THREADS - 1;
	}
	return n;
}

} // namespace

void Com_JobsInit( void ) {
	com_jobs = Cvar_Get( "com_jobs", "-1", CVAR_ARCHIVE_ND,
		"Worker threads of the job system (parallel server work such as bot sensing): -1 = one per hardware thread beyond the first, 0 = off (everything serial, the original code paths)" );
	com_jobsSpin = Cvar_Get( "com_jobsSpin", "100", CVAR_ARCHIVE_ND, "Microseconds a worker thread keeps spinning for the next parallel loop before it sleeps" );
	com_jobs->modified = qtrue;
	Com_JobsApplyCvar();
}

void Com_JobsShutdown( void ) {
	StopWorkers();
}

void Com_JobsApplyCvar( void ) {
	if ( !com_jobs ) {
		return;
	}
	if ( com_jobsSpin->modified ) {
		com_jobsSpin->modified = qfalse;
		pool.spinUs.store( com_jobsSpin->integer < 0 ? 0 : com_jobsSpin->integer );
	}
	if ( !com_jobs->modified ) {
		return;
	}
	com_jobs->modified = qfalse;
	const int want = WantedWorkers();
	if ( want == pool.numWorkers ) {
		return;
	}
	StopWorkers();
	if ( want > 0 ) {
		StartWorkers( want );
	}
	Com_Printf( "Job system: %d worker thread%s (%u hardware threads)\n", pool.numWorkers, pool.numWorkers == 1 ? "" : "s", std::thread::hardware_concurrency() );
}

int Com_JobsThreads( void ) {
	return pool.numWorkers + 1;
}

int Com_JobsThreadIndex( void ) {
	return jobThreadIndex;
}

int Com_JobsActive( void ) {
	return pool.active.load( std::memory_order_relaxed );
}

unsigned long long Com_JobsLoops( void ) {
	return pool.loops;
}

void Com_JobsParallelFor( int count, jobFunc_t fn, void *ctx ) {
	if ( count <= 0 ) {
		return;
	}
	if ( !pool.numWorkers || count == 1 || jobThreadIndex != 0 || pool.active.load( std::memory_order_relaxed ) ) {
		for ( int i = 0; i < count; i++ ) {
			fn( ctx, i, jobThreadIndex );
		}
		return;
	}
	pool.active.store( 1, std::memory_order_relaxed );
	pool.loops++;
	{
		std::unique_lock<std::mutex> lk( pool.m );
		// a worker may still be leaving the previous loop
		while ( pool.busy.load( std::memory_order_acquire ) != 0 ) {
			lk.unlock();
			JOBS_PAUSE();
			lk.lock();
		}
		pool.fn = fn;
		pool.ctx = ctx;
		pool.count = count;
		pool.next.store( 0, std::memory_order_relaxed );
		pool.done.store( 0, std::memory_order_relaxed );
		pool.generation.fetch_add( 1, std::memory_order_release );
	}
	pool.cvWork.notify_all();
	RunItems( fn, ctx, count, 0 );
	int k = 0;
	while ( pool.done.load( std::memory_order_acquire ) < count ) {
		if ( ++k < 4096 ) {
			JOBS_PAUSE();
		} else {
			std::this_thread::yield();
		}
	}
	pool.active.store( 0, std::memory_order_relaxed );
}
