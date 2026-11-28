/*
===========================================================================
cm_shared.cpp -- shared collision: terrain height grids and collision shapes placed many times

A map may ship maps/<name>.theight (terrain) and maps/<name>.tcol (object collision) beside its .bsp (written by the
JKA Mod Studio for SWTOR maps; formats in the doc "NicJK collision formats: heightmap terrain and shared object
collision"). Instead of a brush per triangle per placed copy in the .bsp (Dromund Kaas: 7.2 million brushes, a
3.1 GB map), each grid and each shape is stored once.

Nothing per triangle is kept: for each triangle a trace may touch, its brush is built in world space on the spot -
the six axial planes first (CM_TestBoxInBrush skips them), its faces, then edge bevels as q3map2 adds them - and
handed to the ordinary CM_TraceThroughBrush / CM_TestBoxInBrush. So it collides exactly like the baked brushes:

  terrain triangle, and a placed triangle facing up (within 60 degrees, as placed): an upright prism - the
  triangle on top, a flat bottom some depth under its lowest corner, a vertical side through each edge
  any other triangle: a slab - its plane, a back plane wallDepth behind it, a side through each edge (q3map2's
  clipModel brush)

Only the world model (clip handle 0) of the main map uses them. cm_sharedCollision 0 turns them off (comparisons).

cm_sharedcompare [traces] [seed]: whether the shared files alone collide like the .bsp's own clip brushes - a map
built with both (the Studio's "also") traced both ways: the whole .bsp, and the .bsp without its brushes of the
.../collision and .../floorclip shaders (what the Studio's "only" leaves out) plus the shared files.
===========================================================================
*/

#include "cm_local.h"
#include "qcommon/qcommon.h"

#pragma pack(push, 1)
typedef struct {
	char		magic[4];
	uint32_t	version, numMaterials, numTiles, numHeights, numHoleBytes;
} thHeader_t;						// 24

typedef struct {
	char		shader[64];
	uint32_t	contentFlags, surfaceFlags;
} shMaterialFile_t;					// 72

typedef struct {
	float		origin[3];
	float		axisX[2], axisY[2];
	uint32_t	width, depth;
	uint32_t	material;
	float		prismDepth;
	uint32_t	firstHeight, firstHoleByte;
	float		mins[3], maxs[3];
} thTile_t;							// 76

typedef struct {
	char		magic[4];
	uint32_t	version, numMaterials, numShapes, numVerts, numTris, numPlacements;
	float		floorDepth, wallDepth;
} tcHeader_t;						// 36

typedef struct {
	uint32_t	firstVert, numVerts, firstTri, numTris;
	float		mins[3], maxs[3];
} tcShape_t;						// 40

typedef struct {
	uint32_t	v[3];
	uint16_t	material, flags;
} tcTri_t;							// 16

typedef struct {
	uint32_t	shape;
	float		m[12];
	float		mins[3], maxs[3];
} tcPlacement_t;					// 76
#pragma pack(pop)

typedef struct {
	int			shaderNum;			// into cmg.shaders
	int			contents;
} shMaterial_t;

typedef struct {
	float		mins[3], maxs[3];
	int			first, count;		// a leaf: its triangles (count > 0); a node: first = left child, right = first + 1
} shNode_t;

typedef struct {
	int			firstNode;			// its tree, in sh.nodes
	int			firstTri;			// its triangles, ordered by the tree, in sh.order
} shShapeTree_t;

static struct {
	bool			loaded;
	// terrain
	int				numTiles;
	thTile_t		*tiles;
	float			*heights;
	byte			*holes;
	int				numHoleBytes;
	shMaterial_t	*thMats;
	int				numThMats;
	// objects
	int				numShapes, numPlacements;
	tcShape_t		*shapes;
	float			*verts;
	tcTri_t			*tris;
	tcPlacement_t	*places;
	shMaterial_t	*tcMats;
	int				numTcMats;
	float			floorDepth, wallDepth;
	shShapeTree_t	*trees;
	shNode_t		*nodes;
	int				*order;
	// a grid of tile and placement boxes
	float			gridOrigin[2];
	float			cellSize;
	int				gridW, gridH;
	int				*cellStart;		// gridW * gridH + 1
	int				*cellItems;		// item = tile index, or numTiles + placement index
	int				*stamp;			// per item: the query that last visited it
	int				stampNow;
} sh;

static cvar_t *cm_sharedCollision;

// job threads (q_jobs): their own "visited by this query" stamps, allocated by CM_SharedThreadScratch
// before a parallel loop (CM_ThreadScratchEnsure); the main thread keeps sh.stamp / sh.stampNow
struct alignas( 64 ) shThreadStamp_t {
	int		*stamp;
	int		numItems;
	int		now;
};
static shThreadStamp_t sh_threadStamp[JOBS_MAX_THREADS];

void CM_SharedThreadScratchFree( void ) {
	for ( int t = 0; t < JOBS_MAX_THREADS; t++ ) {
		if ( sh_threadStamp[t].stamp ) {
			Z_Free( sh_threadStamp[t].stamp );
		}
		sh_threadStamp[t].stamp = NULL;
		sh_threadStamp[t].numItems = 0;
		sh_threadStamp[t].now = 0;
	}
}

// true when the arrays fit the loaded files (else the caller reallocates everything)
bool CM_SharedThreadScratchOk( int threads ) {
	const int numItems = sh.loaded ? sh.numTiles + sh.numPlacements : 0;
	for ( int t = 1; t < threads; t++ ) {
		if ( sh_threadStamp[t].numItems != numItems || ( numItems && !sh_threadStamp[t].stamp ) ) {
			return false;
		}
	}
	return true;
}

void CM_SharedThreadScratch( int threads ) {
	const int numItems = sh.loaded ? sh.numTiles + sh.numPlacements : 0;
	CM_SharedThreadScratchFree();
	for ( int t = 1; t < threads; t++ ) {
		sh_threadStamp[t].numItems = numItems;
		if ( numItems ) {
			sh_threadStamp[t].stamp = (int *)Z_Malloc( ( (size_t)numItems + 1 ) * sizeof( int ), TAG_GENERAL, qtrue );
		}
	}
}
static void CM_SharedCompare_f( void );
const byte *cm_skipBrushShader = NULL;
int cm_skipBrushShaderCount = 0;

#define SH_CELL			1024.0f
#define SH_LEAF_TRIS	8
#define SH_SPLIT_BY_COUNT	40		// tree levels split at the middle of space; below, by count (at most log2 more)
#define SH_STACK		128			// the tree walk's stack: two per level
#define SH_WORLD		4.0e6f		// a box beyond this (or not finite) does not size the grid: at most 7,813 cells a side
#define SH_MAX_SIDES	64
#define SH_TRACE_PIECE	512.0f		// a long trace is tested in pieces this long, from its start

