/*
===========================================================================
Game frame profiler, see g_prof.h.
===========================================================================
*/
#include "g_local.h"

#ifdef _WIN32
__declspec(dllimport) int __stdcall QueryPerformanceCounter( long long *lpPerformanceCount );
__declspec(dllimport) int __stdcall QueryPerformanceFrequency( long long *lpFrequency );
static double GProf_NowUs( void ) {
	static double usPerTick = 0.0;
	long long t;
	if ( usPerTick == 0.0 ) {
		long long f;
		QueryPerformanceFrequency( &f );
		usPerTick = 1000000.0 / (double)f;
	}
	QueryPerformanceCounter( &t );
	return (double)t * usPerTick;
}
#else
#include <time.h>
static double GProf_NowUs( void ) {
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}
#endif

static const char *gProfZoneNames[GPZ_MAX] = {
	"G_RunFrame upkeep (not below)",
	"client think (around Pmove)",
	"Pmove (movement physics)",
	"client events",
	"touch triggers",
	"saber position (blades, ghoul2 bolts)",
	"saber collision (CheckSaberDamage)",
	"saber missile block check",
	"force powers update",
	"missiles",
	"items and physics objects",
	"movers",
	"entity think functions",
	"client end frame",
	"rules, votes, queues",
	"icarus, roff",
};

#define GPROF_HIST		2000	// 0.01 ms steps up to 20 ms per zone per frame
#define GPROF_STEP_US	10.0
#define GPROF_DEPTH		32

typedef struct {
	double	selfUs;			// window sum
	double	frameUs;		// this frame
	double	maxFrameUs;
	double	inclUs;
	int		calls;
	int		hist[GPROF_HIST + 1];
} gProfZoneAcc_t;

int gProfEnabled = 0;

static struct {
	double			windowStartUs;
	double			frameStartUs;
	int				frames;
	double			totalUs, maxTotalUs;
	int				histTotal[GPROF_HIST + 1];
	gProfZoneAcc_t	z[GPZ_MAX];
	int				stack[GPROF_DEPTH];
	double			stackStart[GPROF_DEPTH];
	int				depth;
	double			last;
} gProf;

void GProf_Push( int zone ) {
	const double now = GProf_NowUs();
	if ( gProf.depth > 0 ) {
		gProf.z[gProf.stack[gProf.depth - 1]].frameUs += now - gProf.last;
	}
	if ( gProf.depth >= GPROF_DEPTH ) {
		gProf.depth++;		// too deep: count the level, book nothing
		gProf.last = now;
		return;
	}
	gProf.stack[gProf.depth] = zone;
	gProf.stackStart[gProf.depth] = now;
	gProf.depth++;
	gProf.z[zone].calls++;
	gProf.last = now;
}

void GProf_Pop( void ) {
	const double now = GProf_NowUs();
	int zone;
	if ( gProf.depth <= 0 ) {
		return;
	}
	gProf.depth--;
	if ( gProf.depth >= GPROF_DEPTH ) {
		gProf.last = now;
		return;
	}
	zone = gProf.stack[gProf.depth];
	gProf.z[zone].frameUs += now - gProf.last;
	gProf.z[zone].inclUs += now - gProf.stackStart[gProf.depth];
	gProf.last = now;
}

static int GProf_Pct( const int *hist, int total, double pct ) {
	int i, acc = 0;
	const int want = (int)( total * pct / 100.0 );
	for ( i = 0; i <= GPROF_HIST; i++ ) {
		acc += hist[i];
		if ( acc > want ) {
			return i;
		}
	}
	return GPROF_HIST;
}

static int QDECL GProf_Compare( const void *a, const void *b ) {
	const double d = gProf.z[*(const int *)b].selfUs - gProf.z[*(const int *)a].selfUs;
	return d > 0 ? 1 : ( d < 0 ? -1 : 0 );
}

