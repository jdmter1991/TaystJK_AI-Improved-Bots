/*
===========================================================================
tr_tworld.cpp -- cooked static world ("tworld"), prototype

A map may ship maps/<name>.tworld next to its BSP. The file holds a table of
unique meshes, their per-material index ranges and a table of placements
(instances). The BSP keeps collision, entities and spawns; this file is only
drawn.

This prototype deliberately reuses the existing MD3 vertex-buffer path
(SF_VBO_MDVMESH, vbo_mdv pipelines) and ordinary shader_t materials, so no
new SPIR-V is needed. Each visible placement costs one draw call per
material range (no GPU instancing).

File version 2 adds: a constant tint per material, a sun and an ambient
colour (applied through "rgbGen lightingDiffuse" with a private entity
uniform per placement), up to three levels of detail per mesh chosen by
projected size, and culling of placements that are too small on screen.

Materials flagged as blended (water, glass, decals, glow) are drawn by a
second draw surface that sorts with the other blended shaders, after all
opaque geometry; within it placements are not depth-sorted.

The whole pass is one draw surface (SF_TWORLD) owned by the world entity;
its back-end function culls placements against the view frustum and issues
the draws itself, so the 4095 render-entity and 65536 draw-surface limits do
not apply to placements.
===========================================================================
*/

#include "tr_local.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// CPU time of the cooked-world pass per section, summed between two statistics lines (r_tworldStats)
static double s_twTime[4];
static inline double TW_Now( void ) { return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now().time_since_epoch() ).count(); }

/*
Worker pool for the cooked world's per-frame CPU work (r_tworldThreads). A job set is a function and a job count;
the calling thread and the workers take job numbers from one counter until none are left, and the call returns when
every job has finished. Jobs write only their own output ranges, so the result does not depend on which thread ran
which job or in what order. Threads are started once (and again only when r_tworldThreads changes), nothing is
allocated per frame, and the pool stops before the renderer is unloaded (R_TWorldShutdownThreads).
*/
#define TW_MAX_THREADS	16
#define TW_CULL_JOBS	32		// culling jobs per view: several per thread, so threads that finish early take more
#define TW_FILL_JOBS	16		// instance-data jobs per pass at most (two per thread)

typedef void ( *twJobFunc_t )( void *ctx, int job );

static struct {
	std::thread					threads[TW_MAX_THREADS];
	int							numThreads;
	std::mutex					lock;
	std::condition_variable		wake, finished;
	unsigned					generation;		// bumped for every job set
	qboolean					quit;
	twJobFunc_t					func;
	void						*ctx;
	std::atomic<int>			numJobs;
	std::atomic<int>			nextJob;
	int							jobsDone;		// under lock
} s_twPool;

static void TW_PoolRunJobs( void ) {
	int done = 0, job;
	while ( ( job = s_twPool.nextJob.fetch_add( 1 ) ) < s_twPool.numJobs.load() ) {
		s_twPool.func( s_twPool.ctx, job );
		done++;
	}
	if ( done ) {
		std::lock_guard<std::mutex> g( s_twPool.lock );
		s_twPool.jobsDone += done;
		if ( s_twPool.jobsDone >= s_twPool.numJobs.load() )
			s_twPool.finished.notify_one();
	}
}

static void TW_PoolWorker( void ) {
	unsigned seen = 0;
	for ( ;; ) {
		{
			std::unique_lock<std::mutex> g( s_twPool.lock );
			s_twPool.wake.wait( g, [&] { return s_twPool.quit || s_twPool.generation != seen; } );
			if ( s_twPool.quit )
				return;
			seen = s_twPool.generation;
		}
		TW_PoolRunJobs();
	}
}

static void TW_PoolStop( void ) {
	int i;
	if ( !s_twPool.numThreads )
		return;
	{
		std::lock_guard<std::mutex> g( s_twPool.lock );
		s_twPool.quit = qtrue;
	}
	s_twPool.wake.notify_all();
	for ( i = 0; i < s_twPool.numThreads; i++ ) {
		if ( s_twPool.threads[i].joinable() )
			s_twPool.threads[i].join();
	}
	s_twPool.numThreads = 0;
	s_twPool.quit = qfalse;
}

// worker threads besides the calling one: n < 0 = automatic (hardware threads - 1, at most 7), 0 = none
static int TW_PoolResolve( int n ) {
	if ( n < 0 ) {
		const unsigned hw = std::thread::hardware_concurrency();
		n = hw > 1 ? (int)hw - 1 : 0;
		if ( n > 7 )
			n = 7;
	}
	return n > TW_MAX_THREADS ? TW_MAX_THREADS : n;
}

static void TW_PoolSize( int n ) {
	int i;
	n = TW_PoolResolve( n );
	if ( n == s_twPool.numThreads )
		return;
	TW_PoolStop();
	for ( i = 0; i < n; i++ ) {
		try {
			s_twPool.threads[i] = std::thread( TW_PoolWorker );
		} catch ( ... ) {
			break;			// could not start a thread: work with the ones there are
		}
		s_twPool.numThreads++;
	}
}

// runs func( ctx, 0 .. numJobs - 1 ) on the calling thread and the workers; returns when all have finished
static void TW_PoolRun( twJobFunc_t func, void *ctx, int numJobs ) {
	int i;
	if ( numJobs <= 0 )
		return;
	if ( !s_twPool.numThreads || numJobs == 1 ) {
		for ( i = 0; i < numJobs; i++ )
			func( ctx, i );
		return;
	}
	{
		std::lock_guard<std::mutex> g( s_twPool.lock );
		s_twPool.func = func;
		s_twPool.ctx = ctx;
		s_twPool.numJobs.store( numJobs );
		s_twPool.nextJob.store( 0 );
		s_twPool.jobsDone = 0;
		s_twPool.generation++;
	}
	s_twPool.wake.notify_all();
	TW_PoolRunJobs();
	std::unique_lock<std::mutex> g( s_twPool.lock );
	s_twPool.finished.wait( g, [] { return s_twPool.jobsDone >= s_twPool.numJobs.load(); } );
}

#ifdef USE_VBO_MDV
static void TW_CheckFree( void );
#endif

// stop the workers before the renderer is unloaded (RE_Shutdown)
void R_TWorldShutdownThreads( void ) {
	TW_PoolStop();
#ifdef USE_VBO_MDV
	TW_CheckFree();
#endif
}

#ifdef USE_VBO_MDV

#define TWORLD_MAGIC	"TWLD"
#define TWORLD_VERSION	2
#define TWORLD_MAX_LODS	3

#pragma pack(push, 1)
typedef struct {
	char		magic[4];
	uint32_t	version;
	uint32_t	numMaterials;
	uint32_t	numMeshes;
	uint32_t	numSubmeshes;
	uint32_t	numInstances;
	uint32_t	numVerts;
	uint32_t	numIndexes;
	float		mins[3];
	float		maxs[3];
	float		unitsPerMetre;
	float		sunDir[3];		// unit vector pointing towards the sun
	float		sunColor[3];
	float		ambientColor[3];
	uint32_t	reserved[4];
} twHeader_t;				// 112 bytes

typedef struct {
	char		shader[64];
	float		tint[3];
	uint32_t	flags;			// TWMAT_*
} twMaterial_t;				// 80 bytes

#define TWMAT_BLEND		1		// drawn in the blended pass
// flags bits 1-3: animation (0 none, 1 water ripples, 2 vegetation sway, 3 cloth); the instanced path only.
// flags bits 8-15: opacity * 255 (0 = opaque); bits 16-31: detail texture repeats per unit of UV (0 = none).
// The detail texture is the image of the material shader's second stage.

typedef struct {
	uint32_t	firstVert, numVerts;
	uint32_t	firstIndex, numIndexes;
	uint32_t	numLods;
	uint32_t	lodFirstSub[TWORLD_MAX_LODS];
	uint32_t	lodNumSubs[TWORLD_MAX_LODS];
	float		center[3];
	float		radius;
} twMesh_t;					// 60 bytes

typedef struct {
	uint32_t	material;
	uint32_t	firstIndex;		// relative to the mesh's first index
	uint32_t	numIndexes;
	uint32_t	reserved;
} twSubmesh_t;				// 16 bytes

typedef struct {
	uint32_t	mesh;
	float		m[12];			// row-major 3x4: world = m * (x, y, z, 1)
	float		center[3];		// world-space bounding sphere
	float		radius;
} twInstance_t;				// 68 bytes

typedef struct {
	float		xyz[3];
	float		normal[3];
	float		st[2];
} twVertex_t;				// 32 bytes
#pragma pack(pop)

typedef struct {
	shader_t		*shader;
	int				tintIndex;		// index into tworld_t::tints
	int				pass;			// 0 opaque, 1 blended
	uint32_t		pipeline[2];	// instanced pipeline index + 1 (normal view, mirror view); 0 = not created yet
	float			texNoScale;		// 1 = base texture is a pre-compressed image without the upload light scale
	srfVBOMDVMesh_t	surf;
} tworldSub_t;

typedef struct {
	int				numLods;
	int				lodFirstSub[TWORLD_MAX_LODS];
	int				lodNumSubs[TWORLD_MAX_LODS];
	int				lodPassMask[TWORLD_MAX_LODS];	// bit 0: has opaque ranges, bit 1: has blended ranges
	float			zmin, zmax;		// vertical extent in mesh space (animation reference heights)
} tworldMesh_t;

typedef struct {
	int				mesh;
	float			matrix[16];		// column-major model matrix
	vec3_t			center;
	float			radius;
} tworldInst_t;

typedef struct twCell_s {
	vec3_t	center;
	float	radius;			// bounding sphere of the placements' spheres
	float	maxInstRadius;
	int		first, count;	// range in cellOrder
} twCell_t;

#define TW_SMALL_RADIUS		160.0f		// placements up to this radius go to the fine grid
#define TW_CELL_SMALL		1024.0f
#define TW_CELL_LARGE		8192.0f

typedef struct tworld_s {
	surfaceType_t	surfaceType;	// SF_TWORLD, must be first: the opaque pass
	surfaceType_t	blendSurfaceType;	// SF_TWORLD again: the blended pass
	shader_t		*blendSortShader;	// a blended material's shader, used only to sort the blended pass

	int				numMeshes, numSubs, numInstances;
	tworldMesh_t	*meshes;
	tworldSub_t		*subs;
	tworldInst_t	*instances;

	vec3_t			mins, maxs;
	int				numVerts, numIndexes;

	int				numTints;
	vec3_t			*tints;			// one per material
	float			*alphas;		// one per material: opacity for the instanced path
	float			*details;		// one per material: detail repeats per unit of UV, 0 = none
	byte			*anims;			// one per material: animation type

	// instanced path, per-frame scratch
	int				*visInst;		// visible placement indexes
	int				*visAllInst;	// placements that passed the view tests this view (both passes), and their detail levels
	byte			*visAllLod;
	int				numVisAll;
	uint32_t		*visKey;		// distance bucket of each tested placement, then scratch for the sort
	int				*sortInst;
	int				*visAllMesh, *sortMesh;	// mesh of each tested placement
	byte			*sortLod;
	int				*drawList;		// submeshes with something to draw in this pass, nearest first
	struct twCullRec_s	*cullRecs;	// one compact record per placement, in cell order
	// the culling split into jobs (r_tworldThreads): job j tests cells cullJobFirst[j] .. cullJobFirst[j + 1] - 1 and
	// writes its placements at the first record index of its cells; the sort reads the jobs' results in job order
	int				numCullJobs;
	int				cullJobFirst[TW_CULL_JOBS + 1];
	int				cullJobBase[TW_CULL_JOBS], cullJobCount[TW_CULL_JOBS], cullJobSmall[TW_CULL_JOBS];
	int				*fillCount;		// TW_FILL_JOBS rows of numSubs + 1: records per job and material range, then each job's first position
	// interior visibility (maps/<name>.tportal): an axis-aligned interior box and the openings out of it
	qboolean		hasPortal;
	vec3_t			portalMins, portalMaxs;
	int				numPortals;
	float			portals[256][6];	// axis (0 = plane x, 1 = plane y), plane position, centre along the other axis, centre z, half width, half height

	// placements grouped into cells of a grid, small ones and large ones separately, so that whole cells can be
	// rejected by the view frustum and, for small placements, by distance
	struct twCell_s	*cells;
	int				numCells;
	int				*cellOrder;		// placement indexes, cell after cell
	byte			*visLod;
	int				*batchCount, *batchStart, *batchCursor;	// per submesh
	int				*subOrder;		// submesh indexes sorted by shader
	vec3_t			sunDir, sunColor, ambientColor;
	vec3_t			skyZenith, skyHorizon;	// display-space sky colours for water reflections
	float			metresPerUnit;
	float			originM[2];			// source-scene x, y (metres) of the world origin, for world-projected textures

	// physically based path: optional per-material maps and values (maps/<name>.tpbr)
	image_t			**pbrImages;		// normal map per material, or NULL
	float			*pbrScales;			// normal map repeats per unit of UV
	float			*pbrPacks;			// roughness * 255 + 256 * metallic * 255 + 65536 * flags
	image_t			**tileImages;		// tiling colour layer per material, or NULL
	float			*tilePacks;			// 2 * repeats in U + 1024 * repeats in V
	int				numPbrMaps;

	// sky occlusion per placement (maps/<name>.tsky): 0 = open sky .. 255 = enclosed; NULL without the file
	byte			*skyOcc;

	// local lights (maps/<name>.tlights): lamps that light their surroundings
	int				numLights;
	float			*lights;			// 8 floats each: position, radius, colour (already times intensity), unused
	// every lamp at any distance (maps/<name>.tlampocc + rows below the sun map in <name>_shadow.dds, made by
	// tools/bake_lamp_occlusion.py): each placement lists the lamps that reach it (8 indices + 1, two per float);
	// the shader tests each against the lamp's baked occlusion map. 0 = not available (the four nearest lamps are used)
	int				lampSunHeight;		// texel row where the lamp rows start = height of the sun map
	int				lampTileRow;		// texel row of the first occlusion tile
	float			(*instLamps)[4];

	// baked sun shadow map (maps/<name>_shadow.dds + maps/<name>.tshadow), NULL when the map has none
	image_t			*shadowImage;
	float			shadowRow[3][4];	// world -> texel x, texel y, depth 0..1
	float			shadowTexel;		// world units per texel
	float			shadowRange;		// world units covered by depth 0..1
	int				shadowLowW, shadowLowH;
	float			shadowLowScale[2];	// full-size texels per texel of the reduced copy
	uint16_t		*shadowLow;			// reduced copy for CPU lookups

	// statistics of the last view drawn
	int				c_instances, c_draws, c_indexes, c_small, c_lod[TWORLD_MAX_LODS], c_uboFail;
	int				lastStatsTime;
} tworld_t;