void CM_SharedClear( void ) {
	Com_Memset( &sh, 0, sizeof( sh ) );
}

/*
=================
a material's shader: the .bsp's own entry when it has one of that name, else a new entry at the end of cmg.shaders
(its surface flags give footsteps and marks: CM_TraceThroughBrush reads cmg.shaders[side->shaderNum])
=================
*/
static int CM_SharedShader( const shMaterialFile_t *m ) {
	char name[MAX_QPATH];
	Q_strncpyz( name, m->shader, sizeof( name ) );
	for ( int i = 0; i < cmg.numShaders; i++ ) {
		if ( !Q_stricmp( cmg.shaders[i].shader, name ) ) {
			return i;
		}
	}
	CCMShader *grown = (CCMShader *)Hunk_Alloc( ( cmg.numShaders + 2 ) * sizeof( CCMShader ), h_high );
	Com_Memcpy( grown, cmg.shaders, cmg.numShaders * sizeof( CCMShader ) );
	CCMShader *out = &grown[cmg.numShaders];
	Q_strncpyz( out->shader, name, sizeof( out->shader ) );
	out->contentFlags = (int)m->contentFlags;
	out->surfaceFlags = (int)m->surfaceFlags;
	out->mNext = NULL;
	cmg.shaders = grown;
	return cmg.numShaders++;
}

static shMaterial_t *CM_SharedMaterials( const shMaterialFile_t *in, int n ) {
	shMaterial_t *out = (shMaterial_t *)Hunk_Alloc( ( n + 1 ) * sizeof( shMaterial_t ), h_high );
	for ( int i = 0; i < n; i++ ) {
		out[i].shaderNum = CM_SharedShader( &in[i] );
		out[i].contents = (int)in[i].contentFlags;
	}
	return out;
}

/*
=================
loading
=================
*/
static void *CM_SharedCopy( const void *src, size_t bytes ) {
	void *dst = Hunk_AllocLarge( bytes + 4, h_high );
	Com_Memcpy( dst, src, bytes );
	return dst;
}

static bool CM_SharedLoadHeights( const char *name ) {
	byte *buf = NULL;
	const long len = FS_ReadFile( name, (void **)&buf );
	if ( len <= 0 || !buf ) {
		return false;
	}
	const thHeader_t *h = (const thHeader_t *)buf;
	bool ok = len >= (long)sizeof( *h ) && !memcmp( h->magic, "THGT", 4 ) && h->version == 1;
	if ( ok ) {
		const size_t need = sizeof( *h ) + (size_t)h->numMaterials * sizeof( shMaterialFile_t ) + (size_t)h->numTiles * sizeof( thTile_t )
			+ (size_t)h->numHeights * 4 + h->numHoleBytes;
		ok = need == (size_t)len;
	}
	const shMaterialFile_t *mats = (const shMaterialFile_t *)( h + 1 );
	const thTile_t *tiles = ok ? (const thTile_t *)( mats + h->numMaterials ) : NULL;
	for ( uint32_t i = 0; ok && i < h->numTiles; i++ ) {
		const thTile_t *t = &tiles[i];
		const uint64_t cells = (uint64_t)( t->width - 1 ) * ( t->depth - 1 );
		ok = t->width >= 2 && t->depth >= 2 && t->material < h->numMaterials
			&& (uint64_t)t->firstHeight + (uint64_t)t->width * t->depth <= h->numHeights
			&& ( t->firstHoleByte == 0xFFFFFFFFu || (uint64_t)t->firstHoleByte + ( cells + 7 ) / 8 <= h->numHoleBytes );
	}
	if ( !ok ) {
		Com_Printf( S_COLOR_YELLOW "CM_SharedLoad: %s is not a terrain file this build reads - not used\n", name );
		FS_FreeFile( buf );
		return false;
	}
	sh.numTiles = (int)h->numTiles;
	sh.thMats = CM_SharedMaterials( mats, (int)h->numMaterials );
	sh.numThMats = (int)h->numMaterials;
	sh.tiles = (thTile_t *)CM_SharedCopy( tiles, (size_t)h->numTiles * sizeof( thTile_t ) );
	const float *hs = (const float *)( tiles + h->numTiles );
	sh.heights = (float *)CM_SharedCopy( hs, (size_t)h->numHeights * 4 );
	sh.holes = (byte *)CM_SharedCopy( hs + h->numHeights, h->numHoleBytes );
	sh.numHoleBytes = (int)h->numHoleBytes;
	Com_Printf( "CM_SharedLoad: %s: %u tiles, %u heights\n", name, h->numTiles, h->numHeights );
	FS_FreeFile( buf );
	return true;
}

// a shape's tree over its triangles' boxes (shape space), built at load
static int			s_buildNodes;
static float		*s_triBox;		// per triangle of the shape being built: mins, maxs
static int			*s_triIdx;

static void CM_SharedBuildNode( int node, int first, int count, int level ) {
	shNode_t *n = &sh.nodes[node];
	for ( int k = 0; k < 3; k++ ) {
		n->mins[k] = 1e30f;
		n->maxs[k] = -1e30f;
	}
	for ( int i = first; i < first + count; i++ ) {
		const float *b = s_triBox + 6 * s_triIdx[i];
		for ( int k = 0; k < 3; k++ ) {
			n->mins[k] = Q_min( n->mins[k], b[k] );
			n->maxs[k] = Q_max( n->maxs[k], b[3 + k] );
		}
	}
	if ( count <= SH_LEAF_TRIS ) {
		n->first = first;
		n->count = count;
		return;
	}
	// split the longest axis at the middle of the triangle centres
	int axis = 0;
	for ( int k = 1; k < 3; k++ ) {
		if ( n->maxs[k] - n->mins[k] > n->maxs[axis] - n->mins[axis] ) {
			axis = k;
		}
	}
	const float mid = 0.5f * ( n->mins[axis] + n->maxs[axis] );
	// (deep down - very uneven triangle sizes - split by count instead: every tree stays within the walk's stack)
	const bool byCount = level >= SH_SPLIT_BY_COUNT;
	int lo = first, hi = first + count - 1;
	while ( !byCount && lo <= hi ) {
		const float *b = s_triBox + 6 * s_triIdx[lo];
		if ( 0.5f * ( b[axis] + b[3 + axis] ) < mid ) {
			lo++;
		} else {
			const int t = s_triIdx[lo]; s_triIdx[lo] = s_triIdx[hi]; s_triIdx[hi] = t;
			hi--;
		}
	}
	int left = lo - first;
	if ( byCount || left == 0 || left == count ) {
		left = count / 2;			// all centres on one side: halves
	}
	const int child = s_buildNodes;
	s_buildNodes += 2;
	n->first = child;
	n->count = 0;
	CM_SharedBuildNode( child, first, left, level + 1 );
	CM_SharedBuildNode( child + 1, first + left, count - left, level + 1 );
}