static void GProf_Report( double nowUs ) {
	int order[GPZ_MAX], i, clients = 0, bots = 0;
	const double f = gProf.frames > 0 ? gProf.frames : 1;
	double sum = 0.0;
	for ( i = 0; i < level.maxclients; i++ ) {
		if ( g_entities[i].inuse && g_entities[i].client && g_entities[i].client->pers.connected == CON_CONNECTED ) {
			clients++;
			if ( g_entities[i].r.svFlags & SVF_BOT ) bots++;
		}
	}
	for ( i = 0; i < GPZ_MAX; i++ ) {
		order[i] = i;
		sum += gProf.z[i].selfUs;
	}
	trap->Print( "GAMEPROF window=%.1fs frames=%d clients=%d bots=%d entities=%d  game ms/frame avg %.3f p99 %.2f max %.3f  zones cover %.1f%%\n",
		( nowUs - gProf.windowStartUs ) / 1e6, gProf.frames, clients, bots, level.num_entities,
		gProf.totalUs / f / 1e3, GProf_Pct( gProf.histTotal, gProf.frames, 99 ) * GPROF_STEP_US / 1e3, gProf.maxTotalUs / 1e3,
		gProf.totalUs > 0 ? 100.0 * sum / gProf.totalUs : 0.0 );
	qsort( order, GPZ_MAX, sizeof( order[0] ), GProf_Compare );
	for ( i = 0; i < GPZ_MAX; i++ ) {
		const gProfZoneAcc_t *zz = &gProf.z[order[i]];
		if ( !zz->calls ) {
			continue;
		}
		trap->Print( "GAMEZONE %-40s self ms/frame avg %7.4f p99 %6.3f max %7.3f %5.1f%%  calls/frame %7.2f  incl us/call %7.2f\n",
			gProfZoneNames[order[i]], zz->selfUs / f / 1e3, GProf_Pct( zz->hist, gProf.frames, 99 ) * GPROF_STEP_US / 1e3, zz->maxFrameUs / 1e3,
			sum > 0 ? 100.0 * zz->selfUs / sum : 0.0, zz->calls / f, zz->inclUs / zz->calls );
	}
}

void GProf_FrameBegin( void ) {
	const int want = g_profile.integer > 0;
	if ( want != gProfEnabled ) {
		gProfEnabled = want;
		memset( &gProf, 0, sizeof( gProf ) );
	}
	if ( !gProfEnabled ) {
		return;
	}
	gProf.frameStartUs = GProf_NowUs();
	if ( gProf.windowStartUs == 0.0 ) {
		gProf.windowStartUs = gProf.frameStartUs;
	}
	gProf.depth = 0;
	GProf_Push( GPZ_FRAME );
}

void GProf_FrameEnd( void ) {
	double now, total;
	int i;
	if ( !gProfEnabled ) {
		return;
	}
	while ( gProf.depth > 0 ) {
		GProf_Pop();
	}
	now = GProf_NowUs();
	total = now - gProf.frameStartUs;
	gProf.frames++;
	gProf.totalUs += total;
	if ( total > gProf.maxTotalUs ) gProf.maxTotalUs = total;
	{
		int b = (int)( total / GPROF_STEP_US );
		gProf.histTotal[b > GPROF_HIST ? GPROF_HIST : b]++;
	}
	for ( i = 0; i < GPZ_MAX; i++ ) {
		gProfZoneAcc_t *zz = &gProf.z[i];
		int b = (int)( zz->frameUs / GPROF_STEP_US );
		zz->hist[b > GPROF_HIST ? GPROF_HIST : b]++;
		zz->selfUs += zz->frameUs;
		if ( zz->frameUs > zz->maxFrameUs ) zz->maxFrameUs = zz->frameUs;
		zz->frameUs = 0.0;
	}
	if ( now - gProf.windowStartUs >= g_profile.integer * 1e6 ) {
		GProf_Report( now );
		memset( &gProf, 0, sizeof( gProf ) );
		gProf.windowStartUs = now;
	}
}