static tworld_t	*s_tworld;
static cvar_t	*r_tworld;
static cvar_t	*r_tworldStats;
static cvar_t	*r_tworldCullSize;
static cvar_t	*r_tworldLodSize1;
static cvar_t	*r_tworldLodSize2;
static cvar_t	*r_tworldLight;
static cvar_t	*r_tworldInstancing;
static cvar_t	*r_tworldThreads;
static cvar_t	*r_tworldThreadsCheck;
static cvar_t	*r_tworldShadows;
static cvar_t	*r_tworldShadowOffset;
static cvar_t	*r_tworldShadowBias;
static cvar_t	*r_tworldWaterBump;
static cvar_t	*r_tworldPBR;
static cvar_t	*r_tworldFoliageLight;
static cvar_t	*r_tworldHaze;
static cvar_t	*r_tworldLamps;
static cvar_t	*r_tworldLampDebug;
static cvar_t	*r_tworldLampDistance;
static cvar_t	*r_tworldIndoor;
static cvar_t	*r_tworldAO;
static cvar_t	*r_tworldPortal;
static cvar_t	*r_tworldBench;


static const int64_t		*s_cellKeys;
static int TW_CompareCellKeys( const void *a, const void *b )
{
	const int64_t ka = s_cellKeys[*(const int *)a], kb = s_cellKeys[*(const int *)b];
	if ( ka != kb )
		return ( ka < kb ) ? -1 : 1;
	return *(const int *)a - *(const int *)b;
}

/*
=================
TW_BuildCells

Sorts the placements by (size class, grid cell) and records each cell's bounding sphere.
=================
*/
typedef struct twCullRec_s {
	vec3_t	center;
	float	radius;
	int		inst;
	short	numLods;
	short	pad;
	int		mesh;
} twCullRec_t;				// 28 bytes

static void TW_BuildCells( tworld_t *tw )
{
	const int n = tw->numInstances;
	int64_t *keys;
	int i, numCells = 0, c = -1;

	tw->cellOrder = (int *)ri.Hunk_Alloc( sizeof( int ) * ( n + 1 ), h_low );
	if ( !n )
		return;
	keys = (int64_t *)Z_Malloc( sizeof( int64_t ) * n, TAG_TEMP_WORKSPACE, qtrue );
	for ( i = 0; i < n; i++ ) {
		const tworldInst_t *in = &tw->instances[i];
		const int isSmall = ( in->radius <= TW_SMALL_RADIUS ) ? 1 : 0;
		const float cs = isSmall ? TW_CELL_SMALL : TW_CELL_LARGE;
		const int64_t cx = (int64_t)floorf( in->center[0] / cs ) + 100000, cy = (int64_t)floorf( in->center[1] / cs ) + 100000;
		keys[i] = ( (int64_t)isSmall << 40 ) | ( cx << 20 ) | cy;
		tw->cellOrder[i] = i;
	}
	s_cellKeys = keys;
	qsort( tw->cellOrder, n, sizeof( int ), TW_CompareCellKeys );
	for ( i = 0; i < n; i++ ) {
		if ( i == 0 || keys[tw->cellOrder[i]] != keys[tw->cellOrder[i - 1]] )
			numCells++;
	}
	tw->cells = (twCell_t *)ri.Hunk_Alloc( sizeof( twCell_t ) * numCells, h_low );
	tw->numCells = numCells;
	for ( i = 0; i < n; i++ ) {
		if ( i == 0 || keys[tw->cellOrder[i]] != keys[tw->cellOrder[i - 1]] ) {
			c++;
			tw->cells[c].first = i;
		}
		tw->cells[c].count++;
	}
	for ( c = 0; c < numCells; c++ ) {
		twCell_t *cell = &tw->cells[c];
		vec3_t mins, maxs;
		ClearBounds( mins, maxs );
		for ( i = 0; i < cell->count; i++ ) {
			const tworldInst_t *in = &tw->instances[tw->cellOrder[cell->first + i]];
			AddPointToBounds( in->center, mins, maxs );
			if ( in->radius > cell->maxInstRadius )
				cell->maxInstRadius = in->radius;
		}
		VectorAdd( mins, maxs, cell->center );
		VectorScale( cell->center, 0.5f, cell->center );
		cell->radius = Distance( maxs, cell->center ) + cell->maxInstRadius;
	}
	Z_Free( keys );
	tw->cullRecs = (twCullRec_t *)ri.Hunk_Alloc( sizeof( twCullRec_t ) * ( n + 1 ), h_low );
	for ( i = 0; i < n; i++ ) {
		const tworldInst_t *in = &tw->instances[tw->cellOrder[i]];
		VectorCopy( in->center, tw->cullRecs[i].center );
		tw->cullRecs[i].radius = in->radius;
		tw->cullRecs[i].inst = tw->cellOrder[i];
		tw->cullRecs[i].numLods = (short)tw->meshes[in->mesh].numLods;
		tw->cullRecs[i].mesh = in->mesh;
	}
	{
		// culling jobs: runs of whole cells with about the same number of placements each
		int job = 0, acc = 0;
		const int per = ( n + TW_CULL_JOBS - 1 ) / TW_CULL_JOBS;
		tw->cullJobFirst[0] = 0;
		for ( c = 0; c < numCells - 1 && job < TW_CULL_JOBS - 1; c++ ) {
			acc += tw->cells[c].count;
			if ( acc >= per * ( job + 1 ) )
				tw->cullJobFirst[++job] = c + 1;
		}
		tw->numCullJobs = job + 1;
		tw->cullJobFirst[tw->numCullJobs] = numCells;
		for ( job = 0; job < tw->numCullJobs; job++ )
			tw->cullJobBase[job] = tw->cells[tw->cullJobFirst[job]].first;
	}
	ri.Printf( PRINT_ALL, "R_LoadTWorld: %i placements in %i culling cells, %i culling jobs\n", n, numCells, tw->numCullJobs );
}

static const tworldSub_t *s_sortSubs;
static int TW_CompareSubs( const void *a, const void *b )
{
	const shader_t *sa = s_sortSubs[*(const int *)a].shader, *sb = s_sortSubs[*(const int *)b].shader;
	if ( sa != sb )
		return ( sa < sb ) ? -1 : 1;
	return *(const int *)a - *(const int *)b;
}

#pragma pack(push, 1)
typedef struct {
	char		magic[4];		// "TSHD"
	uint32_t	version;		// 1
	uint32_t	width, height;	// of the full-size map in the .dds
	uint32_t	lowWidth, lowHeight;
	float		row[3][4];
	float		texelUnits, depthRange, lowScaleX, lowScaleY;
} twShadowHeader_t;				// 88 bytes, followed by lowWidth * lowHeight uint16 depths
#pragma pack(pop)

/*
=================
R_LoadTWorldShadow

Optional baked sun shadows: a 16-bit depth map seen from the sun (tools: bake_sun_shadow.py) and a small file
with the world -> map transform plus a reduced copy of the map for CPU lookups.
=================
*/
static void R_LoadTWorldShadow( tworld_t *tw, const char *bspName )
{
	char	name[MAX_QPATH], imgName[MAX_QPATH];
	byte	*buffer = NULL;
	long	len;
	const twShadowHeader_t *h;
	image_t	*image;
	size_t	lowBytes;

	COM_StripExtension( bspName, name, sizeof( name ) );
	Q_strncpyz( imgName, name, sizeof( imgName ) );
	Q_strcat( name, sizeof( name ), ".tshadow" );
	Q_strcat( imgName, sizeof( imgName ), "_shadow" );

	len = ri.FS_ReadFile( name, (void **)&buffer );
	if ( len <= 0 || !buffer )
		return;

	h = (const twShadowHeader_t *)buffer;
	if ( len < (long)sizeof( *h ) || memcmp( h->magic, "TSHD", 4 ) || h->version != 1
		|| h->lowWidth < 1 || h->lowHeight < 1 || h->lowWidth > 4096 || h->lowHeight > 4096
		|| (size_t)len != sizeof( *h ) + (size_t)h->lowWidth * h->lowHeight * 2 ) {
		ri.Printf( PRINT_WARNING, "R_LoadTWorldShadow: %s: bad file\n", name );
		ri.FS_FreeFile( buffer );
		return;
	}

	image = R_FindImageFile( imgName, (imgFlags_t)( IMGFLAG_CLAMPTOEDGE | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOSCALE | IMGFLAG_NOLIGHTSCALE ) );
	if ( !image || image->internalFormat != VK_FORMAT_R16_UNORM
		|| image->width != (int)h->width || image->height < (int)h->height ) {		// taller: lamp rows below the sun map
		ri.Printf( PRINT_WARNING, "R_LoadTWorldShadow: %s.dds is missing, not R16 or not the size the .tshadow file names\n", imgName );
		ri.FS_FreeFile( buffer );
		return;
	}

	Com_Memcpy( tw->shadowRow, h->row, sizeof( tw->shadowRow ) );
	tw->shadowTexel = h->texelUnits;
	tw->shadowRange = h->depthRange;
	tw->shadowLowW = (int)h->lowWidth;
	tw->shadowLowH = (int)h->lowHeight;
	tw->shadowLowScale[0] = h->lowScaleX;
	tw->shadowLowScale[1] = h->lowScaleY;
	lowBytes = (size_t)h->lowWidth * h->lowHeight * 2;
	tw->shadowLow = (uint16_t *)ri.Hunk_Alloc( (int)lowBytes, h_low );
	Com_Memcpy( tw->shadowLow, h + 1, lowBytes );
	tw->shadowImage = image;

	ri.Printf( PRINT_ALL, "R_LoadTWorldShadow: %s: %u x %u, %.2f units per texel, CPU copy %u x %u\n",
		imgName, h->width, h->height, h->texelUnits, h->lowWidth, h->lowHeight );
	ri.FS_FreeFile( buffer );
}

/*
=================
TW_SunVisibility

CPU lookup in the reduced shadow map: 1 when the point sees the sun, 0 when it is occluded.
=================
*/
static float TW_SunVisibility( const tworld_t *tw, const vec3_t p )
{
	float	c[3];
	int		i, x, y;

	for ( i = 0; i < 3; i++ )
		c[i] = tw->shadowRow[i][0] * p[0] + tw->shadowRow[i][1] * p[1] + tw->shadowRow[i][2] * p[2] + tw->shadowRow[i][3];
	x = (int)floorf( c[0] / tw->shadowLowScale[0] );
	y = (int)floorf( c[1] / tw->shadowLowScale[1] );
	if ( x < 0 || y < 0 || x >= tw->shadowLowW || y >= tw->shadowLowH )
		return 1.0f;
	// the reduced copy keeps the deepest value of each block; allow two of its texels of slope
	return ( c[2] - 2.0f * tw->shadowTexel * tw->shadowLowScale[0] / tw->shadowRange <= tw->shadowLow[y * tw->shadowLowW + x] / 65535.0f ) ? 1.0f : 0.0f;
}

#pragma pack(push, 1)
typedef struct {
	char		image[64];		// normal map, empty = none
	float		scale;			// its repeats per unit of UV
	float		roughness, metallic;
	uint32_t	flags;			// 1: roughness = 1 - base alpha, 2: metallic = normal map alpha, 4: metallic = normal map blue
} twPbrMaterial_t;				// 80 bytes

typedef struct {
	twPbrMaterial_t	pbr;
	char		tile[64];		// tiling colour layer, empty = none
	float		tileU, tileV;	// its repeats per unit of UV (whole numbers up to 255)
} twPbrMaterial2_t;				// 152 bytes, file version 2
#pragma pack(pop)

