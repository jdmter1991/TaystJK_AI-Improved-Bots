/*
===========================================================================
Job system: a fixed pool of worker threads for data-parallel loops.

	Com_JobsParallelFor( count, fn, ctx ) runs fn( ctx, index, thread ) for every index in
	[0, count). The calling thread takes part; the call returns when every index is done.
	"thread" is 0 for the calling (main) thread and 1..Com_JobsThreads()-1 for the workers, so
	a job can use per-thread scratch memory. Which thread runs which index depends on timing:
	a job must write its results only to memory owned by its index, and the caller merges them
	in index order afterwards, so the outcome does not depend on the timing.

	The pool size comes from the cvar com_jobs (-1 = one worker per hardware thread beyond the
	first, at most JOBS_MAX_THREADS-1, the default; 0 = no workers: every loop runs serially on the
	calling thread, exactly like the code before the job system). It is applied between frames.

Nothing is allocated while a loop runs. Only one parallel loop runs at a time, started from the
main thread.
===========================================================================
*/
#pragma once

#define JOBS_MAX_THREADS	16		// main thread + up to 15 workers

typedef void ( *jobFunc_t )( void *ctx, int index, int thread );

#ifdef __cplusplus
extern "C" {
#endif

void	Com_JobsInit( void );
void	Com_JobsShutdown( void );
void	Com_JobsApplyCvar( void );			// main thread, between frames: start/stop workers after com_jobs changed
int		Com_JobsThreads( void );			// threads taking part in a parallel loop (1 = serial)
void	Com_JobsParallelFor( int count, jobFunc_t fn, void *ctx );
int		Com_JobsThreadIndex( void );		// 0 on the main thread, 1.. on the workers
int		Com_JobsActive( void );				// a parallel loop is running
unsigned long long Com_JobsLoops( void );	// parallel loops run so far (statistics)

#ifdef __cplusplus
}
#endif