static bool CM_SharedLoadShapes( const char *name ) {
	byte *buf = NULL;
	const long len = FS_ReadFile( name, (void **)&buf );
	if ( len <= 0 || !buf ) {
		return false;
	}
	const tcHeader_t *h = (const tcHeader_t *)buf;
	bool ok = len >= (long)sizeof( *h ) && !memcmp( h->magic, "TCOL", 4 ) && h->version == 1;
	if ( ok ) {
		const size_t need = sizeof( *h ) + (size_t)h->numMaterials * sizeof( shMaterialFile_t ) + (size_t)h->numShapes * sizeof( tcShape_t )
			+ (size_t)h->numVerts * 12 + (size_t)h->numTris * sizeof( tcTri_t ) + (size_t)h->numPlacements * sizeof( tcPlacement_t );
		ok = need == (size_t)len;
	}
	const shMaterialFile_t *mats = (const shMaterialFile_t *)( h + 1 );
	const tcShape_t *shapes = ok ? (const tcShape_t *)( mats + h->numMaterials ) : NULL;
	const float *verts = ok ? (const float *)( shapes + h->numShapes ) : NULL;
	const tcTri_t *tris = ok ? (const tcTri_t *)( verts + 3 * (size_t)h->numVerts ) : NULL;
	const tcPlacement_t *places = ok ? (const tcPlacement_t *)( tris + h->numTris ) : NULL;
	for ( uint32_t i = 0; ok && i < h->numShapes; i++ ) {
		const tcShape_t *s = &shapes[i];
		ok = (uint64_t)s->firstVert + s->numVerts <= h->numVerts && (uint64_t)s->firstTri + s->numTris <= h->numTris;
		for ( uint32_t t = 0; ok && t < s->numTris; t++ ) {
			const tcTri_t *tr = &tris[s->firstTri + t];
			ok = tr->v[0] < s->numVerts && tr->v[1] < s->numVerts && tr->v[2] < s->numVerts && tr->material < h->numMaterials;
		}
	}
	for ( uint32_t i = 0; ok && i < h->numPlacements; i++ ) {
		ok = places[i].shape < h->numShapes;
	}
	if ( !ok ) {
		Com_Printf( S_COLOR_YELLOW "CM_SharedLoad: %s is not a collision file this build reads - not used\n", name );
		FS_FreeFile( buf );
		return false;
	}
	// (the counts kept: the header lives in the file's buffer, freed below)
	const uint32_t numShapes = h->numShapes, numTris = h->numTris, numPlacements = h->numPlacements;
	sh.numShapes = (int)h->numShapes;
	sh.numPlacements = (int)h->numPlacements;
	sh.tcMats = CM_SharedMaterials( mats, (int)h->numMaterials );
	sh.numTcMats = (int)h->numMaterials;
	sh.floorDepth = h->floorDepth;
	sh.wallDepth = h->wallDepth;
	sh.shapes = (tcShape_t *)CM_SharedCopy( shapes, (size_t)h->numShapes * sizeof( tcShape_t ) );
	sh.verts = (float *)CM_SharedCopy( verts, (size_t)h->numVerts * 12 );
	sh.tris = (tcTri_t *)CM_SharedCopy( tris, (size_t)h->numTris * sizeof( tcTri_t ) );
	sh.places = (tcPlacement_t *)CM_SharedCopy( places, (size_t)h->numPlacements * sizeof( tcPlacement_t ) );
	FS_FreeFile( buf );

	// trees: a node per leaf of up to SH_LEAF_TRIS triangles, at most 2 * triangles / 1 nodes in all
	sh.trees = (shShapeTree_t *)Hunk_AllocLarge( ( sh.numShapes + 1 ) * sizeof( shShapeTree_t ), h_high );
	sh.nodes = (shNode_t *)Hunk_AllocLarge( ( 2 * (size_t)numTris + sh.numShapes + 1 ) * sizeof( shNode_t ), h_high );
	sh.order = (int *)Hunk_AllocLarge( ( (size_t)numTris + 1 ) * sizeof( int ), h_high );
	s_triBox = (float *)Z_MallocLarge( ( (size_t)numTris + 1 ) * 6 * sizeof( float ), TAG_TEMP_WORKSPACE, qfalse );
	s_buildNodes = 0;
	for ( int i = 0; i < sh.numShapes; i++ ) {
		const tcShape_t *s = &sh.shapes[i];
		const float *V = sh.verts + 3 * (size_t)s->firstVert;
		for ( uint32_t t = 0; t < s->numTris; t++ ) {
			const tcTri_t *tr = &sh.tris[s->firstTri + t];
			float *b = s_triBox + 6 * (size_t)t;
			for ( int k = 0; k < 3; k++ ) {
				b[k] = Q_min( V[3 * tr->v[0] + k], Q_min( V[3 * tr->v[1] + k], V[3 * tr->v[2] + k] ) );
				b[3 + k] = Q_max( V[3 * tr->v[0] + k], Q_max( V[3 * tr->v[1] + k], V[3 * tr->v[2] + k] ) );
			}
			sh.order[s->firstTri + t] = (int)t;
		}
		s_triIdx = sh.order + s->firstTri;
		sh.trees[i].firstNode = s_buildNodes++;
		sh.trees[i].firstTri = (int)s->firstTri;
		if ( s->numTris ) {
			CM_SharedBuildNode( sh.trees[i].firstNode, 0, (int)s->numTris, 0 );
		} else {
			sh.nodes[sh.trees[i].firstNode].count = 0;
			sh.nodes[sh.trees[i].firstNode].first = 0;
		}
	}
	Z_Free( s_triBox );
	s_triBox = NULL;
	Com_Printf( "CM_SharedLoad: %s: %u shapes, %u triangles, %u placements\n", name, numShapes, numTris, numPlacements );
	return true;
}