/*
=================
R_LoadTWorldLampOcclusion

Optional maps/<name>.tlampocc (tools/bake_lamp_occlusion.py): "TLOC", version 1, lamps, tile size, sun map height,
parameter row, first tile row, tiles per row. The lamp parameters and one octahedral occlusion tile per lamp are
rows of the shadow texture below the sun map. With it every lamp lights what it reaches at any distance; each
placement gets the (up to 8) lamps whose radius touches its bounds, nearest first.
=================
*/
static void R_LoadTWorldLampOcclusion( tworld_t *tw, const char *bspName )
{
	char		name[MAX_QPATH];
	uint32_t	*buf = NULL;
	long		len;
	int			i, k, tooMany = 0, lit = 0;

	tw->lampSunHeight = 0;
	tw->instLamps = NULL;
	if ( !tw->numLights || !tw->shadowImage )
		return;
	COM_StripExtension( bspName, name, sizeof( name ) );
	Q_strcat( name, sizeof( name ), ".tlampocc" );
	len = ri.FS_ReadFile( name, (void **)&buf );
	if ( !buf )
		return;
	if ( len != 32 || memcmp( buf, "TLOC", 4 ) || buf[1] != 3 || (int)buf[2] != tw->numLights || buf[3] != 64 || buf[2] > 4095
		|| buf[5] != buf[4] || (int)buf[6] != (int)buf[4] + 2 * ( ( (int)buf[2] + tw->shadowImage->width / 4 - 1 ) / ( tw->shadowImage->width / 4 ) )
		|| (int)buf[7] != tw->shadowImage->width / 64
		|| tw->shadowImage->height < (int)( buf[6] + ( ( buf[2] + buf[7] - 1 ) / buf[7] ) * 64 ) ) {
		ri.Printf( PRINT_WARNING, "R_LoadTWorldLampOcclusion: %s does not match the lamps or the shadow texture\n", name );
		ri.FS_FreeFile( buf );
		return;
	}
	tw->lampSunHeight = (int)buf[4];
	tw->lampTileRow = (int)buf[6];
	ri.FS_FreeFile( buf );

	tw->instLamps = (float (*)[4])ri.Hunk_Alloc( sizeof( float ) * 4 * ( tw->numInstances + 1 ), h_low );
	for ( i = 0; i < tw->numInstances; i++ ) {
		const tworldInst_t *in = &tw->instances[i];
		int		best[8];
		float	bestD[8];
		int		nb = 0, q;
		for ( k = 0; k < tw->numLights; k++ ) {
			const float *L = tw->lights + k * 8;
			const float gap = Distance( L, in->center ) - L[3] - in->radius;
			if ( gap >= 0.0f )
				continue;
			if ( nb == 8 ) {
				tooMany++;
				if ( gap >= bestD[7] )
					continue;
				nb = 7;
			}
			for ( q = nb; q > 0 && bestD[q - 1] > gap; q-- ) {
				best[q] = best[q - 1]; bestD[q] = bestD[q - 1];
			}
			best[q] = k; bestD[q] = gap; nb++;
		}
		for ( q = 0; q < 4; q++ ) {
			const int a = ( 2 * q < nb ) ? best[2 * q] + 1 : 0, b = ( 2 * q + 1 < nb ) ? best[2 * q + 1] + 1 : 0;
			tw->instLamps[i][q] = (float)( a + b * 4096 );		// two 12-bit values: exact in a float
		}
		if ( nb )
			lit++;
	}
	ri.Printf( PRINT_ALL, "R_LoadTWorldLampOcclusion: %s: %i lamps at any distance, %i placements lit (%i lamp lists capped at 8)\n",
		name, tw->numLights, lit, tooMany );
}

/*
=================
R_LoadTWorldPBR

Optional maps/<name>.tpbr: "TPBR", version 1, material count, then one twPbrMaterial_t per material of the
world file. Without it the physically based path still works, with default roughness and no normal maps.
=================
*/
static void R_LoadTWorldPBR( tworld_t *tw, const char *bspName )
{
	char	name[MAX_QPATH];
	byte	*buffer = NULL;
	long	len;
	int		i, n, nt = 0, version;
	size_t	stride;

	tw->pbrImages = (image_t **)ri.Hunk_Alloc( sizeof( image_t * ) * ( tw->numTints + 1 ), h_low );
	tw->pbrScales = (float *)ri.Hunk_Alloc( sizeof( float ) * ( tw->numTints + 1 ), h_low );
	tw->pbrPacks = (float *)ri.Hunk_Alloc( sizeof( float ) * ( tw->numTints + 1 ), h_low );
	tw->tileImages = (image_t **)ri.Hunk_Alloc( sizeof( image_t * ) * ( tw->numTints + 1 ), h_low );
	tw->tilePacks = (float *)ri.Hunk_Alloc( sizeof( float ) * ( tw->numTints + 1 ), h_low );

	COM_StripExtension( bspName, name, sizeof( name ) );
	Q_strcat( name, sizeof( name ), ".tpbr" );
	len = ri.FS_ReadFile( name, (void **)&buffer );
	if ( len <= 0 || !buffer )
		return;
	version = ( len >= 12 ) ? (int)( (const uint32_t *)buffer )[1] : 0;
	stride = ( version == 2 ) ? sizeof( twPbrMaterial2_t ) : sizeof( twPbrMaterial_t );
	if ( len < 12 || memcmp( buffer, "TPBR", 4 ) || ( version != 1 && version != 2 )
		|| ( (const uint32_t *)buffer )[2] != (uint32_t)tw->numTints
		|| (size_t)len != 12 + (size_t)tw->numTints * stride ) {
		ri.Printf( PRINT_WARNING, "R_LoadTWorldPBR: %s does not match the world file\n", name );
		ri.FS_FreeFile( buffer );
		return;
	}
	n = 0;
	for ( i = 0; i < tw->numTints; i++ ) {
		char img[MAX_QPATH];
		const twPbrMaterial_t *pmi = (const twPbrMaterial_t *)( buffer + 12 + (size_t)i * stride );
		const int r = (int)( Com_Clamp( 0.0f, 1.0f, pmi->roughness ) * 255.0f + 0.5f );
		const int m = (int)( Com_Clamp( 0.0f, 1.0f, pmi->metallic ) * 255.0f + 0.5f );
		tw->pbrPacks[i] = (float)( ( r < 1 ? 1 : r ) + 256 * m + 65536 * (int)( pmi->flags & 15 ) );
		tw->pbrScales[i] = ( pmi->scale > 0.0f ) ? pmi->scale : 1.0f;
		if ( version == 2 ) {
			const twPbrMaterial2_t *p2 = (const twPbrMaterial2_t *)pmi;
			const int tu = (int)( Com_Clamp( 0.0f, 255.0f, p2->tileU ) + 0.5f ), tv = (int)( Com_Clamp( 0.0f, 255.0f, p2->tileV ) + 0.5f );
			Q_strncpyz( img, p2->tile, sizeof( img ) );
			if ( img[0] && tu > 0 && tv > 0 && vk.maxBoundDescriptorSets >= 5 ) {
				image_t *im = R_FindImageFile( img, (imgFlags_t)( IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE ) );
				if ( im && ( im->flags & IMGFLAG_NOSCALE ) ) {
					tw->tileImages[i] = im;
					tw->tilePacks[i] = (float)( 2 * tu + 1024 * tv );
					nt++;
				}
			}
		}
		Q_strncpyz( img, pmi->image, sizeof( img ) );
		if ( img[0] ) {
			image_t *im = R_FindImageFile( img, (imgFlags_t)( IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE ) );
			// only data that reached the GPU unmodified is usable as a normal map: a pre-compressed .dds
			if ( im && ( im->flags & IMGFLAG_NOSCALE ) ) {
				tw->pbrImages[i] = im;
				n++;
			}
		}
	}
	tw->numPbrMaps = n;
	ri.Printf( PRINT_ALL, "R_LoadTWorldPBR: %s: %i materials, %i with a normal map, %i with a tiling colour layer\n", name, tw->numTints, n, nt );
	ri.FS_FreeFile( buffer );
}

#define TW_ERR( msg ) { ri.Printf( PRINT_WARNING, "R_LoadTWorld: %s: " msg "\n", name ); ri.FS_FreeFile( buffer ); return; }

