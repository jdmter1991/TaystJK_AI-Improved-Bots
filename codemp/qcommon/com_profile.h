/*
===========================================================================
Lightweight server/collision profiling counters.

Enabled with the "sv_profile <seconds>" cvar (0 = off). When enabled the server
prints one SVPROF line every <seconds> with frame-time, bot-AI, game-simulation,
snapshot and world-trace statistics. Counters are plain integers and are always
maintained; timestamps are only taken while profiling is enabled.
===========================================================================
*/
#pragma once

#include <chrono>
#include <cstdint>

// engine-side parts of the frame, timed where the game calls into the engine (Ghoul2) and in the
// snapshot code; they overlap the phases above (the Ghoul2 parts are inside "game" and "botai")
enum comProfPart_e {
	CPP_G2BOLT = 0,		// G2API_GetBoltMatrix* (skeleton transforms for bolts: saber, hands, head)
	CPP_G2COLL,			// Ghoul2 collision: G2API_CollisionDetect* and Ghoul2 hit tests inside traces
	CPP_G2ANIM,			// G2API_SetBoneAnim / SetBoneAngles / GetBoneAnim and other Ghoul2 state calls
	CPP_SNAPBUILD,		// SV_BuildClientSnapshot (entity culling, PVS)
	CPP_SNAPSEND,		// writing and sending snapshots to network clients
	CPP_JOBS,			// wall time of parallel sections (main thread waiting included)
	CPP_NUM_PARTS
};

enum comProfPhase_e {
	CPROF_OTHER = 0,	// client packets (human usercmds), misc
	CPROF_BOTAI,		// SV_BotFrame -> BotAIStartFrame
	CPROF_GAME,			// GVM_RunFrame (entity think, bot usercmd execution, physics)
	CPROF_SNAPSHOT,		// SV_SendClientMessages
	CPROF_NUM_PHASES
};

// collision-model work counters
typedef struct comProfCm_s {
	uint64_t	leafVisits;			// leaves handed to CM_TraceThroughLeaf / CM_TestInLeaf
	uint64_t	leafBrushesWalked;	// leaf-brush references iterated
	uint64_t	brushesTested;		// brushes that passed the bounds reject (plane tests run)
	uint64_t	accelNodes;			// accelerator nodes visited (0 when the accelerator is off)
} comProfCm_t;

typedef struct comProfile_s {
	int			enabled;
	int			phase;

	// SV_Trace / SV_PointContents, split by phase
	uint64_t	svTraces[CPROF_NUM_PHASES];
	uint64_t	svTraceNs[CPROF_NUM_PHASES];		// world + entity clipping
	uint64_t	svWorldTraceNs[CPROF_NUM_PHASES];	// world (CM_BoxTrace model 0) only
	uint64_t	svPointContents[CPROF_NUM_PHASES];
	uint64_t	svPointContentsNs[CPROF_NUM_PHASES];

	// collision-model work (all callers on the main thread; the job threads count in comProfThread_t)
	comProfCm_t	cm;

	// slowest single SV_Trace since the server last cleared it (once per frame)
	uint64_t	worstTraceNs;
	float		worstTraceStart[3], worstTraceEnd[3], worstTraceMins[3], worstTraceMaxs[3];
	int			worstTracePhase;

	// time spent in Com_Printf (console window, log file): taken out of the SVPROF frame times,
	// since printing the profile itself could stall a frame for a second on a busy machine
	uint64_t	printNs;

	// engine-side parts (comProfPart_e): time in the current frame and calls since start
	uint64_t	partFrameNs[CPP_NUM_PARTS];
	uint64_t	partCalls[CPP_NUM_PARTS];
} comProfile_t;

extern comProfile_t com_prof;

// counters of the job threads (index = job thread, 0 unused); merged into com_prof by the code that
// started the parallel loop, once the loop has finished
typedef struct comProfThread_s {
	comProfCm_t	cm;
	uint64_t	svTraces, svTraceNs, svWorldTraceNs;
	uint64_t	svPointContents;
} comProfThread_t;
extern comProfThread_t com_profThread[16];	// JOBS_MAX_THREADS

// the collision counters of the calling thread
comProfCm_t *CM_ProfCounters( void );

static inline uint64_t Com_ProfNowNs( void ) {
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// times one engine-side part while profiling is on (main thread only: the parallel jobs book their
// own counters)
struct ComProfPart {
	int			part;
	uint64_t	t0;
	explicit ComProfPart( int p ) : part( p ), t0( com_prof.enabled ? Com_ProfNowNs() : 0 ) {}
	~ComProfPart() {
		if ( t0 ) {
			com_prof.partFrameNs[part] += Com_ProfNowNs() - t0;
			com_prof.partCalls[part]++;
		}
	}
};