static void CM_SharedBuildGrid( void ) {
	const int numItems = sh.numTiles + sh.numPlacements;
	if ( !numItems ) {
		return;
	}
	float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
	for ( int i = 0; i < numItems; i++ ) {
		const float *mn = i < sh.numTiles ? sh.tiles[i].mins : sh.places[i - sh.numTiles].mins;
		const float *mx = i < sh.numTiles ? sh.tiles[i].maxs : sh.places[i - sh.numTiles].maxs;
		// (a box that is not a finite one inside SH_WORLD - a damaged file - does not size the grid; its item still goes
		//  in, at the clamped edge cells)
		if ( !( fabsf( mn[0] ) < SH_WORLD && fabsf( mn[1] ) < SH_WORLD && fabsf( mx[0] ) < SH_WORLD && fabsf( mx[1] ) < SH_WORLD ) ) {
			continue;
		}
		for ( int k = 0; k < 2; k++ ) {
			lo[k] = Q_min( lo[k], mn[k] );
			hi[k] = Q_max( hi[k], mx[k] );
		}
	}
	if ( lo[0] > hi[0] || lo[1] > hi[1] ) {
		lo[0] = lo[1] = hi[0] = hi[1] = 0.0f;
	}
	sh.cellSize = SH_CELL;
	sh.gridOrigin[0] = lo[0];
	sh.gridOrigin[1] = lo[1];
	sh.gridW = Q_max( 1, (int)( ( hi[0] - lo[0] ) / SH_CELL ) + 1 );
	sh.gridH = Q_max( 1, (int)( ( hi[1] - lo[1] ) / SH_CELL ) + 1 );
	const size_t cells = (size_t)sh.gridW * sh.gridH;
	sh.cellStart = (int *)Hunk_AllocLarge( ( cells + 1 ) * sizeof( int ), h_high );
	int *count = (int *)Z_MallocLarge( ( cells + 1 ) * sizeof( int ), TAG_TEMP_WORKSPACE, qtrue );
	for ( int pass = 0; pass < 2; pass++ ) {
		for ( int i = 0; i < numItems; i++ ) {
			const float *mn = i < sh.numTiles ? sh.tiles[i].mins : sh.places[i - sh.numTiles].mins;
			const float *mx = i < sh.numTiles ? sh.tiles[i].maxs : sh.places[i - sh.numTiles].maxs;
			const int x0 = Com_Clampi( 0, sh.gridW - 1, (int)( ( mn[0] - lo[0] ) / SH_CELL ) );
			const int x1 = Com_Clampi( 0, sh.gridW - 1, (int)( ( mx[0] - lo[0] ) / SH_CELL ) );
			const int y0 = Com_Clampi( 0, sh.gridH - 1, (int)( ( mn[1] - lo[1] ) / SH_CELL ) );
			const int y1 = Com_Clampi( 0, sh.gridH - 1, (int)( ( mx[1] - lo[1] ) / SH_CELL ) );
			for ( int y = y0; y <= y1; y++ ) {
				for ( int x = x0; x <= x1; x++ ) {
					const size_t c = (size_t)y * sh.gridW + x;
					if ( pass == 0 ) {
						count[c]++;
					} else {
						sh.cellItems[sh.cellStart[c] + count[c]++] = i;
					}
				}
			}
		}
		if ( pass == 0 ) {
			size_t total = 0;
			for ( size_t c = 0; c < cells; c++ ) {
				sh.cellStart[c] = (int)total;
				total += count[c];
				count[c] = 0;
			}
			sh.cellStart[cells] = (int)total;
			sh.cellItems = (int *)Hunk_AllocLarge( ( total + 1 ) * sizeof( int ), h_high );
		}
	}
	Z_Free( count );
	sh.stamp = (int *)Hunk_AllocLarge( ( (size_t)numItems + 1 ) * sizeof( int ), h_high );
	sh.stampNow = 0;
}

/*
=================
CM_SharedLoad

After the main map's .bsp: its maps/<name>.theight and .tcol, when it has them.
=================
*/
void CM_SharedLoad( const char *bspName ) {
	char base[MAX_QPATH], name[MAX_QPATH];

	CM_SharedClear();
	cm_sharedCollision = Cvar_Get( "cm_sharedCollision", "1", CVAR_CHEAT, "Shared collision (maps/<name>.theight and .tcol): 0 = the .bsp's own collision only" );
	COM_StripExtension( bspName, base, sizeof( base ) );
	Com_sprintf( name, sizeof( name ), "%s.theight", base );
	const bool hasHeights = CM_SharedLoadHeights( name );
	Com_sprintf( name, sizeof( name ), "%s.tcol", base );
	const bool hasShapes = CM_SharedLoadShapes( name );
	if ( hasHeights || hasShapes ) {
		CM_SharedBuildGrid();
		sh.loaded = true;
	}
	static bool registered = false;
	if ( !registered ) {
		Cmd_AddCommand( "cm_sharedcompare", CM_SharedCompare_f, "Shared collision against the .bsp's clip brushes: cm_sharedcompare [traces] [seed]" );
		registered = true;
	}
}

/*
=================
brushes built on the spot
=================
*/
typedef struct {
	cbrush_t		brush;
	cbrushside_t	sides[SH_MAX_SIDES];
	cplane_t		planes[SH_MAX_SIDES];
} shBrush_t;

static void CM_SharedAddPlane( shBrush_t *b, const vec3_t normal, float dist, int shaderNum ) {
	if ( b->brush.numsides >= SH_MAX_SIDES ) {
		return;
	}
	cplane_t *p = &b->planes[b->brush.numsides];
	VectorCopy( normal, p->normal );
	p->dist = dist;
	p->type = PlaneTypeForNormal( p->normal );
	SetPlaneSignbits( p );
	cbrushside_t *s = &b->sides[b->brush.numsides];
	s->plane = p;
	s->shaderNum = shaderNum;
	b->brush.numsides++;
}