/*
=================
R_LoadTWorld

Called once per map after the BSP has loaded. bspName is "maps/<name>.bsp".
=================
*/
void R_LoadTWorld( const char *bspName )
{
	char			name[MAX_QPATH];
	byte			*buffer = NULL;
	long			len;
	const twHeader_t	*h;
	const twMaterial_t	*fmat;
	const twMesh_t		*fmesh;
	const twSubmesh_t	*fsub;
	const twInstance_t	*finst;
	const twVertex_t	*fvert;
	const float			**uv2Table = NULL;
	byte				*aoBuf = NULL;
	const byte			*aoVerts = NULL;		// one byte per vertex of the file, or NULL
	uint32_t			numInstFile = 0;
	byte				*uv2Buf = NULL;
	const uint32_t		*findex;
	shader_t		**shaders;
	tworld_t		*tw;
	uint32_t		i, j, l;
	size_t			need;
	int				startTime;

	s_tworld = NULL;

	r_tworld = ri.Cvar_Get( "r_tworld", "1", CVAR_ARCHIVE_ND, "Draw the cooked static world (maps/<name>.tworld) when a map has one" );
	r_tworldStats = ri.Cvar_Get( "r_tworldStats", "0", CVAR_TEMP, "Print cooked-world draw statistics once per second" );
	// sizes are bounding-sphere radius divided by distance (roughly 1/1000 per pixel of radius at 1080p)
	r_tworldCullSize = ri.Cvar_Get( "r_tworldCullSize", "0.004", CVAR_ARCHIVE_ND, "Cooked world: skip placements smaller than this (radius / distance)" );
	r_tworldLodSize1 = ri.Cvar_Get( "r_tworldLodSize1", "0.12", CVAR_ARCHIVE_ND, "Cooked world: use detail level 1 below this size (radius / distance)" );
	r_tworldLodSize2 = ri.Cvar_Get( "r_tworldLodSize2", "0.035", CVAR_ARCHIVE_ND, "Cooked world: use detail level 2 below this size (radius / distance)" );
	r_tworldLight = ri.Cvar_Get( "r_tworldLight", "1", CVAR_ARCHIVE_ND, "Cooked world: brightness scale of the sun and ambient light" );

	r_tworldShadows = ri.Cvar_Get( "r_tworldShadows", "1", CVAR_ARCHIVE_ND, "Cooked world: baked sun shadows, 0 = off, up to 1 = sun fully removed in shadow" );
	r_tworldShadowOffset = ri.Cvar_Get( "r_tworldShadowOffset", "2.0", CVAR_TEMP, "Cooked world shadows: receiver offset along its normal, in shadow map texels" );
	r_tworldShadowBias = ri.Cvar_Get( "r_tworldShadowBias", "1.5", CVAR_TEMP, "Cooked world shadows: depth bias, in shadow map texels" );
	r_tworldWaterBump = ri.Cvar_Get( "r_tworldWaterBump", "0.015", CVAR_ARCHIVE_ND, "Cooked world water: slope of the ripples (0 = mirror)" );
	r_tworldPBR = ri.Cvar_Get( "r_tworldPBR", "1", CVAR_ARCHIVE_ND, "Cooked world: 1 = physically based shading (normal maps, roughness, metallic), 0 = classic shading" );
	r_tworldFoliageLight = ri.Cvar_Get( "r_tworldFoliageLight", "0.6", CVAR_ARCHIVE_ND, "Cooked world: share of the ambient light that reaches foliage (1 = lit like open surfaces)" );
	r_tworldHaze = ri.Cvar_Get( "r_tworldHaze", "110000", CVAR_ARCHIVE_ND, "Cooked world: distance in world units at which haze has taken half of the view (0 = no haze)" );
	r_tworldBench = ri.Cvar_Get( "r_tworldBench", "0", CVAR_TEMP, "Cooked world: benchmark section label; every change prints the frame-time figures of the section that ended (0 = off)" );
	r_tworldPortal = ri.Cvar_Get( "r_tworldPortal", "1", CVAR_ARCHIVE_ND, "Cooked world: inside an interior, draw the outside only through its openings (0 = draw everything)" );
	r_tworldAO = ri.Cvar_Get( "r_tworldAO", "0.6", CVAR_ARCHIVE_ND, "Cooked world: strength of the baked ambient occlusion of each mesh (0 = off)" );
	r_tworldIndoor = ri.Cvar_Get( "r_tworldIndoor", "0.55", CVAR_ARCHIVE_ND, "Cooked world: share of the ambient light inside fully enclosed spaces (1 = no darkening)" );
	r_tworldLamps = ri.Cvar_Get( "r_tworldLamps", "1", CVAR_ARCHIVE_ND, "Cooked world: brightness of the local lights (lamps), 0 = off" );
	r_tworldLampDistance = ri.Cvar_Get( "r_tworldLampDistance", "4000", CVAR_ARCHIVE_ND, "Cooked world: lamp light is drawn up to this distance from the camera (world units), fading over the last quarter; 0 = no limit" );
	r_tworldLampDebug = ri.Cvar_Get( "r_tworldLampDebug", "0", CVAR_TEMP, "Cooked world: lamp debug view, 1 = no occlusion test, 2 = placements with lamps in red" );
	r_tworldInstancing = ri.Cvar_Get( "r_tworldInstancing", "1", CVAR_ARCHIVE_ND, "Cooked world: 1 = one instanced draw per mesh level and material, 0 = one draw per placement through the model path" );
	r_tworldThreads = ri.Cvar_Get( "r_tworldThreads", "-1", CVAR_ARCHIVE_ND, "Cooked world: worker threads for the per-frame culling and instance data, besides the main thread; -1 = automatic (hardware threads - 1, at most 7), 0 = single-threaded" );
	r_tworldThreadsCheck = ri.Cvar_Get( "r_tworldThreadsCheck", "0", CVAR_TEMP, "Cooked world debug: every frame, compare the threaded culling and instance data with the one-thread path; r_tworldStats prints the result" );

	COM_StripExtension( bspName, name, sizeof( name ) );
	Q_strcat( name, sizeof( name ), ".tworld" );

	if ( !vk.vboMdvActive ) {
		// the cooked world is drawn through the model VBO path. Say so instead of showing an empty map, and turn the
		// (latched, saved) settings on so that the next start of the game or vid_restart draws it
		if ( ri.FS_ReadFile( name, NULL ) > 0 ) {
			ri.Printf( PRINT_WARNING, "WARNING: %s is not drawn: it needs r_vbo 1 and r_vbo_models 1. Both are set now; restart the game or type vid_restart.\n", name );
			ri.Cvar_Set( "r_vbo", "1" );
			ri.Cvar_Set( "r_vbo_models", "1" );
		}
		return;
	}

	len = ri.FS_ReadFile( name, (void **)&buffer );
	if ( len <= 0 || !buffer )
		return;		// most maps have none

	startTime = ri.Milliseconds();

	if ( len < (long)sizeof( twHeader_t ) )
		TW_ERR( "file too short" );

	h = (const twHeader_t *)buffer;
	if ( memcmp( h->magic, TWORLD_MAGIC, 4 ) || h->version != TWORLD_VERSION )
		TW_ERR( "wrong magic or version (this build reads version 2)" );

	need = sizeof( twHeader_t )
		+ (size_t)h->numMaterials * sizeof( twMaterial_t )
		+ (size_t)h->numMeshes * sizeof( twMesh_t )
		+ (size_t)h->numSubmeshes * sizeof( twSubmesh_t )
		+ (size_t)h->numInstances * sizeof( twInstance_t )
		+ (size_t)h->numVerts * sizeof( twVertex_t )
		+ (size_t)h->numIndexes * sizeof( uint32_t );
	if ( need != (size_t)len )
		TW_ERR( "size does not match the header counts" );

	fmat	= (const twMaterial_t *)( h + 1 );
	fmesh	= (const twMesh_t *)( fmat + h->numMaterials );
	fsub	= (const twSubmesh_t *)( fmesh + h->numMeshes );
	finst	= (const twInstance_t *)( fsub + h->numSubmeshes );
	fvert	= (const twVertex_t *)( finst + h->numInstances );
	findex	= (const uint32_t *)( fvert + h->numVerts );

	// validate every range before touching the GPU
	for ( i = 0; i < h->numMeshes; i++ ) {
		const twMesh_t *m = &fmesh[i];
		if ( (uint64_t)m->firstVert + m->numVerts > h->numVerts || (uint64_t)m->firstIndex + m->numIndexes > h->numIndexes
			|| m->numVerts == 0 || m->numLods < 1 || m->numLods > TWORLD_MAX_LODS )
			TW_ERR( "mesh range out of bounds" );
		for ( l = 0; l < m->numLods; l++ ) {
			if ( (uint64_t)m->lodFirstSub[l] + m->lodNumSubs[l] > h->numSubmeshes )
				TW_ERR( "mesh detail level out of bounds" );
			for ( j = 0; j < m->lodNumSubs[l]; j++ ) {
				const twSubmesh_t *s = &fsub[m->lodFirstSub[l] + j];
				if ( s->material >= h->numMaterials || (uint64_t)s->firstIndex + s->numIndexes > m->numIndexes )
					TW_ERR( "submesh range out of bounds" );
			}
		}
		for ( j = 0; j < m->numIndexes; j++ ) {
			if ( findex[m->firstIndex + j] >= m->numVerts )
				TW_ERR( "vertex index out of bounds" );
		}
	}
	for ( i = 0; i < h->numInstances; i++ ) {
		if ( finst[i].mesh >= h->numMeshes )
			TW_ERR( "instance mesh out of bounds" );
	}

	tw = (tworld_t *)ri.Hunk_Alloc( sizeof( *tw ), h_low );
	tw->surfaceType = SF_TWORLD;
	tw->numMeshes = h->numMeshes;
	tw->numSubs = h->numSubmeshes;
	tw->numInstances = h->numInstances;
	tw->numVerts = h->numVerts;
	tw->numIndexes = h->numIndexes;
	VectorCopy( h->mins, tw->mins );
	VectorCopy( h->maxs, tw->maxs );
	VectorCopy( h->sunDir, tw->sunDir );
	VectorNormalize( tw->sunDir );
	VectorCopy( h->sunColor, tw->sunColor );
	VectorCopy( h->ambientColor, tw->ambientColor );
	tw->metresPerUnit = ( h->unitsPerMetre > 0.001f ) ? 1.0f / h->unitsPerMetre : 0.0254f;
	Com_Memcpy( tw->originM, &h->reserved[2], sizeof( tw->originM ) );		// two floats; zero in older files
	if ( h->reserved[0] || h->reserved[1] ) {
		for ( i = 0; i < 3; i++ ) {
			tw->skyZenith[i] = ( ( h->reserved[0] >> ( 16 - 8 * i ) ) & 255 ) / 255.0f;
			tw->skyHorizon[i] = ( ( h->reserved[1] >> ( 16 - 8 * i ) ) & 255 ) / 255.0f;
		}
	} else {
		VectorSet( tw->skyZenith, 0.35f, 0.50f, 0.80f );
		VectorSet( tw->skyHorizon, 0.75f, 0.80f, 0.85f );
	}
	tw->meshes = (tworldMesh_t *)ri.Hunk_Alloc( sizeof( tworldMesh_t ) * h->numMeshes, h_low );
	tw->subs = (tworldSub_t *)ri.Hunk_Alloc( sizeof( tworldSub_t ) * ( h->numSubmeshes + 1 ), h_low );
	tw->instances = (tworldInst_t *)ri.Hunk_Alloc( sizeof( tworldInst_t ) * ( h->numInstances + 1 ), h_low );
	tw->numTints = h->numMaterials;
	tw->tints = (vec3_t *)ri.Hunk_Alloc( sizeof( vec3_t ) * ( h->numMaterials + 1 ), h_low );

	// materials are ordinary shaders (scripts generated by the cooker) plus a constant tint
	shaders = (shader_t **)ri.Hunk_AllocateTempMemory( sizeof( shader_t * ) * ( h->numMaterials + 1 ) );
	for ( i = 0; i < h->numMaterials; i++ ) {
		char sname[MAX_QPATH];
		Q_strncpyz( sname, fmat[i].shader, sizeof( sname ) );
		shaders[i] = R_FindShader( sname, lightmapsNone, stylesDefault, qtrue );
		if ( !shaders[i] )
			shaders[i] = tr.defaultShader;
		VectorCopy( fmat[i].tint, tw->tints[i] );
		if ( ( fmat[i].flags & TWMAT_BLEND ) && !tw->blendSortShader && shaders[i] != tr.defaultShader )
			tw->blendSortShader = shaders[i];
	}
	tw->alphas = (float *)ri.Hunk_Alloc( sizeof( float ) * ( h->numMaterials + 1 ), h_low );
	for ( i = 0; i < h->numMaterials; i++ ) {
		const uint32_t a = ( fmat[i].flags >> 8 ) & 255;
		tw->alphas[i] = a ? a / 255.0f : 1.0f;
	}
	tw->details = (float *)ri.Hunk_Alloc( sizeof( float ) * ( h->numMaterials + 1 ), h_low );
	tw->anims = (byte *)ri.Hunk_Alloc( h->numMaterials + 1, h_low );
	for ( i = 0; i < h->numMaterials; i++ ) {
		tw->details[i] = (float)( ( fmat[i].flags >> 16 ) & 0xffff );
		tw->anims[i] = (byte)( ( fmat[i].flags >> 1 ) & 7 );
	}

	// the far plane and the cull distance must cover the cooked world, whatever the BSP's worldspawn says
	{
		vec3_t size;
		float need;
		VectorSubtract( h->maxs, h->mins, size );
		need = VectorLength( size ) / 1.732f;
		if ( need < 8.0e6f && tr.distanceCull < need )
			tr.distanceCull = need;
	}
	tw->visInst = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numInstances + 1 ), h_low );
	tw->visLod = (byte *)ri.Hunk_Alloc( h->numInstances + 1, h_low );
	tw->visAllInst = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numInstances + 1 ), h_low );
	tw->visAllLod = (byte *)ri.Hunk_Alloc( h->numInstances + 1, h_low );
	tw->visKey = (uint32_t *)ri.Hunk_Alloc( sizeof( uint32_t ) * ( h->numInstances + 1 ), h_low );
	tw->sortInst = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numInstances + 1 ), h_low );
	tw->visAllMesh = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numInstances + 1 ), h_low );
	tw->sortMesh = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numInstances + 1 ), h_low );
	tw->sortLod = (byte *)ri.Hunk_Alloc( h->numInstances + 1, h_low );
	tw->drawList = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numSubmeshes + 1 ), h_low );
	tw->batchCount = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numSubmeshes + 1 ) * 3, h_low );
	tw->batchStart = tw->batchCount + h->numSubmeshes + 1;
	tw->batchCursor = tw->batchStart + h->numSubmeshes + 1;
	tw->subOrder = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numSubmeshes + 1 ), h_low );
	tw->fillCount = (int *)ri.Hunk_Alloc( sizeof( int ) * ( h->numSubmeshes + 1 ) * TW_FILL_JOBS, h_low );
	tw->blendSurfaceType = SF_TWORLD;

	// optional second UV set (maps/<name>.tuv2): "TUV2", version 1, count, then per entry the mesh index, its vertex
	// count and two floats per vertex. Meshes whose colour texture uses another UV set than their normal map.
	{
		char uname[MAX_QPATH];
		long ulen;
		COM_StripExtension( bspName, uname, sizeof( uname ) );
		Q_strcat( uname, sizeof( uname ), ".tuv2" );
		uv2Table = (const float **)ri.Hunk_AllocateTempMemory( sizeof( float * ) * ( h->numMeshes + 1 ) );
		Com_Memset( uv2Table, 0, sizeof( float * ) * ( h->numMeshes + 1 ) );
		ulen = ri.FS_ReadFile( uname, (void **)&uv2Buf );
		if ( ulen >= 12 && uv2Buf && !memcmp( uv2Buf, "TUV2", 4 ) && ( (const uint32_t *)uv2Buf )[1] == 1 ) {
			const uint32_t n = ( (const uint32_t *)uv2Buf )[2];
			size_t o = 12;
			uint32_t e, used = 0;
			for ( e = 0; e < n && o + 8 <= (size_t)ulen; e++ ) {
				const uint32_t mi = *(const uint32_t *)( uv2Buf + o ), nv = *(const uint32_t *)( uv2Buf + o + 4 );
				o += 8;
				if ( o + (size_t)nv * 8 > (size_t)ulen )
					break;
				if ( mi < h->numMeshes && nv == fmesh[mi].numVerts ) {
					uv2Table[mi] = (const float *)( uv2Buf + o );
					used++;
				}
				o += (size_t)nv * 8;
			}
			ri.Printf( PRINT_ALL, "R_LoadTWorld: %s: second UV set for %u meshes\n", uname, used );
		}
	}

	{
		// optional ambient occlusion per vertex: "TAO1", version 1, vertex count, then one byte per vertex
		char aname[MAX_QPATH];
		long alen;
		COM_StripExtension( bspName, aname, sizeof( aname ) );
		Q_strcat( aname, sizeof( aname ), ".tao" );
		alen = ri.FS_ReadFile( aname, (void **)&aoBuf );
		if ( alen >= 12 && aoBuf && !memcmp( aoBuf, "TAO1", 4 ) && ( (const uint32_t *)aoBuf )[1] == 1
			&& ( (const uint32_t *)aoBuf )[2] == h->numVerts && (size_t)alen == 12 + (size_t)h->numVerts ) {
			aoVerts = aoBuf + 12;
			ri.Printf( PRINT_ALL, "R_LoadTWorld: %s: ambient occlusion for %u vertices\n", aname, h->numVerts );
		}
	}

	// one vertex buffer and one index buffer per mesh (all detail levels), in the MD3 VBO layout
	for ( i = 0; i < h->numMeshes; i++ ) {
		const twMesh_t *m = &fmesh[i];
		const int stride = sizeof( vec4_t ) + sizeof( vec4_t ) + sizeof( vec2_t ) + sizeof( vec4_t );
		const int dataSize = stride * m->numVerts;
		byte *data = (byte *)Z_Malloc( dataSize, TAG_TEMP_WORKSPACE, qtrue );
		byte *p = data;
		char bufName[MAX_QPATH];
		VBO_t *vbo;
		IBO_t *ibo;

		tw->meshes[i].zmin = tw->meshes[i].zmax = fvert[m->firstVert].xyz[2];
		for ( j = 0; j < m->numVerts; j++, p += stride ) {
			const twVertex_t *v = &fvert[m->firstVert + j];
			float *out = (float *)p;
			if ( v->xyz[2] < tw->meshes[i].zmin ) tw->meshes[i].zmin = v->xyz[2];
			if ( v->xyz[2] > tw->meshes[i].zmax ) tw->meshes[i].zmax = v->xyz[2];
			out[0] = v->xyz[0]; out[1] = v->xyz[1]; out[2] = v->xyz[2];
			out[3] = aoVerts ? aoVerts[m->firstVert + j] * ( 1.0f / 255.0f ) : 1.0f;
			out[4] = v->normal[0]; out[5] = v->normal[1]; out[6] = v->normal[2]; out[7] = 0.0f;
			out[8] = v->st[0]; out[9] = v->st[1];
			if ( uv2Table[i] ) {
				Com_Memcpy( out + 10, uv2Table[i] + j * 2, 8 );		// the file is not aligned
			} else {
				out[10] = v->st[0]; out[11] = v->st[1];
			}
		}

		Com_sprintf( bufName, sizeof( bufName ), "tworld_%u", i );
		vbo = R_CreateVBO( bufName, data, dataSize );
		ibo = R_CreateIBO( bufName, (const byte *)( findex + m->firstIndex ), sizeof( uint32_t ) * m->numIndexes );
		Z_Free( data );

		vbo->offsets[0] = 0;
		vbo->offsets[5] = sizeof( vec4_t );
		vbo->offsets[2] = sizeof( vec4_t ) * 2;
		vbo->offsets[8] = sizeof( vec4_t ) * 2 + sizeof( vec2_t );

		tw->meshes[i].numLods = m->numLods;
		for ( l = 0; l < m->numLods; l++ ) {
			tw->meshes[i].lodFirstSub[l] = m->lodFirstSub[l];
			tw->meshes[i].lodNumSubs[l] = m->lodNumSubs[l];

			for ( j = 0; j < m->lodNumSubs[l]; j++ ) {
				const twSubmesh_t *s = &fsub[m->lodFirstSub[l] + j];
				tworldSub_t *out = &tw->subs[m->lodFirstSub[l] + j];

				out->shader = shaders[s->material];
				out->tintIndex = s->material;
				out->pass = ( fmat[s->material].flags & TWMAT_BLEND ) ? 1 : 0;
				{
					const shaderStage_t *st0 = out->shader->stages[0];
					const image_t *im0 = st0 ? st0->bundle[0].image[0] : NULL;
					out->texNoScale = ( im0 && ( im0->flags & IMGFLAG_NOLIGHTSCALE ) ) ? 1.0f : 0.0f;
				}
				tw->meshes[i].lodPassMask[l] |= 1 << out->pass;
				out->surf.surfaceType = SF_VBO_MDVMESH;
				out->surf.vbo = vbo;
				out->surf.ibo = ibo;
				out->surf.indexOffset = s->firstIndex;
				out->surf.numIndexes = s->numIndexes;
				out->surf.numVerts = m->numVerts;
				out->surf.minIndex = 0;
				out->surf.maxIndex = m->numVerts - 1;
			}
		}
	}
	ri.Hunk_FreeTempMemory( shaders );

	for ( i = 0; i < h->numSubmeshes; i++ )
		tw->subOrder[i] = i;
	s_sortSubs = tw->subs;
	qsort( tw->subOrder, h->numSubmeshes, sizeof( int ), TW_CompareSubs );

	for ( i = 0; i < h->numInstances; i++ ) {
		const twInstance_t *in = &finst[i];
		tworldInst_t *out = &tw->instances[i];
		float *mm = out->matrix;

		out->mesh = in->mesh;
		mm[0] = in->m[0];  mm[1] = in->m[4];  mm[2] = in->m[8];   mm[3] = 0.0f;
		mm[4] = in->m[1];  mm[5] = in->m[5];  mm[6] = in->m[9];   mm[7] = 0.0f;
		mm[8] = in->m[2];  mm[9] = in->m[6];  mm[10] = in->m[10]; mm[11] = 0.0f;
		mm[12] = in->m[3]; mm[13] = in->m[7]; mm[14] = in->m[11]; mm[15] = 1.0f;
		VectorCopy( in->center, out->center );
		out->radius = in->radius;
	}

	ri.Printf( PRINT_ALL, "R_LoadTWorld: %s: %u meshes, %u submeshes, %u instances, %u materials, %u verts, %u tris (all detail levels), %.1f MB file, %i ms\n",
		name, h->numMeshes, h->numSubmeshes, h->numInstances, h->numMaterials, h->numVerts, h->numIndexes / 3,
		len / ( 1024.0f * 1024.0f ), ri.Milliseconds() - startTime );

	numInstFile = h->numInstances;		// the header lives in the file buffer, which is released next
	ri.FS_FreeFile( buffer );
	if ( aoBuf )
		ri.FS_FreeFile( aoBuf );
	if ( uv2Buf )
		ri.FS_FreeFile( uv2Buf );
	ri.Hunk_FreeTempMemory( uv2Table );
	{
		// optional sky occlusion: "TSKY", version 1, placement count, then one byte per placement
		char kname[MAX_QPATH];
		byte *kbuf = NULL;
		long klen;
		COM_StripExtension( bspName, kname, sizeof( kname ) );
		Q_strcat( kname, sizeof( kname ), ".tsky" );
		klen = ri.FS_ReadFile( kname, (void **)&kbuf );
		if ( klen >= 12 && kbuf && !memcmp( kbuf, "TSKY", 4 ) && ( (const uint32_t *)kbuf )[1] == 1
			&& ( (const uint32_t *)kbuf )[2] == numInstFile && (size_t)klen == 12 + (size_t)numInstFile ) {
			tw->skyOcc = (byte *)ri.Hunk_Alloc( numInstFile + 1, h_low );
			Com_Memcpy( tw->skyOcc, kbuf + 12, numInstFile );
			ri.Printf( PRINT_ALL, "R_LoadTWorld: %s: sky occlusion for %u placements\n", kname, numInstFile );
		}
		if ( kbuf )
			ri.FS_FreeFile( kbuf );
	}
	TW_BuildCells( tw );
	{
		// optional interior visibility: "TPRT", version 1, count, six floats (box), then six floats per opening
		char pname[MAX_QPATH];
		byte *pbuf = NULL;
		long plen;
		COM_StripExtension( bspName, pname, sizeof( pname ) );
		Q_strcat( pname, sizeof( pname ), ".tportal" );
		plen = ri.FS_ReadFile( pname, (void **)&pbuf );
		if ( plen >= 36 && pbuf && !memcmp( pbuf, "TPRT", 4 ) && ( (const uint32_t *)pbuf )[1] == 1 ) {
			const uint32_t n = ( (const uint32_t *)pbuf )[2];
			if ( n <= 256 && (size_t)plen == 36 + (size_t)n * 24 ) {
				Com_Memcpy( tw->portalMins, pbuf + 12, 12 );
				Com_Memcpy( tw->portalMaxs, pbuf + 24, 12 );
				Com_Memcpy( tw->portals, pbuf + 36, n * 24 );
				tw->numPortals = (int)n;
				tw->hasPortal = qtrue;
				ri.Printf( PRINT_ALL, "R_LoadTWorld: %s: interior box with %i openings\n", pname, tw->numPortals );
			}
		}
		if ( pbuf )
			ri.FS_FreeFile( pbuf );
	}
	{
		// optional local lights: "TLGT", version 1, count, then 8 floats per light
		char lname[MAX_QPATH];
		byte *lbuf = NULL;
		long llen;
		COM_StripExtension( bspName, lname, sizeof( lname ) );
		Q_strcat( lname, sizeof( lname ), ".tlights" );
		llen = ri.FS_ReadFile( lname, (void **)&lbuf );
		if ( llen >= 12 && lbuf ) {
			const uint32_t n = ( (const uint32_t *)lbuf )[2];
			if ( !memcmp( lbuf, "TLGT", 4 ) && ( (const uint32_t *)lbuf )[1] == 1 && n <= 4096 && (size_t)llen == 12 + (size_t)n * 32 ) {
				tw->lights = (float *)ri.Hunk_Alloc( (int)( n * 32 + 4 ), h_low );
				Com_Memcpy( tw->lights, lbuf + 12, n * 32 );
				tw->numLights = (int)n;
				ri.Printf( PRINT_ALL, "R_LoadTWorld: %s: %i local lights\n", lname, tw->numLights );
			}
		}
		if ( lbuf )
			ri.FS_FreeFile( lbuf );
	}
	R_LoadTWorldShadow( tw, bspName );
	R_LoadTWorldLampOcclusion( tw, bspName );
	R_LoadTWorldPBR( tw, bspName );
	s_tworld = tw;
}