/*
the brush of a triangle P (world space, facing out): an upright prism when prism, else a slab depth behind it.
-> false when the triangle is degenerate
*/
static bool CM_SharedBuildBrush( shBrush_t *b, const vec3_t P[3], bool prism, float depth, int shaderNum, int contents ) {
	vec3_t	e1, e2, n, pts[6], up = { 0, 0, 1 };
	int		k;

	VectorSubtract( P[1], P[0], e1 );
	VectorSubtract( P[2], P[0], e2 );
	CrossProduct( e1, e2, n );
	if ( VectorNormalize( n ) < 1e-6f ) {
		return false;
	}
	if ( prism && n[2] <= 0.01f ) {
		prism = false;
	}
	for ( k = 0; k < 3; k++ ) {
		VectorCopy( P[k], pts[k] );
	}
	if ( prism ) {
		const float bottom = Q_min( P[0][2], Q_min( P[1][2], P[2][2] ) ) - depth;
		for ( k = 0; k < 3; k++ ) {
			VectorSet( pts[3 + k], P[k][0], P[k][1], bottom );
		}
	} else {
		for ( k = 0; k < 3; k++ ) {
			VectorMA( P[k], -depth, n, pts[3 + k] );
		}
	}

	b->brush.numsides = 0;
	b->brush.sides = b->sides;
	b->brush.contents = contents;
	b->brush.shaderNum = shaderNum;
	b->brush.checkcount = 0;
	ClearBounds( b->brush.bounds[0], b->brush.bounds[1] );
	for ( k = 0; k < 6; k++ ) {
		AddPointToBounds( pts[k], b->brush.bounds[0], b->brush.bounds[1] );
	}
	// the six axial planes first
	for ( int axis = 0; axis < 3; axis++ ) {
		vec3_t a = { 0, 0, 0 };
		a[axis] = -1;
		CM_SharedAddPlane( b, a, -b->brush.bounds[0][axis], shaderNum );
		a[axis] = 1;
		CM_SharedAddPlane( b, a, b->brush.bounds[1][axis], shaderNum );
	}
	// its faces
	CM_SharedAddPlane( b, n, DotProduct( n, P[0] ), shaderNum );
	if ( prism ) {
		vec3_t down = { 0, 0, -1 };
		CM_SharedAddPlane( b, down, -pts[3][2], shaderNum );
	} else {
		vec3_t back;
		VectorNegate( n, back );
		CM_SharedAddPlane( b, back, DotProduct( back, pts[3] ), shaderNum );
	}
	for ( k = 0; k < 3; k++ ) {
		const float *p = P[k], *q = P[( k + 1 ) % 3], *r = P[( k + 2 ) % 3];
		vec3_t e, sn, d;
		VectorSubtract( q, p, e );
		CrossProduct( e, prism ? up : n, sn );
		if ( VectorNormalize( sn ) < 1e-6f ) {
			continue;
		}
		VectorSubtract( r, p, d );
		if ( DotProduct( sn, d ) > 0 ) {
			VectorNegate( sn, sn );
		}
		CM_SharedAddPlane( b, sn, DotProduct( sn, p ), shaderNum );
	}
	// edge bevels: for each edge and axis, the plane through the edge that holds every corner behind it (box traces
	// glide along the edges instead of catching on them, as with q3map2's brushes)
	static const int edges[9][2] = { { 0, 1 }, { 1, 2 }, { 2, 0 }, { 3, 4 }, { 4, 5 }, { 5, 3 }, { 0, 3 }, { 1, 4 }, { 2, 5 } };
	for ( int ei = 0; ei < 9; ei++ ) {
		vec3_t e;
		VectorSubtract( pts[edges[ei][1]], pts[edges[ei][0]], e );
		if ( VectorNormalize( e ) < 1e-6f ) {
			continue;
		}
		for ( int axis = 0; axis < 3; axis++ ) {
			for ( int dir = -1; dir <= 1; dir += 2 ) {
				vec3_t a = { 0, 0, 0 }, bn;
				a[axis] = (float)dir;
				CrossProduct( e, a, bn );
				if ( VectorNormalize( bn ) < 0.01f ) {
					continue;
				}
				const float dist = DotProduct( bn, pts[edges[ei][0]] );
				int c;
				for ( c = 0; c < 6; c++ ) {
					if ( DotProduct( bn, pts[c] ) - dist > 0.05f ) {
						break;
					}
				}
				if ( c < 6 ) {
					continue;			// not a supporting plane on this side
				}
				int s;
				for ( s = 0; s < b->brush.numsides; s++ ) {
					if ( DotProduct( b->planes[s].normal, bn ) > 0.9999f && fabsf( b->planes[s].dist - dist ) < 0.05f ) {
						break;
					}
				}
				if ( s == b->brush.numsides ) {
					CM_SharedAddPlane( b, bn, dist, shaderNum );
				}
			}
		}
	}
	return true;
}

/*
=================
the triangles a box may touch
=================
*/
static inline bool CM_SharedBoxesMeet( const float *amins, const float *amaxs, const vec3pair_t b ) {
	return !( amins[0] > b[1][0] || amins[1] > b[1][1] || amins[2] > b[1][2]
		|| amaxs[0] < b[0][0] || amaxs[1] < b[0][1] || amaxs[2] < b[0][2] );
}

// visit: called with each candidate brush; returns false to stop
typedef bool ( *shVisit_t )( shBrush_t *b, void *ctx );

static bool CM_SharedTile( int ti, const vec3pair_t box, int contentsMask, shVisit_t visit, void *ctx ) {
	const thTile_t *t = &sh.tiles[ti];
	const shMaterial_t *m = &sh.thMats[t->material];
	if ( !( m->contents & contentsMask ) ) {
		return true;
	}
	// the box's corners in grid coordinates: p - origin = i * axisX + j * axisY
	const float det = t->axisX[0] * t->axisY[1] - t->axisX[1] * t->axisY[0];
	if ( fabsf( det ) < 1e-6f ) {
		return true;
	}
	float i0 = 1e30f, i1 = -1e30f, j0 = 1e30f, j1 = -1e30f;
	for ( int c = 0; c < 4; c++ ) {
		const float x = box[c & 1][0] - t->origin[0], y = box[c >> 1][1] - t->origin[1];
		const float i = ( x * t->axisY[1] - y * t->axisY[0] ) / det;
		const float j = ( t->axisX[0] * y - t->axisX[1] * x ) / det;
		i0 = Q_min( i0, i ); i1 = Q_max( i1, i );
		j0 = Q_min( j0, j ); j1 = Q_max( j1, j );
	}
	const int ci0 = Q_max( 0, (int)floorf( i0 ) ), ci1 = Q_min( (int)t->width - 2, (int)floorf( i1 ) );
	const int cj0 = Q_max( 0, (int)floorf( j0 ) ), cj1 = Q_min( (int)t->depth - 2, (int)floorf( j1 ) );
	const float *H = sh.heights + t->firstHeight;
	shBrush_t b;
	for ( int i = ci0; i <= ci1; i++ ) {
		for ( int j = cj0; j <= cj1; j++ ) {
			if ( t->firstHoleByte != 0xFFFFFFFFu ) {
				const uint32_t bit = (uint32_t)i * ( t->depth - 1 ) + j;
				if ( sh.holes[t->firstHoleByte + ( bit >> 3 )] & ( 1 << ( bit & 7 ) ) ) {
					continue;
				}
			}
			vec3_t c[4];			// a (i, j), b (i+1, j), c (i, j+1), e (i+1, j+1)
			for ( int k = 0; k < 4; k++ ) {
				const int ii = i + ( k & 1 ), jj = j + ( k >> 1 );
				c[k][0] = t->origin[0] + ii * t->axisX[0] + jj * t->axisY[0];
				c[k][1] = t->origin[1] + ii * t->axisX[1] + jj * t->axisY[1];
				c[k][2] = H[(size_t)ii * t->depth + jj];
			}
			const float zlo = Q_min( Q_min( c[0][2], c[1][2] ), Q_min( c[2][2], c[3][2] ) ) - t->prismDepth;
			const float zhi = Q_max( Q_max( c[0][2], c[1][2] ), Q_max( c[2][2], c[3][2] ) );
			if ( zlo > box[1][2] || zhi < box[0][2] ) {
				continue;
			}
			// split along b - c: (a, c, b) and (b, c, e); each turned to face up
			const int tri[2][3] = { { 0, 2, 1 }, { 1, 2, 3 } };
			for ( int h = 0; h < 2; h++ ) {
				vec3_t P[3];
				VectorCopy( c[tri[h][0]], P[0] );
				VectorCopy( c[tri[h][1]], P[1] );
				VectorCopy( c[tri[h][2]], P[2] );
				vec3_t e1, e2, nn;
				VectorSubtract( P[1], P[0], e1 );
				VectorSubtract( P[2], P[0], e2 );
				CrossProduct( e1, e2, nn );
				if ( nn[2] < 0 ) {
					vec3_t tmp;
					VectorCopy( P[1], tmp ); VectorCopy( P[2], P[1] ); VectorCopy( tmp, P[2] );
				}
				if ( CM_SharedBuildBrush( &b, P, true, t->prismDepth, m->shaderNum, m->contents )
					&& CM_SharedBoxesMeet( b.brush.bounds[0], b.brush.bounds[1], box ) && !visit( &b, ctx ) ) {
					return false;
				}
			}
		}
	}
	return true;
}