/*
=================
R_TWorldEntityLight

Entity lighting on a map whose BSP has no light grid: use the cooked world's sun and ambient colour, so
players and other models are shaded like the geometry around them (same response curve in both shaders).
With a baked sun shadow map the sun part is scaled by how much of the entity sees the sun (five lookups
around its origin, so the change at a shadow edge is gradual).
=================
*/
qboolean R_TWorldEntityLight( trRefEntity_t *ent )
{
	float scale;

	if ( !s_tworld || !r_tworld || !r_tworld->integer )
		return qfalse;

	scale = 255.0f * tr.identityLight * r_tworldLight->value;
	VectorScale( s_tworld->ambientColor, scale, ent->ambientLight );
	VectorScale( s_tworld->sunColor, scale, ent->directedLight );
	VectorCopy( s_tworld->sunDir, ent->lightDir );

	if ( s_tworld->shadowImage && r_tworldShadows->value > 0.0f ) {
		static const float ofs[5][3] = { { 0, 0, 24 }, { 20, 0, 8 }, { -20, 0, 8 }, { 0, 20, 40 }, { 0, -20, 40 } };
		const float strength = ( r_tworldShadows->value > 1.0f ) ? 1.0f : r_tworldShadows->value;
		float vis = 0.0f;
		vec3_t p;
		int i;
		for ( i = 0; i < 5; i++ ) {
			VectorAdd( ent->e.origin, ofs[i], p );
			vis += TW_SunVisibility( s_tworld, p );
		}
		VectorScale( ent->directedLight, 1.0f - strength * ( 1.0f - vis * 0.2f ), ent->directedLight );
	}
	return qtrue;
}

/*
=================
R_AddTWorldSurface

Front end: one draw surface for the whole cooked world, and its bounds added
to the visible bounds so the far plane covers it.
=================
*/
void R_AddTWorldSurface( void )
{
	if ( !s_tworld || !r_tworld || !r_tworld->integer )
		return;

	AddPointToBounds( s_tworld->mins, tr.viewParms.visBounds[0], tr.viewParms.visBounds[1] );
	AddPointToBounds( s_tworld->maxs, tr.viewParms.visBounds[0], tr.viewParms.visBounds[1] );

	R_AddDrawSurf( &s_tworld->surfaceType, tr.defaultShader, 0, 0 );

	if ( s_tworld->blendSortShader )
		R_AddDrawSurf( &s_tworld->blendSurfaceType, s_tworld->blendSortShader, 0, 0 );
}

/*
=================
RB_TWorldInstanced

Instanced path: cull and pick detail levels as usual, then group the visible
placements by (mesh level, material) range, write one 64-byte record per
placement into the per-frame buffer and issue one instanced draw per group.
Returns qfalse only when the path is not available.
=================
*/
typedef struct {
	float	row0[4], row1[4], row2[4];
	float	tint[4];
	float	params[4];		// x: detail repeats per unit of UV
	float	lamps[4];		// lamps that reach the placement: two indices + 1 per value (a + 4096 * b), 0 = none
} twInstData_t;				// 96 bytes (vk_pipelines.cpp: the instance binding's stride)

// r_tworldThreadsCheck: the threaded results compared with the one-thread path every frame (a debug aid; the scratch
// buffers exist only while it is on)
static struct {
	int				*inst, *mesh;
	byte			*lod;
	int				capInst;
	twInstData_t	*data;
	int				capData;
	int				cullViews, cullBad, fillPasses, fillBad;
} s_twCheck;

static qboolean TW_CheckAlloc( const int numInst, const int numRecords ) {
	if ( numInst > s_twCheck.capInst ) {
		if ( s_twCheck.inst ) {
			Z_Free( s_twCheck.inst ); Z_Free( s_twCheck.mesh ); Z_Free( s_twCheck.lod );
		}
		s_twCheck.inst = (int *)Z_Malloc( sizeof( int ) * numInst, TAG_GENERAL, qfalse );
		s_twCheck.mesh = (int *)Z_Malloc( sizeof( int ) * numInst, TAG_GENERAL, qfalse );
		s_twCheck.lod = (byte *)Z_Malloc( numInst, TAG_GENERAL, qfalse );
		s_twCheck.capInst = numInst;
	}
	if ( numRecords > s_twCheck.capData ) {
		if ( s_twCheck.data )
			Z_Free( s_twCheck.data );
		s_twCheck.capData = numRecords + numRecords / 2;
		s_twCheck.data = (twInstData_t *)Z_Malloc( sizeof( twInstData_t ) * s_twCheck.capData, TAG_GENERAL, qfalse );
	}
	return qtrue;
}

static void TW_CheckFree( void ) {
	if ( s_twCheck.inst ) {
		Z_Free( s_twCheck.inst ); Z_Free( s_twCheck.mesh ); Z_Free( s_twCheck.lod );
	}
	if ( s_twCheck.data )
		Z_Free( s_twCheck.data );
	Com_Memset( &s_twCheck, 0, sizeof( s_twCheck ) );
}

// one placement's record for one material range of the instanced path
static inline void TW_WriteInstRecord( const tworld_t *tw, twInstData_t *out, const int instIndex, const tworldMesh_t *mesh, const tworldSub_t *sub, const float *m, const qboolean usePbr )
{
	out->row0[0] = m[0]; out->row0[1] = m[4]; out->row0[2] = m[8];  out->row0[3] = m[12];
	out->row1[0] = m[1]; out->row1[1] = m[5]; out->row1[2] = m[9];  out->row1[3] = m[13];
	out->row2[0] = m[2]; out->row2[1] = m[6]; out->row2[2] = m[10]; out->row2[3] = m[14];
	VectorCopy( tw->tints[sub->tintIndex], out->tint );
	out->tint[3] = tw->alphas[sub->tintIndex];
	out->params[0] = tw->details[sub->tintIndex];
	out->params[1] = sub->texNoScale + tw->tilePacks[sub->tintIndex];
	if ( tw->instLamps ) {
		const float *il = tw->instLamps[instIndex];
		out->lamps[0] = il[0]; out->lamps[1] = il[1]; out->lamps[2] = il[2]; out->lamps[3] = il[3];
	} else {
		out->lamps[0] = out->lamps[1] = out->lamps[2] = out->lamps[3] = 0.0f;
	}
	// animation type, plus the placement's sky occlusion in the fraction (0 .. 0.98)
	out->params[2] = (float)tw->anims[sub->tintIndex] + ( tw->skyOcc ? tw->skyOcc[instIndex] * ( 0.98f / 255.0f ) : 0.0f );
	if ( tw->anims[sub->tintIndex] == 0 || tw->anims[sub->tintIndex] >= 6 ) {
		// physically based path: packed material values; a normal map takes the second texture slot
		out->params[3] = tw->pbrPacks[sub->tintIndex];
		if ( usePbr && out->params[0] <= 0.0f && tw->pbrImages[sub->tintIndex] && ( (int)tw->tilePacks[sub->tintIndex] >> 10 ) != 254 )
			out->params[0] = -tw->pbrScales[sub->tintIndex];
	}
	else if ( tw->anims[sub->tintIndex] == 4 )
		out->params[3] = sub->shader->stages[1] ? 1.0f : 0.0f;		// foam: a second noise texture is present
	else
		out->params[3] = ( tw->anims[sub->tintIndex] == 3 ) ? mesh->zmax : mesh->zmin;
}

// the instance-data job: visible placements job * chunk .. (job + 1) * chunk - 1, written at the cursors counted for this job
typedef struct {
	const tworld_t	*tw;
	twInstData_t	*data;
	int				pass, numVis, chunk, stride;
	qboolean		usePbr;
} twFillCtx_t;

static void TW_FillJob( void *ctx, int job )
{
	const twFillCtx_t	*c = (const twFillCtx_t *)ctx;
	const tworld_t		*tw = c->tw;
	int					*cursor = tw->fillCount + job * c->stride;
	const int			end = ( job + 1 ) * c->chunk < c->numVis ? ( job + 1 ) * c->chunk : c->numVis;
	int					i, j;

	for ( i = job * c->chunk; i < end; i++ ) {
		const tworldInst_t *inst = &tw->instances[tw->visInst[i]];
		const tworldMesh_t *mesh = &tw->meshes[inst->mesh];
		const int lod = tw->visLod[i];
		for ( j = 0; j < mesh->lodNumSubs[lod]; j++ ) {
			const int s = mesh->lodFirstSub[lod] + j;
			const tworldSub_t *sub = &tw->subs[s];
			if ( sub->pass != c->pass || !sub->surf.numIndexes )
				continue;
			TW_WriteInstRecord( tw, &c->data[cursor[s]++], tw->visInst[i], mesh, sub, inst->matrix, c->usePbr );
		}
	}
}

// the view tests of one view (pass 0), shared by every culling job
typedef struct {
	tworld_t		*tw;
	const cplane_t	*frustum;
	const float		*viewOrg;
	const float		**act;		// openings that count for this view
	int				numAct;
	qboolean		cull, inside;
	float			cullSize, kCull, kLod1, kLod2;
} twCullCtx_t;

/*
Tests the placements of cells cellFirst .. cellEnd - 1 against the view and writes the ones that pass, with their
detail level and distance bucket, to sortInst / sortMesh / sortLod / visKey from index outBase on (the first record
index of those cells, so jobs never overlap). Returns how many it wrote; *smallOut counts the ones too small to draw.
*/
static int TW_CullCells( const twCullCtx_t *c, const int cellFirst, const int cellEnd, const int outBase, int *smallOut )
{
	tworld_t		*tw = c->tw;
	const cplane_t	*frustum = c->frustum;
	const float		*viewOrg = c->viewOrg;
	const float		**act = c->act;
	const int		numAct = c->numAct;
	const qboolean	cull = c->cull, inside = c->inside;
	const float		cullSize = c->cullSize, kCull = c->kCull, kLod1 = c->kLod1, kLod2 = c->kLod2;
	int				ci, k, p, n = 0, nSmall = 0;

	for ( ci = cellFirst; ci < cellEnd; ci++ ) {
		const twCell_t *cell = &tw->cells[ci];
		const twCullRec_t *rec = &tw->cullRecs[cell->first];
		qboolean testEach = cull;			// qfalse when the whole cell lies inside the view
		if ( cull ) {
			vec3_t cd;
			float cdist;
			float minSide = 1e30f;
			for ( p = 0; p < 4; p++ ) {
				const float side = DotProduct( cell->center, frustum[p].normal ) - frustum[p].dist;
				if ( side < -cell->radius )
					break;
				if ( side < minSide )
					minSide = side;
			}
			if ( p < 4 )
				continue;
			if ( minSide > cell->radius )
				testEach = qfalse;
			// too far for even the largest placement of the cell to pass the size test
			VectorSubtract( cell->center, viewOrg, cd );
			cdist = VectorLength( cd ) - cell->radius;
			if ( cdist > 1.0f && cell->maxInstRadius < cullSize * cdist ) {
				nSmall += cell->count;
				continue;
			}
		}
		if ( inside && cell->maxInstRadius < 1500.0f ) {
			// a whole cell outside the interior that cannot be seen through any opening
			const float m = cell->radius + 150.0f;
			if ( cell->center[0] < tw->portalMins[0] - m || cell->center[0] > tw->portalMaxs[0] + m
				|| cell->center[1] < tw->portalMins[1] - m || cell->center[1] > tw->portalMaxs[1] + m ) {
				vec3_t cd2;
				int pi;
				VectorSubtract( cell->center, viewOrg, cd2 );
				for ( pi = 0; pi < numAct; pi++ ) {
					const float *po = act[pi];
					const int ax = ( po[0] > 0.5f ) ? 1 : 0, ox = 1 - ax;
					float tt, grow;
					if ( fabsf( cd2[ax] ) < 1e-3f )
						break;				// cannot tell: keep the cell
					tt = ( po[1] - viewOrg[ax] ) / cd2[ax];
					if ( tt <= 0.0f )
						continue;
					if ( tt > 1.0f )
						tt = 1.0f;
					grow = cell->radius * tt + 16.0f;
					if ( fabsf( viewOrg[ox] + cd2[ox] * tt - po[2] ) < po[4] + grow )
						break;
				}
				if ( pi == numAct ) {
					nSmall += cell->count;
					continue;
				}
			}
		}
		for ( k = 0; k < cell->count; k++, rec++ ) {
			vec3_t	d;
			float	d2, t;
			int		lod = 0;

			if ( testEach ) {
				for ( p = 0; p < 4; p++ ) {
					if ( DotProduct( rec->center, frustum[p].normal ) - frustum[p].dist < -rec->radius )
						break;
				}
				if ( p < 4 )
					continue;
			}
			VectorSubtract( rec->center, viewOrg, d );
			// (placements that are themselves enclosed belong to the interior wherever they stand)
			if ( inside && rec->radius < 1500.0f && !( tw->skyOcc && tw->skyOcc[rec->inst] >= 120 ) ) {
				const float m = rec->radius + 150.0f;
				if ( rec->center[0] < tw->portalMins[0] - m || rec->center[0] > tw->portalMaxs[0] + m
					|| rec->center[1] < tw->portalMins[1] - m || rec->center[1] > tw->portalMaxs[1] + m
					|| rec->center[2] < tw->portalMins[2] - m || rec->center[2] > tw->portalMaxs[2] + m ) {
					// outside the interior: visible only through an opening. The line from the eye to the placement
					// must cross the opening's plane inside its rectangle, grown by the placement's radius.
					int pi;
					for ( pi = 0; pi < numAct; pi++ ) {
						const float *po = act[pi];
						const int ax = ( po[0] > 0.5f ) ? 1 : 0, ox = 1 - ax;
						const float den = d[ax];
						float tt, hx, hz, grow;
						if ( fabsf( den ) < 1e-3f )
							continue;
						tt = ( po[1] - viewOrg[ax] ) / den;
						if ( tt <= 0.0f || tt > 1.0f )
							continue;
						grow = rec->radius * tt + 16.0f;
						hx = viewOrg[ox] + d[ox] * tt - po[2];
						hz = viewOrg[2] + d[2] * tt - po[3];
						if ( fabsf( hx ) < po[4] + grow && fabsf( hz ) < po[5] + grow )
							break;
					}
					if ( pi == numAct ) {
						nSmall++;
						continue;
					}
				}
			}
			d2 = DotProduct( d, d );
			t = rec->radius + 1.0f;
			if ( d2 > t * t ) {
				t = rec->radius * kCull;
				if ( kCull > 0.0f && d2 > t * t ) {
					nSmall++;
					continue;
				}
				t = rec->radius * kLod2;
				if ( rec->numLods > 2 && d2 > t * t )
					lod = 2;
				else {
					t = rec->radius * kLod1;
					if ( rec->numLods > 1 && d2 > t * t )
						lod = 1;
				}
			}
			{
				const int at = outBase + n;
				// bucket = exponent of the squared distance to the placement's near side, two buckets per doubling
				float nearD = sqrtf( d2 ) - rec->radius;
				uint32_t bits;
				int b;
				tw->sortInst[at] = rec->inst;
				tw->sortMesh[at] = rec->mesh;
				tw->sortLod[at] = (byte)lod;
				if ( nearD < 1.0f ) nearD = 1.0f;
				Com_Memcpy( &bits, &nearD, 4 );
				b = (int)( bits >> 22 ) - ( 127 << 1 );
				tw->visKey[at] = (uint32_t)( b < 0 ? 0 : ( b > 63 ? 63 : b ) );
			}
			n++;
		}
	}
	*smallOut = nSmall;
	return n;
}

static void TW_CullJob( void *ctx, int job )
{
	const twCullCtx_t *c = (const twCullCtx_t *)ctx;
	tworld_t *tw = c->tw;
	tw->cullJobCount[job] = TW_CullCells( c, tw->cullJobFirst[job], tw->cullJobFirst[job + 1], tw->cullJobBase[job], &tw->cullJobSmall[job] );
}