static bool CM_SharedPlacement( int pi, const vec3pair_t box, int contentsMask, shVisit_t visit, void *ctx ) {
	const tcPlacement_t *p = &sh.places[pi];
	const tcShape_t *s = &sh.shapes[p->shape];
	const float *R = p->m;		// row-major 3 x 4
	// the box in shape space: s = R^T (w - o)
	vec3_t c, ext, smin, smax;
	for ( int k = 0; k < 3; k++ ) {
		c[k] = 0.5f * ( box[0][k] + box[1][k] ) - R[4 * k + 3];
		ext[k] = 0.5f * ( box[1][k] - box[0][k] );
	}
	for ( int k = 0; k < 3; k++ ) {
		const float cs = R[k] * c[0] + R[4 + k] * c[1] + R[8 + k] * c[2];
		const float es = fabsf( R[k] ) * ext[0] + fabsf( R[4 + k] ) * ext[1] + fabsf( R[8 + k] ) * ext[2];
		smin[k] = cs - es - 1.0f;
		smax[k] = cs + es + 1.0f;
	}
	const float *V = sh.verts + 3 * (size_t)s->firstVert;
	const int *order = sh.order + sh.trees[p->shape].firstTri;
	int stack[SH_STACK], depth = 0;
	stack[depth++] = sh.trees[p->shape].firstNode;
	shBrush_t b;
	while ( depth ) {
		const shNode_t *n = &sh.nodes[stack[--depth]];
		if ( n->mins[0] > smax[0] || n->mins[1] > smax[1] || n->mins[2] > smax[2]
			|| n->maxs[0] < smin[0] || n->maxs[1] < smin[1] || n->maxs[2] < smin[2] ) {
			continue;
		}
		if ( !n->count ) {
			if ( depth + 2 > SH_STACK ) {
				Com_Error( ERR_DROP, "CM_SharedPlacement: shape %u's tree is deeper than %i", p->shape, SH_STACK / 2 );
			}
			stack[depth++] = n->first;
			stack[depth++] = n->first + 1;
			continue;
		}
		for ( int t = n->first; t < n->first + n->count; t++ ) {
			const tcTri_t *tr = &sh.tris[s->firstTri + order[t]];
			const shMaterial_t *m = &sh.tcMats[tr->material];
			if ( !( m->contents & contentsMask ) ) {
				continue;
			}
			vec3_t P[3];
			for ( int k = 0; k < 3; k++ ) {
				const float *v = V + 3 * tr->v[k];
				for ( int a = 0; a < 3; a++ ) {
					P[k][a] = R[4 * a] * v[0] + R[4 * a + 1] * v[1] + R[4 * a + 2] * v[2] + R[4 * a + 3];
				}
			}
			// (a floor - facing up within 60 degrees, as placed: the Studio's FloorPrisms test - an upright prism,
			//  whatever the placement's tilt; the file's floor flag is the shape's own, before it is turned)
			vec3_t e1, e2, fn;
			VectorSubtract( P[1], P[0], e1 );
			VectorSubtract( P[2], P[0], e2 );
			CrossProduct( e1, e2, fn );
			const bool prism = fn[2] > 0.5f * VectorLength( fn );
			if ( CM_SharedBuildBrush( &b, P, prism, prism ? sh.floorDepth : sh.wallDepth, m->shaderNum, m->contents )
				&& CM_SharedBoxesMeet( b.brush.bounds[0], b.brush.bounds[1], box ) && !visit( &b, ctx ) ) {
				return false;
			}
		}
	}
	return true;
}

static void CM_SharedQuery( const vec3pair_t box, int contentsMask, shVisit_t visit, void *ctx ) {
	if ( !sh.loaded || !sh.cellStart || ( cm_sharedCollision && !cm_sharedCollision->integer ) ) {
		return;
	}
	const int x0 = Com_Clampi( 0, sh.gridW - 1, (int)floorf( ( box[0][0] - sh.gridOrigin[0] ) / sh.cellSize ) );
	const int x1 = Com_Clampi( 0, sh.gridW - 1, (int)floorf( ( box[1][0] - sh.gridOrigin[0] ) / sh.cellSize ) );
	const int y0 = Com_Clampi( 0, sh.gridH - 1, (int)floorf( ( box[0][1] - sh.gridOrigin[1] ) / sh.cellSize ) );
	const int y1 = Com_Clampi( 0, sh.gridH - 1, (int)floorf( ( box[1][1] - sh.gridOrigin[1] ) / sh.cellSize ) );
	if ( box[1][0] < sh.gridOrigin[0] || box[1][1] < sh.gridOrigin[1]
		|| box[0][0] > sh.gridOrigin[0] + sh.gridW * sh.cellSize || box[0][1] > sh.gridOrigin[1] + sh.gridH * sh.cellSize ) {
		return;
	}
	const int thread = Com_JobsThreadIndex();
	int *const stampArr = thread ? sh_threadStamp[thread].stamp : sh.stamp;
	const int stamp = thread ? ++sh_threadStamp[thread].now : ++sh.stampNow;
	for ( int y = y0; y <= y1; y++ ) {
		for ( int x = x0; x <= x1; x++ ) {
			const size_t c = (size_t)y * sh.gridW + x;
			for ( int k = sh.cellStart[c]; k < sh.cellStart[c + 1]; k++ ) {
				const int item = sh.cellItems[k];
				if ( stampArr[item] == stamp ) {
					continue;
				}
				stampArr[item] = stamp;
				const float *mn = item < sh.numTiles ? sh.tiles[item].mins : sh.places[item - sh.numTiles].mins;
				const float *mx = item < sh.numTiles ? sh.tiles[item].maxs : sh.places[item - sh.numTiles].maxs;
				if ( !CM_SharedBoxesMeet( mn, mx, box ) ) {
					continue;
				}
				const bool go = item < sh.numTiles ? CM_SharedTile( item, box, contentsMask, visit, ctx )
					: CM_SharedPlacement( item - sh.numTiles, box, contentsMask, visit, ctx );
				if ( !go ) {
					return;
				}
			}
		}
	}
}