static qboolean RB_TWorldInstanced( tworld_t *tw, const int pass )
{
	const cplane_t	*frustum = backEnd.viewParms.frustum;
	const qboolean	cull = (qboolean)( !r_nocull->integer );
	const float		cullSize = r_tworldCullSize->value;
	const float		lodSize1 = r_tworldLodSize1->value;
	const float		lodSize2 = r_tworldLodSize2->value;
	const float		light = r_tworldLight->value;
	const float		*viewOrg = backEnd.viewParms.ori.origin;
	const int		mirror = ( backEnd.viewParms.portalView == PV_MIRROR ) ? 1 : 0;
	int				i, j, p, ci, k, numVis = 0, total = 0, run = 0, draws = 0, indexes = 0, numDrawList = 0;
	uint32_t		bufOffset, uboOffset, mainOffset = ~0U;
	const qboolean	usePbr = (qboolean)( r_tworldPBR->integer != 0 );
	twInstData_t	*data;
	vkUniformEntity_t u = {};

	if ( vk.geometry_buffer_size_new )
		return qtrue;		// the per-frame buffer is being replaced; nothing can be drawn from it this frame

	if ( pass == 0 ) {
		tw->c_instances = tw->c_draws = tw->c_indexes = tw->c_small = tw->c_uboFail = 0;
		tw->c_lod[0] = tw->c_lod[1] = tw->c_lod[2] = 0;
	}

	const double tA = TW_Now();
	Com_Memset( tw->batchCount, 0, sizeof( int ) * tw->numSubs );

	if ( pass == 0 ) {
		// view tests, once per view. size = radius / ( distance - radius ) < limit  <=>  distance > radius * ( 1 + limit ) / limit
		const float kCull = ( cullSize > 0.0f ) ? ( 1.0f + cullSize ) / cullSize : 0.0f;
		const float kLod1 = ( lodSize1 > 0.0f ) ? ( 1.0f + lodSize1 ) / lodSize1 : 1e30f;
		const float kLod2 = ( lodSize2 > 0.0f ) ? ( 1.0f + lodSize2 ) / lodSize2 : 1e30f;
		int nAll = 0;
		// openings that count for this view: doors always, windows while they are near (the nearest 24)
		const float	*act[40];
		int		numAct = 0;
		// camera inside the interior box (not for mirror or portal views)
		const qboolean inside = (qboolean)( tw->hasPortal && r_tworldPortal->integer && backEnd.viewParms.portalView == PV_NONE
			&& viewOrg[0] > tw->portalMins[0] && viewOrg[0] < tw->portalMaxs[0] && viewOrg[1] > tw->portalMins[1] && viewOrg[1] < tw->portalMaxs[1]
			&& viewOrg[2] > tw->portalMins[2] && viewOrg[2] < tw->portalMaxs[2] );
		if ( inside ) {
			float	wd[24];
			int		wi[24], nw = 0, q, q2;
			for ( q = 0; q < tw->numPortals; q++ ) {
				const float *po = tw->portals[q];
				const int ax = ( po[0] > 0.5f ) ? 1 : 0, ox = 1 - ax;
				float dx, dy, dz, dd;
				if ( po[5] > 150.0f ) {
					if ( numAct < 16 )
						act[numAct++] = po;
					continue;
				}
				dx = po[1] - viewOrg[ax]; dy = po[2] - viewOrg[ox]; dz = po[3] - viewOrg[2];
				dd = dx * dx + dy * dy + dz * dz;
				if ( dd > 3000.0f * 3000.0f )
					continue;
				// keep the 24 nearest
				for ( q2 = nw; q2 > 0 && wd[q2 - 1] > dd; q2-- ) {
					if ( q2 < 24 ) { wd[q2] = wd[q2 - 1]; wi[q2] = wi[q2 - 1]; }
				}
				if ( q2 < 24 ) {
					wd[q2] = dd; wi[q2] = q;
					if ( nw < 24 ) nw++;
				}
			}
			for ( q = 0; q < nw; q++ )
				act[numAct++] = tw->portals[wi[q]];
		}
		{
			// the view tests: on the worker threads in jobs of whole cells (r_tworldThreads), or in one run here
			twCullCtx_t	cc;
			int			numJobs = 1, jb, b;
			int			cnt[65];
			cc.tw = tw; cc.frustum = frustum; cc.viewOrg = viewOrg; cc.act = act; cc.numAct = numAct;
			cc.cull = cull; cc.inside = inside; cc.cullSize = cullSize; cc.kCull = kCull; cc.kLod1 = kLod1; cc.kLod2 = kLod2;
			TW_PoolSize( r_tworldThreads->integer );
			if ( s_twPool.numThreads && tw->numCullJobs > 1 ) {
				numJobs = tw->numCullJobs;
				TW_PoolRun( TW_CullJob, &cc, numJobs );
			} else {
				tw->cullJobBase[0] = 0;
				tw->cullJobCount[0] = TW_CullCells( &cc, 0, tw->numCells, 0, &tw->cullJobSmall[0] );
			}
			// counting sort, nearest first, over the jobs' results in job order (the order of the cells)
			Com_Memset( cnt, 0, sizeof( cnt ) );
			for ( jb = 0; jb < numJobs; jb++ ) {
				const int base = tw->cullJobBase[jb], end = base + tw->cullJobCount[jb];
				for ( k = base; k < end; k++ )
					cnt[tw->visKey[k] + 1]++;
				nAll += tw->cullJobCount[jb];
				tw->c_small += tw->cullJobSmall[jb];
			}
			for ( b = 1; b < 65; b++ )
				cnt[b] += cnt[b - 1];
			for ( jb = 0; jb < numJobs; jb++ ) {
				const int base = tw->cullJobBase[jb], end = base + tw->cullJobCount[jb];
				for ( k = base; k < end; k++ ) {
					const int at = cnt[tw->visKey[k]]++;
					tw->visAllInst[at] = tw->sortInst[k];
					tw->visAllMesh[at] = tw->sortMesh[k];
					tw->visAllLod[at] = tw->sortLod[k];
				}
			}
			if ( numJobs > 1 && r_tworldThreadsCheck->integer && TW_CheckAlloc( tw->numInstances + 1, 0 ) ) {
				// debug: the same view tests in one run, compared with the jobs' result (placements, order, levels)
				int sm = 0, nRef, bad = 0;
				nRef = TW_CullCells( &cc, 0, tw->numCells, 0, &sm );
				Com_Memset( cnt, 0, sizeof( cnt ) );
				for ( k = 0; k < nRef; k++ )
					cnt[tw->visKey[k] + 1]++;
				for ( b = 1; b < 65; b++ )
					cnt[b] += cnt[b - 1];
				for ( k = 0; k < nRef; k++ ) {
					const int at = cnt[tw->visKey[k]]++;
					s_twCheck.inst[at] = tw->sortInst[k];
					s_twCheck.mesh[at] = tw->sortMesh[k];
					s_twCheck.lod[at] = tw->sortLod[k];
				}
				if ( nRef != nAll || sm != tw->c_small ) {
					bad = 1;
				} else {
					for ( k = 0; k < nAll && !bad; k++ )
						bad = ( s_twCheck.inst[k] != tw->visAllInst[k] || s_twCheck.mesh[k] != tw->visAllMesh[k] || s_twCheck.lod[k] != tw->visAllLod[k] );
				}
				s_twCheck.cullViews++;
				s_twCheck.cullBad += bad;
			}
		}
		tw->numVisAll = nAll;
	}

	// the instance data written in jobs on the worker threads (r_tworldThreads): the visible placements split into runs
	// of fillChunk; each job writes its run's records at positions counted here, exactly where the one-thread loop
	// below would put them
	const int		fillStride = tw->numSubs + 1;
	const int		fillJobs = ( s_twPool.numThreads && tw->fillCount && tw->numVisAll >= 2048 )
							? ( ( s_twPool.numThreads + 1 ) * 2 < TW_FILL_JOBS ? ( s_twPool.numThreads + 1 ) * 2 : TW_FILL_JOBS ) : 0;
	const int		fillChunk = fillJobs ? ( tw->numVisAll + fillJobs - 1 ) / fillJobs : 0;

	// this pass: the placements whose mesh has something to draw in it
	for ( k = 0; k < tw->numVisAll; k++ ) {
		const int i = tw->visAllInst[k];
		const int lod = tw->visAllLod[k];
		const tworldMesh_t *mesh = &tw->meshes[tw->visAllMesh[k]];

		if ( !( mesh->lodPassMask[lod] & ( 1 << pass ) ) )
			continue;

		int *fillRow = fillJobs ? tw->fillCount + ( numVis / fillChunk ) * fillStride : NULL;
		tw->visInst[numVis] = i;
		tw->visLod[numVis] = (byte)lod;
		numVis++;

		for ( j = 0; j < mesh->lodNumSubs[lod]; j++ ) {
			const int s = mesh->lodFirstSub[lod] + j;
			if ( tw->subs[s].pass == pass && tw->subs[s].surf.numIndexes ) {
				if ( tw->batchCount[s]++ == 0 ) {
					tw->drawList[numDrawList++] = s;		// first seen = nearest
					if ( fillJobs ) {
						int fj;
						for ( fj = 0; fj < fillJobs; fj++ )
							tw->fillCount[fj * fillStride + s] = 0;
					}
				}
				if ( fillRow )
					fillRow[s]++;
				total++;
			}
		}

		if ( pass == 0 ) {
			tw->c_instances++;
			tw->c_lod[lod]++;
		}
	}

	const double tB = TW_Now();
	s_twTime[0] += tB - tA;
	if ( !total )
		return qtrue;

	data = (twInstData_t *)vk_reserve_uniform( (size_t)total * sizeof( twInstData_t ), &bufOffset );
	VectorScale( tw->ambientColor, light, u.ambientLight );
	u.ambientLight[3] = (float)backEnd.refdef.floatTime;		// animation time
	VectorScale( tw->sunColor, light, u.directedLight );
	VectorCopy( tw->sunDir, u.localLightOrigin );
	// light scale for pre-compressed textures, the same curve R_LightScaleTexture bakes into uncompressed ones
	u.localViewOrigin[0] = r_intensity->value;
	if ( !glConfig.deviceSupportsGamma && !vk.fboActive ) {
		u.localViewOrigin[1] = 1.0f / r_gamma->value;
		u.localViewOrigin[2] = (float)( 1 << tr.overbrightBits );
	} else {
		u.localViewOrigin[1] = 1.0f;
		u.localViewOrigin[2] = 1.0f;
	}
	if ( tw->shadowImage && r_tworldShadows->value > 0.0f ) {
		Com_Memcpy( u.modelMatrix, tw->shadowRow, sizeof( tw->shadowRow ) );
		u.modelMatrix[12] = r_tworldShadowOffset->value * tw->shadowTexel;
		u.modelMatrix[13] = r_tworldShadowBias->value * tw->shadowTexel / tw->shadowRange;
		u.modelMatrix[14] = r_tworldShadows->value;		// above 1: debug views, see tworld.frag
	}
	// where the sun map ends and the lamp rows begin (0 = no lamp rows: the sun lookup uses the whole texture)
	u.modelMatrix[15] = (float)tw->lampSunHeight;
	uboOffset = data ? vk_append_uniform( &u, sizeof( u ), vk.uniform_entity_item_size ) : ~0U;
	{
		// the pass's own block in the main uniform slot: camera and sky, for water reflections
		vkUniform_t m = {};
		float *f = (float *)&m;
		VectorCopy( viewOrg, f );					f[3] = tw->metresPerUnit;
		VectorCopy( tw->skyZenith, f + 4 );			f[7] = r_tworldWaterBump->value;
		VectorCopy( tw->skyHorizon, f + 8 );
		f[12] = usePbr ? 1.0f : 0.0f;
		f[13] = r_tworldFoliageLight->value;
		f[14] = ( r_tworldHaze->value > 1.0f ) ? 0.693147f / r_tworldHaze->value : 0.0f;
		f[35] = Com_Clamp( 0.0f, 1.0f, r_tworldIndoor->value );
		f[43] = Com_Clamp( 0.0f, 1.0f, r_tworldAO->value );
		{
			// how enclosed the viewer is: the sky occlusion of the small placements within 500 units, nearer ones
			// counting more, eased over a few frames (window panes show daylight only to a viewer indoors)
			static float viewerIndoor = 0.0f;
			static int indoorTick = 0;
			float sum = 0.0f, wsum = 0.0f, target = 0.0f;
			if ( tw->skyOcc && pass == 0 && ( indoorTick++ & 7 ) == 0 ) {
				int ii;
				float sum = 0.0f, wsum = 0.0f, target = 0.0f;
				for ( ii = 0; ii < tw->numInstances; ii++ ) {
					const twCullRec_t *rec = &tw->cullRecs[ii];
					vec3_t dd;
					float d2;
					if ( rec->radius < 40.0f || rec->radius > 900.0f )
						continue;
					VectorSubtract( rec->center, viewOrg, dd );
					d2 = DotProduct( dd, dd );
					if ( d2 < 500.0f * 500.0f ) {
						const float wgt = 1.0f / ( 100.0f + sqrtf( d2 ) );
						sum += wgt * tw->skyOcc[rec->inst]; wsum += wgt;
					}
				}
				if ( wsum > 0.0f )
					target = Com_Clamp( 0.0f, 1.0f, ( sum / wsum / 255.0f - 0.35f ) / 0.3f );
				viewerIndoor += ( target - viewerIndoor ) * 0.6f;
			}
			f[39] = viewerIndoor;
		}
		f[11] = tw->originM[0];
		f[15] = tw->originM[1];
		if ( tw->instLamps ) {
			// every lamp at any distance: the placements carry their lamp lists, the shader reads the lamps from the
			// shadow texture. lightPos[*].w = 0 switches the nearest-four path off; lightPos[0].x is the brightness
			f[16] = r_tworldLamps->value;
			f[17] = 1.0f;
			f[18] = r_tworldLampDebug->value;
			f[20] = (float)tw->lampTileRow;		// lightPos[1].x (lightPos[1].w stays 0)
			f[21] = r_tworldLampDistance->value > 0.0f ? r_tworldLampDistance->value : 1e9f;		// lightPos[1].y
		} else if ( tw->numLights && r_tworldLamps->value > 0.0f ) {
			// the four lights nearest the camera (distance beyond their radius)
			int best[5] = { -1, -1, -1, -1, -1 };
			float bestD[5] = { 1e30f, 1e30f, 1e30f, 1e30f, 1e30f };
			int li, lj, lk;
			for ( li = 0; li < tw->numLights; li++ ) {
				const float *L = tw->lights + li * 8;
				vec3_t dd;
				float dist;
				VectorSubtract( L, viewOrg, dd );
				dist = VectorLength( dd ) - L[3];
				for ( lj = 0; lj < 5; lj++ ) {
					if ( dist < bestD[lj] ) {
						for ( lk = 4; lk > lj; lk-- ) { best[lk] = best[lk - 1]; bestD[lk] = bestD[lk - 1]; }
						best[lj] = li; bestD[lj] = dist;
						break;
					}
				}
			}
			for ( lj = 0; lj < 4; lj++ ) {
				if ( best[lj] >= 0 && bestD[lj] < 20000.0f ) {
					const float *L = tw->lights + best[lj] * 8;
					VectorCopy( L, f + 16 + lj * 4 );
					f[16 + lj * 4 + 3] = 1.0f / ( L[3] * L[3] );
					// fade each lamp out as it approaches the distance of the fifth nearest: it reaches zero just
					// before another lamp takes its place, so lamps never switch on or off visibly
					float fadeL = 1.0f;
					if ( best[4] >= 0 ) {
						const float span = ( bestD[4] > 0.0f ? bestD[4] : 0.0f ) * 0.3f + 60.0f;
						fadeL = Com_Clamp( 0.0f, 1.0f, ( bestD[4] - bestD[lj] ) / span );
					}
					VectorScale( L + 4, r_tworldLamps->value * fadeL, f + 32 + lj * 4 );
				}
			}
		}
		mainOffset = ( uboOffset != ~0U ) ? vk_append_uniform( &m, sizeof( m ), vk.uniform_item_size ) : ~0U;
	}

	if ( !data || uboOffset == ~0U || mainOffset == ~0U ) {
		// per-frame buffer too small: skip the cooked world this frame and ask for a bigger buffer
		tw->c_uboFail++;
		if ( !vk.geometry_buffer_size_new && vk.geometry_buffer_size < ( 256u << 20 ) )
			vk.geometry_buffer_size_new = vk.geometry_buffer_size * 2;
		return qtrue;
	}

	const double tC = TW_Now();
	s_twTime[1] += tC - tB;
	for ( i = 0; i < tw->numSubs; i++ ) {
		tw->batchStart[i] = tw->batchCursor[i] = run;
		run += tw->batchCount[i];
	}

	if ( fillJobs ) {
		// each job's first record per material range: the ranges' starts plus the earlier jobs' counts
		twFillCtx_t fc;
		int fj, d;
		for ( d = 0; d < numDrawList; d++ ) {
			const int s = tw->drawList[d];
			int cur = tw->batchStart[s];
			for ( fj = 0; fj < fillJobs; fj++ ) {
				const int n = tw->fillCount[fj * fillStride + s];
				tw->fillCount[fj * fillStride + s] = cur;
				cur += n;
			}
		}
		fc.tw = tw; fc.data = data; fc.pass = pass; fc.numVis = numVis; fc.chunk = fillChunk; fc.stride = fillStride; fc.usePbr = usePbr;
		TW_PoolRun( TW_FillJob, &fc, fillJobs );
		if ( r_tworldThreadsCheck->integer && TW_CheckAlloc( 0, total ) ) {
			// debug: the same records written in one run into a scratch buffer, compared byte for byte
			for ( i = 0; i < numVis; i++ ) {
				const tworldInst_t *inst = &tw->instances[tw->visInst[i]];
				const tworldMesh_t *mesh = &tw->meshes[inst->mesh];
				const int lod = tw->visLod[i];
				for ( j = 0; j < mesh->lodNumSubs[lod]; j++ ) {
					const int s = mesh->lodFirstSub[lod] + j;
					const tworldSub_t *sub = &tw->subs[s];
					if ( sub->pass != pass || !sub->surf.numIndexes )
						continue;
					TW_WriteInstRecord( tw, &s_twCheck.data[tw->batchCursor[s]++], tw->visInst[i], mesh, sub, inst->matrix, usePbr );
				}
			}
			s_twCheck.fillPasses++;
			s_twCheck.fillBad += ( memcmp( s_twCheck.data, data, (size_t)total * sizeof( twInstData_t ) ) != 0 ) ? 1 : 0;
		}
	} else {
		for ( i = 0; i < numVis; i++ ) {
			const tworldInst_t *inst = &tw->instances[tw->visInst[i]];
			const tworldMesh_t *mesh = &tw->meshes[inst->mesh];
			const int lod = tw->visLod[i];
			for ( j = 0; j < mesh->lodNumSubs[lod]; j++ ) {
				const int s = mesh->lodFirstSub[lod] + j;
				const tworldSub_t *sub = &tw->subs[s];
				if ( sub->pass != pass || !sub->surf.numIndexes )
					continue;
				TW_WriteInstRecord( tw, &data[tw->batchCursor[s]++], tw->visInst[i], mesh, sub, inst->matrix, usePbr );
			}
		}
	}

	const double tD = TW_Now();
	s_twTime[2] += tD - tC;
	// world matrix in the push constants, dynamic state, our lighting uniform
	Com_Memcpy( vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64 );
	vk_update_mvp( NULL );
	vk_tworld_set_dynamic_state();
	vk_tworld_reset_descriptors();
	vk_update_descriptor( VK_DESC_UNIFORM, vk.cmd->uniform_descriptor );
	vk_update_descriptor_offset( VK_DESC_UNIFORM_ENTITY_BINDING, uboOffset );
	vk_update_descriptor_offset( VK_DESC_UNIFORM_MAIN_BINDING, mainOffset );

	// opaque pass: nearest batch first; blended pass: the fixed order
	for ( i = 0; i < ( pass == 0 ? numDrawList : tw->numSubs ); i++ ) {
		const int s = ( pass == 0 ) ? tw->drawList[i] : tw->subOrder[i];
		const int count = tw->batchCount[s];
		tworldSub_t *sub = &tw->subs[s];
		const shaderStage_t *stage;
		image_t *image;
		VkBuffer bufs[2];
		VkDeviceSize offs[2];

		if ( !count )
			continue;

		stage = sub->shader->stages[0];
		if ( !stage )
			continue;

		if ( !sub->pipeline[mirror] ) {
			Vk_Pipeline_Def def;
			Com_Memset( &def, 0, sizeof( def ) );
			def.shader_type = TYPE_TWORLD;
			def.state_bits = stage->stateBits;
			def.face_culling = sub->shader->cullType;
			def.polygon_offset = sub->shader->polygonOffset;
			def.mirror = (qboolean)mirror;
			sub->pipeline[mirror] = vk_find_pipeline_ext( 0, &def, qfalse ) + 1;
		}

		image = stage->bundle[0].image[0] ? stage->bundle[0].image[0] : tr.whiteImage;

		vk_bind_pipeline( sub->pipeline[mirror] - 1 );
		vk_update_descriptor( VK_DESC_TEXTURE0, image->descriptor_set );
		{
			const shaderStage_t *ds = ( tw->details[sub->tintIndex] > 0.0f || tw->anims[sub->tintIndex] == 1 || tw->anims[sub->tintIndex] == 4 ) ? sub->shader->stages[1] : NULL;
			image_t *detail = ( ds && ds->bundle[0].image[0] ) ? ds->bundle[0].image[0] : tr.whiteImage;
			// blended ground (tile code 254): the second texture is its weight map, with or without the PBR path
			if ( !ds && ( tw->anims[sub->tintIndex] == 0 || tw->anims[sub->tintIndex] >= 6 ) && tw->pbrImages[sub->tintIndex] && ( usePbr || ( (int)tw->tilePacks[sub->tintIndex] >> 10 ) == 254 ) )
				detail = tw->pbrImages[sub->tintIndex];
			vk_update_descriptor( VK_DESC_TEXTURE0 + 1, detail->descriptor_set );
		}
		vk_update_descriptor( VK_DESC_TEXTURE0 + 2, ( tw->shadowImage ? tw->shadowImage : tr.whiteImage )->descriptor_set );
		if ( vk.maxBoundDescriptorSets >= 5 )
			vk_update_descriptor( VK_DESC_TEXTURE0 + 3, ( tw->tileImages[sub->tintIndex] ? tw->tileImages[sub->tintIndex] : tr.whiteImage )->descriptor_set );
		vk_bind_descriptor_sets();

		if ( sub->shader->polygonOffset )
			qvkCmdSetDepthBias( vk.cmd->command_buffer, r_offsetUnits->value, 0.0f, r_offsetFactor->value );

		bufs[0] = sub->surf.vbo->buffer;	offs[0] = 0;
		bufs[1] = vk.cmd->vertex_buffer;	offs[1] = (VkDeviceSize)bufOffset + (VkDeviceSize)tw->batchStart[s] * sizeof( twInstData_t );
		qvkCmdBindVertexBuffers( vk.cmd->command_buffer, 0, 2, bufs, offs );
		vk_bind_index_buffer( sub->surf.ibo->buffer, 0, VK_INDEX_TYPE_UINT32 );
		qvkCmdDrawIndexed( vk.cmd->command_buffer, sub->surf.numIndexes, count, sub->surf.indexOffset, 0, 0 );

		draws++;
		indexes += sub->surf.numIndexes * count;
	}

	vk_tworld_reset_descriptors();
	s_twTime[3] += TW_Now() - tD;

	if ( pass == 0 ) {
		tw->c_draws = draws;
		tw->c_indexes = indexes;
	}
	return qtrue;
}

/*
=================
RB_SurfaceTWorld

Back end: cull placements, pick a detail level, set the model-view matrix and
a private lighting uniform per placement, and draw each material range
through the normal stage iterator.
=================
*/
void RB_SurfaceTWorld( void *surface )
{
	tworld_t		*tw = s_tworld;
	const int		pass = ( tw && surface == (void *)&tw->blendSurfaceType ) ? 1 : 0;
	shader_t		*savedShader = tess.shader;
	const int		savedFog = tess.fogNum;
	const uint32_t	savedWorldUbo = vk.cmd->entity_ubo_offset[REFENTITYNUM_WORLD];
	const cplane_t	*frustum = backEnd.viewParms.frustum;
	const qboolean	cull = (qboolean)( !r_nocull->integer );
	const float		cullSize = r_tworldCullSize->value;
	const float		lodSize1 = r_tworldLodSize1->value;
	const float		lodSize2 = r_tworldLodSize2->value;
	const float		light = r_tworldLight->value;
	const float		*viewOrg = backEnd.viewParms.ori.origin;
	float			modelView[16];
	int				i, j, p;

	if ( !tw )
		return;

	// finish whatever batch was open (normally nothing: our dummy shader has no geometry)
	RB_EndSurface();

	if ( r_tworldInstancing->integer && RB_TWorldInstanced( tw, pass ) ) {
		RB_BeginSurface( savedShader, savedFog );
		goto stats;
	}

	if ( pass == 0 ) {
		tw->c_instances = tw->c_draws = tw->c_indexes = tw->c_small = tw->c_uboFail = 0;
		tw->c_lod[0] = tw->c_lod[1] = tw->c_lod[2] = 0;
	}

	for ( i = 0; i < tw->numInstances; i++ ) {
		const tworldInst_t *inst = &tw->instances[i];
		const tworldMesh_t *mesh = &tw->meshes[inst->mesh];
		vec3_t	d;
		float	dist, size;
		int		lod = 0, lastTint = -1;

		if ( cull ) {
			for ( p = 0; p < 4; p++ ) {
				if ( DotProduct( inst->center, frustum[p].normal ) - frustum[p].dist < -inst->radius )
					break;
			}
			if ( p < 4 )
				continue;
		}

		// projected size: radius over distance to the surface of the bounding sphere
		VectorSubtract( inst->center, viewOrg, d );
		dist = VectorLength( d ) - inst->radius;
		if ( dist > 1.0f ) {
			size = inst->radius / dist;
			if ( size < cullSize ) {
				if ( pass == 0 )
					tw->c_small++;
				continue;
			}
			if ( mesh->numLods > 2 && size < lodSize2 )
				lod = 2;
			else if ( mesh->numLods > 1 && size < lodSize1 )
				lod = 1;
		}

		if ( !( mesh->lodPassMask[lod] & ( 1 << pass ) ) )
			continue;		// nothing of this placement belongs to this pass

		myGlMultMatrix( inst->matrix, backEnd.viewParms.world.modelViewMatrix, modelView );
		Com_Memcpy( vk_world.modelview_transform, modelView, 64 );
		vk_update_mvp( NULL );

		if ( pass == 0 ) {
			tw->c_instances++;
			tw->c_lod[lod]++;
		}

		for ( j = 0; j < mesh->lodNumSubs[lod]; j++ ) {
			tworldSub_t *sub = &tw->subs[mesh->lodFirstSub[lod] + j];

			if ( !sub->surf.numIndexes || sub->pass != pass )
				continue;

			// lighting uniform: sun and ambient multiplied by the material tint, normals rotated by the placement
			if ( sub->tintIndex != lastTint ) {
				const float *tint = tw->tints[sub->tintIndex];
				vkUniformEntity_t u = {};
				uint32_t ofs;

				u.ambientLight[0] = tw->ambientColor[0] * tint[0] * light;
				u.ambientLight[1] = tw->ambientColor[1] * tint[1] * light;
				u.ambientLight[2] = tw->ambientColor[2] * tint[2] * light;
				u.directedLight[0] = tw->sunColor[0] * tint[0] * light;
				u.directedLight[1] = tw->sunColor[1] * tint[1] * light;
				u.directedLight[2] = tw->sunColor[2] * tint[2] * light;
				VectorCopy( tw->sunDir, u.localLightOrigin );
				u.localLightOrigin[3] = 0.0f;		// directional
				Com_Memcpy( u.modelMatrix, inst->matrix, sizeof( u.modelMatrix ) );

				ofs = vk_append_uniform( &u, sizeof( u ), vk.uniform_entity_item_size );
				if ( ofs != ~0U ) {
					vk.cmd->entity_ubo_offset[REFENTITYNUM_WORLD] = ofs;
					lastTint = sub->tintIndex;
				} else {
					// per-frame buffer full: keep the previous uniform for this frame and ask for a
					// bigger buffer, which the frame code creates before the next frame
					tw->c_uboFail++;
					if ( !vk.geometry_buffer_size_new && vk.geometry_buffer_size < ( 256u << 20 ) )
						vk.geometry_buffer_size_new = vk.geometry_buffer_size * 2;
				}
			}

			RB_BeginSurface( sub->shader, 0 );
			RB_SurfaceVBOMDVMesh( &sub->surf );
			RB_EndSurface();

			tw->c_draws++;
			tw->c_indexes += sub->surf.numIndexes;
		}
	}

	// back to the world matrix, the world lighting uniform and the batch the caller expects
	vk.cmd->entity_ubo_offset[REFENTITYNUM_WORLD] = savedWorldUbo;
	Com_Memcpy( vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64 );
	vk_update_mvp( NULL );
	RB_BeginSurface( savedShader, savedFog );

stats:
	// benchmark recorder: frame times between two changes of r_tworldBench (a label). Each change prints the
	// finished section: frames, average, 1 % low (99th-percentile frame time) and the worst frame.
	if ( pass == 0 && r_tworldBench && backEnd.viewParms.portalView == PV_NONE ) {
		static double	lastT = 0.0, sumMs = 0.0, worstMs = 0.0;
		static int		hist[800], frames = 0, lastMod = -1;		// 0.25 ms bins up to 200 ms
		static char		label[64] = "";
		const double	t = TW_Now();
		if ( lastT > 0.0 && label[0] ) {
			const double ms = t - lastT;
			int b = (int)( ms * 4.0 );
			hist[b < 0 ? 0 : ( b > 799 ? 799 : b )]++;
			sumMs += ms; frames++;
			if ( ms > worstMs ) worstMs = ms;
		}
		lastT = t;
		if ( r_tworldBench->modificationCount != lastMod ) {
			lastMod = r_tworldBench->modificationCount;
			if ( label[0] && frames > 10 ) {
				int acc = 0, b = 799;
				const int cut = frames / 100;
				for ( ; b > 0; b-- ) {
					acc += hist[b];
					if ( acc > cut )
						break;
				}
				ri.Printf( PRINT_ALL, "TWBENCH %s: frames %i, avg %.1f fps (%.2f ms), 1%% low %.1f fps (%.2f ms), worst frame %.1f ms, %i x %i, %i placements, %i draws, %i tris\n",
					label, frames, 1000.0 * frames / sumMs, sumMs / frames, 1000.0 / ( ( b + 1 ) * 0.25 ), ( b + 1 ) * 0.25, worstMs,
					glConfig.vidWidth, glConfig.vidHeight, tw->c_instances, tw->c_draws, tw->c_indexes / 3 );
			}
			Q_strncpyz( label, r_tworldBench->string, sizeof( label ) );
			if ( !Q_stricmp( label, "0" ) || !Q_stricmp( label, "off" ) )
				label[0] = 0;
			Com_Memset( hist, 0, sizeof( hist ) );
			sumMs = worstMs = 0.0; frames = 0;
		}
	}
	if ( pass == 0 && r_tworldStats && r_tworldStats->integer && backEnd.viewParms.portalView == PV_NONE ) {
		const int now = ri.Milliseconds();
		static int statFrames = 0;
		statFrames++;
		if ( now - tw->lastStatsTime >= 1000 ) {
			const float fps = statFrames * 1000.0f / (float)( now - tw->lastStatsTime );
			const double statN = statFrames > 0 ? statFrames : 1;
			statFrames = 0;
			tw->lastStatsTime = now;
			if ( r_tworldThreadsCheck->integer )
				ri.Printf( PRINT_ALL, "TWORLD threads check (%i workers): culling %i views, %i differ; instance data %i passes, %i differ\n",
					s_twPool.numThreads, s_twCheck.cullViews, s_twCheck.cullBad, s_twCheck.fillPasses, s_twCheck.fillBad );
			ri.Printf( PRINT_ALL, "TWORLD cpu ms per frame: cull %.2f, uniforms %.2f, fill %.2f, draw %.2f (frame %.2f)\n",
				s_twTime[0] / statN, s_twTime[1] / statN, s_twTime[2] / statN, s_twTime[3] / statN, 1000.0f / fps );
			s_twTime[0] = s_twTime[1] = s_twTime[2] = s_twTime[3] = 0.0;
			ri.Printf( PRINT_ALL, "TWORLD view: %.0f fps, %i of %i instances (lod %i/%i/%i, %i too small), %i draws, %i tris%s\n",
				fps, tw->c_instances, tw->numInstances, tw->c_lod[0], tw->c_lod[1], tw->c_lod[2], tw->c_small,
				tw->c_draws, tw->c_indexes / 3, tw->c_uboFail ? " UNIFORM BUFFER FULL" : "" );
		}
	}
}

#else // !USE_VBO_MDV

void R_LoadTWorld( const char *bspName ) {}
void R_AddTWorldSurface( void ) {}
void RB_SurfaceTWorld( void *surface ) {}

#endif