/*
=================
CM_SharedTrace

After the world's own trace (clip handle 0 of the main map): the shared collision's brushes, the same tests.
=================
*/
typedef struct {
	traceWork_t	*tw;
	trace_t		*trace;
	bool		position;
} shTraceCtx_t;

static bool CM_SharedTraceVisit( shBrush_t *b, void *ctx ) {
	shTraceCtx_t *c = (shTraceCtx_t *)ctx;
	if ( c->position ) {
		CM_TestBoxInBrush( c->tw, *c->trace, &b->brush );
	} else {
		CM_TraceThroughBrush( c->tw, *c->trace, &b->brush, false );	// (it copies the plane it hit: nothing points
	}																	//  into this brush once it is gone)
	return !c->trace->allsolid;
}

void CM_SharedTrace( traceWork_t *tw, trace_t &trace, bool position ) {
	if ( !sh.loaded || trace.allsolid ) {
		return;
	}
	shTraceCtx_t c = { tw, &trace, position };
	vec3_t delta;
	VectorSubtract( tw->end, tw->start, delta );
	const float len = VectorLength( delta );
	if ( position || len <= SH_TRACE_PIECE ) {
		CM_SharedQuery( tw->bounds, tw->contents, CM_SharedTraceVisit, &c );
		return;
	}
	// a long trace (a crosshair line, a bot's sight line) walked in pieces from its start: the box round its whole
	// path took in every grid cell under it - thousands of terrain prisms for one line. A piece starting past what
	// has already been hit is not tested
	const int n = (int)ceilf( len / SH_TRACE_PIECE );
	for ( int k = 0; k < n; k++ ) {
		const float f0 = (float)k / n, f1 = (float)( k + 1 ) / n;
		if ( trace.fraction < f0 || trace.allsolid ) {
			break;
		}
		vec3pair_t box;
		for ( int a = 0; a < 3; a++ ) {
			const float p0 = tw->start[a] + delta[a] * f0, p1 = tw->start[a] + delta[a] * f1;
			box[0][a] = Q_min( p0, p1 ) + tw->size[0][a] - 1.0f;
			box[1][a] = Q_max( p0, p1 ) + tw->size[1][a] + 1.0f;
		}
		CM_SharedQuery( box, tw->contents, CM_SharedTraceVisit, &c );
	}
}

/*
=================
CM_SharedPointContents
=================
*/
typedef struct {
	const float	*p;
	int			contents;
} shPointCtx_t;

static bool CM_SharedPointVisit( shBrush_t *b, void *ctx ) {
	shPointCtx_t *c = (shPointCtx_t *)ctx;
	for ( int i = 0; i < b->brush.numsides; i++ ) {
		if ( DotProduct( c->p, b->sides[i].plane->normal ) > b->sides[i].plane->dist ) {
			return true;
		}
	}
	c->contents |= b->brush.contents;
	return true;
}

int CM_SharedPointContents( const vec3_t p ) {
	if ( !sh.loaded ) {
		return 0;
	}
	shPointCtx_t c = { p, 0 };
	vec3pair_t box;
	VectorCopy( p, box[0] );
	VectorCopy( p, box[1] );
	CM_SharedQuery( box, -1, CM_SharedPointVisit, &c );
	return c.contents;
}

/*
=================
cm_sharedcompare [traces] [seed]

Player-sized boxes stood on the floor at random places - over terrain tiles and inside placements' boxes - then
moved: a step down (the floor under them) and a walk in a random direction. Each move traced twice: A = the whole
.bsp without the shared files, B = the .bsp without its collision / floorclip brushes, with the shared files. Where
B goes on farther than A: a hole (walked through, fallen through); where it stops sooner: a wall A has not; standing
inside something in B: stuck. Every difference is printed with its place (setviewpos x y z).
=================
*/
static float CM_SharedRand( unsigned &s ) {
	s = s * 1664525u + 1013904223u;
	return (float)( s >> 8 ) * ( 1.0f / 16777216.0f );
}

static void CM_SharedCompare_f( void ) {
	if ( !sh.loaded || !cm_sharedCollision || !cmg.numShaders ) {
		Com_Printf( "cm_sharedcompare: this map has no shared collision\n" );
		return;
	}
	const int count = Cmd_Argc() > 1 ? Q_max( 1, atoi( Cmd_Argv( 1 ) ) ) : 20000;
	unsigned seed = Cmd_Argc() > 2 ? (unsigned)atoi( Cmd_Argv( 2 ) ) : 1u;
	byte *skip = (byte *)Z_Malloc( cmg.numShaders, TAG_TEMP_WORKSPACE, qtrue );
	int skipShaders = 0, skipBrushes = 0;
	for ( int i = 0; i < cmg.numShaders; i++ ) {
		const char *n = cmg.shaders[i].shader;
		const size_t l = strlen( n );
		if ( l >= 10 && ( !Q_stricmp( n + l - 10, "/collision" ) || !Q_stricmp( n + l - 10, "/floorclip" ) ) ) {
			skip[i] = 1;
			skipShaders++;
		}
	}
	for ( int i = 0; i < cmg.numBrushes; i++ ) {
		const int sn = cmg.brushes[i].shaderNum;
		skipBrushes += ( sn >= 0 && sn < cmg.numShaders && skip[sn] ) ? 1 : 0;
	}
	Com_Printf( "cm_sharedcompare: %i traces, seed %u - %i of %i .bsp brushes left out in B (%i shaders)\n",
		count, seed, skipBrushes, cmg.numBrushes, skipShaders );

	const int savedShared = cm_sharedCollision->integer;
	const int mask = CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BODY | CONTENTS_TERRAIN;
	const vec3_t mins = { -15, -15, -24 }, maxs = { 15, 15, 40 };
	int tried = 0, stood = 0, holesDown = 0, holesWalk = 0, walls = 0, stuck = 0, same = 0, printed = 0;
	auto traceA = [&]( trace_t &t, const vec3_t a, const vec3_t b ) {
		cm_skipBrushShader = NULL;
		cm_sharedCollision->integer = 0;
		CM_BoxTrace( &t, a, b, mins, maxs, 0, mask, 0 );
	};
	auto traceB = [&]( trace_t &t, const vec3_t a, const vec3_t b ) {
		cm_skipBrushShader = skip;
		cm_skipBrushShaderCount = cmg.numShaders;
		cm_sharedCollision->integer = 1;
		CM_BoxTrace( &t, a, b, mins, maxs, 0, mask, 0 );
		cm_skipBrushShader = NULL;
	};
	const char *from = "";
	// (which of the left-out shaders stopped A: the .bsp traced again without each)
	byte *skipF = (byte *)Z_Malloc( cmg.numShaders, TAG_TEMP_WORKSPACE, qtrue );
	byte *skipC = (byte *)Z_Malloc( cmg.numShaders, TAG_TEMP_WORKSPACE, qtrue );
	for ( int i = 0; i < cmg.numShaders; i++ ) {
		const char *n = cmg.shaders[i].shader;
		const size_t l = strlen( n );
		skipF[i] = skip[i] && !Q_stricmp( n + l - 10, "/floorclip" );
		skipC[i] = skip[i] && !Q_stricmp( n + l - 10, "/collision" );
	}
	auto traceAskip = [&]( trace_t &t, const vec3_t a, const vec3_t b, const byte *sk ) {
		cm_skipBrushShader = sk;
		cm_skipBrushShaderCount = cmg.numShaders;
		cm_sharedCollision->integer = 0;
		CM_BoxTrace( &t, a, b, mins, maxs, 0, mask, 0 );
		cm_skipBrushShader = NULL;
	};
	auto report = [&]( const char *what, const vec3_t at, const vec3_t to, float da, float db ) {
		if ( printed++ >= 2000 ) {
			return;
		}
		// what stopped A: floorclip / collision brushes / something else; the shared placements round where it stopped
		trace_t tf, tc;
		traceAskip( tf, at, to, skipF );
		traceAskip( tc, at, to, skipC );
		const char *by = ( tf.fraction > tc.fraction + 0.01f ) ? "floorclip" : ( tc.fraction > tf.fraction + 0.01f ) ? "collision" : "both/other";
		vec3_t hit;
		trace_t ta2;
		traceA( ta2, at, to );
		VectorCopy( ta2.endpos, hit );
		int near = 0, tilted = 0, tile = 0;
		for ( int i = 0; i < sh.numPlacements; i++ ) {
			const tcPlacement_t *pl = &sh.places[i];
			if ( hit[0] >= pl->mins[0] - 32 && hit[0] <= pl->maxs[0] + 32 && hit[1] >= pl->mins[1] - 32 && hit[1] <= pl->maxs[1] + 32
				&& hit[2] >= pl->mins[2] - 64 && hit[2] <= pl->maxs[2] + 64 ) {
				near++;
				tilted += pl->m[10] < 0.999f ? 1 : 0;
			}
		}
		for ( int i = 0; i < sh.numTiles; i++ ) {
			const thTile_t *t = &sh.tiles[i];
			tile += ( hit[0] >= t->mins[0] && hit[0] <= t->maxs[0] && hit[1] >= t->mins[1] && hit[1] <= t->maxs[1] ) ? 1 : 0;
		}
		Com_Printf( "cm_sharedcompare: %s at %.0f %.0f %.0f - .bsp %.0f, shared %.0f (%s; stopped by %s at %.0f %.0f %.0f; "
			"%i placements round it, %i tilted; %i tiles)\n", what, at[0], at[1], at[2], da, db, from, by, hit[0], hit[1], hit[2],
			near, tilted, tile );
	};
	for ( int k = 0; k < count; k++ ) {
		tried++;
		vec3_t s, e;
		const float *mn, *mx;
		from = ( sh.numTiles && ( ( k & 1 ) || !sh.numPlacements ) ) ? "terrain" : "object";
		if ( sh.numTiles && ( ( k & 1 ) || !sh.numPlacements ) ) {
			const thTile_t *t = &sh.tiles[Q_min( sh.numTiles - 1, (int)( CM_SharedRand( seed ) * sh.numTiles ) )];
			mn = t->mins;
			mx = t->maxs;
		} else {
			const tcPlacement_t *p = &sh.places[Q_min( sh.numPlacements - 1, (int)( CM_SharedRand( seed ) * sh.numPlacements ) )];
			mn = p->mins;
			mx = p->maxs;
		}
		for ( int a = 0; a < 3; a++ ) {
			s[a] = mn[a] + ( mx[a] - mn[a] ) * CM_SharedRand( seed );
		}
		s[2] += 64.0f;
		// stood on the floor under it (the .bsp's)
		trace_t t0, ta, tb;
		VectorCopy( s, e );
		e[2] -= 4096.0f;
		traceA( t0, s, e );
		if ( t0.startsolid || t0.allsolid || t0.fraction >= 1.0f || t0.plane.normal[2] < 0.7f ) {
			continue;
		}
		vec3_t at;
		VectorCopy( t0.endpos, at );
		at[2] += 0.5f;
		stood++;
		// standing there in B
		traceB( tb, at, at );
		if ( tb.startsolid ) {
			stuck++;
			// (how far up it is free in B: a floor a little higher, or inside something)
			float lift = -1.0f;
			for ( float dz = 1.0f; dz <= 128.0f; dz *= 2.0f ) {
				vec3_t u;
				VectorCopy( at, u );
				u[2] += dz;
				trace_t tu;
				traceB( tu, u, u );
				if ( !tu.startsolid ) {
					lift = dz;
					break;
				}
			}
			report( "stuck", at, at, 0, lift );
			continue;
		}
		// a step down: the floor under it
		VectorCopy( at, e );
		e[2] -= 64.0f;
		traceA( ta, at, e );
		traceB( tb, at, e );
		const float dA = ta.fraction * 64.0f, dB = tb.fraction * 64.0f;
		if ( dB > dA + 8.0f ) {
			holesDown++;
			report( "no floor", at, e, dA, dB );
			continue;
		}
		// a walk
		const float yaw = CM_SharedRand( seed ) * 2.0f * (float)M_PI, len = 32.0f + 352.0f * CM_SharedRand( seed );
		vec3_t up;
		VectorCopy( at, up );
		up[2] += 18.0f;			// (over a step)
		e[0] = up[0] + cosf( yaw ) * len;
		e[1] = up[1] + sinf( yaw ) * len;
		e[2] = up[2];
		traceA( ta, up, e );
		traceB( tb, up, e );
		if ( ta.startsolid || tb.startsolid ) {
			continue;
		}
		const float wA = ta.fraction * len, wB = tb.fraction * len;
		if ( wB > wA + 8.0f ) {
			holesWalk++;
			report( "walked through", up, e, wA, wB );
		} else if ( wA > wB + 8.0f ) {
			walls++;
			report( "wall", up, e, wA, wB );
		} else {
			same++;
		}
	}
	cm_sharedCollision->integer = savedShared;
	cm_skipBrushShader = NULL;
	Z_Free( skip );
	Z_Free( skipF );
	Z_Free( skipC );
	Com_Printf( "cm_sharedcompare: %i places tried, %i on a floor: %i the same, %i stuck, %i no floor, %i walked through, "
		"%i walls the .bsp has not\n", tried, stood, same, stuck, holesDown, holesWalk, walls );
}
