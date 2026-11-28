/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#pragma once

#include "cm_polylib.h"
#include "com_profile.h"
#include "cm_public.h"
#include "qcommon/qcommon.h"

#define	MAX_SUBMODELS			512
#define	BOX_MODEL_HANDLE		(MAX_SUBMODELS-1)
#define CAPSULE_MODEL_HANDLE	(MAX_SUBMODELS-2)

struct Point
{
	long x, y;
};

typedef struct cNode_s {
	cplane_t	*plane;
	int			children[2];		// negative numbers are leafs
} cNode_t;

typedef struct cLeaf_s {
	int			cluster;
	int			area;

	ptrdiff_t	firstLeafBrush;
	int			numLeafBrushes;

	ptrdiff_t	firstLeafSurface;
	int			numLeafSurfaces;

	int			accelRoot;			// 1 + index into clipMap_t::accelNodes, 0 = no accelerator for this leaf
} cLeaf_t;

// Per-leaf bounding volume hierarchy over the leaf's brush list (see CM_BuildLeafAccel).
// Inner node: count == 0, children are nodes[first] and nodes[first+1].
// Leaf node:  count > 0, items are accelItems[first .. first+count-1]; each item is the
//             position k of a brush inside its BSP leaf's brush list.
typedef struct cmAccelNode_s {
	vec3_t		mins;
	int			first;
	vec3_t		maxs;
	int			count;
} cmAccelNode_t;

#define	CM_ACCEL_MIN_LEAF_BRUSHES	24		// leaves with fewer brushes are walked linearly
#define	CM_ACCEL_NODE_ITEMS			4		// max brushes in a hierarchy leaf node
#define	CM_ACCEL_MAX_CANDIDATES		4096	// more overlapping brushes than this -> linear walk

typedef struct cmodel_s {
	vec3_t		mins, maxs;
	cLeaf_t		leaf;			// submodels don't reference the main tree
	int			firstNode;		// only for cmodel[0] (for the main and bsp instances)
} cmodel_t;

typedef struct cbrushside_s {
	cplane_t	*plane;
	int			shaderNum;
} cbrushside_t;

typedef struct cbrush_s {
	int					shaderNum;		// the shader that determined the contents
	int					contents;
	vec3_t				bounds[2];
	cbrushside_t		*sides;
	unsigned short		numsides;
	unsigned short		boundsUsable;	// bounds[] really contain the brush (first six sides are the axial planes)
	int					checkcount;		// to avoid repeated testings (int: clipMap_t::checkcount is an int and
										// passes 65535 within seconds; a 16-bit copy never matched after that)
} cbrush_t;

class CCMShader
{
public:
	char		shader[MAX_QPATH];
	class CCMShader	*mNext;
	int			surfaceFlags;
	int			contentFlags;

	const char	*GetName(void) const { return(shader); }
	class CCMShader *GetNext(void) const { return(mNext); }
	void	SetNext(class CCMShader *next) { mNext = next; }
	void	Destroy(void) { }
};

typedef struct cPatch_s {
	int			checkcount;				// to avoid repeated testings
	int			surfaceFlags;
	int			contents;
	struct patchCollide_s	*pc;
} cPatch_t;


typedef struct cArea_s {
	int			floodnum;
	int			floodvalid;
} cArea_t;

typedef struct clipMap_s {
	char		name[MAX_QPATH];

	int			numShaders;
	CCMShader	*shaders;

	int			numBrushSides;
	cbrushside_t *brushsides;

	int			numPlanes;
	cplane_t	*planes;

	int			numNodes;
	cNode_t		*nodes;

	int			numLeafs;
	cLeaf_t		*leafs;

	int			numLeafBrushes;
	int			*leafbrushes;

	int			numLeafSurfaces;
	int			*leafsurfaces;

	int			numSubModels;
	cmodel_t	*cmodels;

	int			numBrushes;
	cbrush_t	*brushes;

	int			numClusters;
	int			clusterBytes;
	byte		*visibility;
	qboolean	vised;			// if false, visibility is just a single cluster of ffs

	int			numEntityChars;
	char		*entityString;

	int			numAreas;
	cArea_t		*areas;
	int			*areaPortals;	// [ numAreas*numAreas ] reference counts

	int			numSurfaces;
	cPatch_t	**surfaces;			// non-patches will be NULL

	int			floodvalid;
	int			checkcount;					// incremented on each trace

	int			numAccelNodes;
	cmAccelNode_t *accelNodes;				// per-leaf brush hierarchies, all leaves in one pool
	int			numAccelItems;
	int			*accelItems;
} clipMap_t;


// keep 1/8 unit away to keep the position valid before network snapping
// and to avoid various numeric issues
#define	SURFACE_CLIP_EPSILON	(0.125)

extern	clipMap_t	cmg; //rwwRMG - changed from cm
extern	int			c_pointcontents;
extern	int			c_traces, c_brush_traces, c_patch_traces;
extern	cvar_t		*cm_noAreas;
extern	cvar_t		*cm_noCurves;
extern	cvar_t		*cm_playerCurveClip;
extern	cvar_t		*cm_extraVerbose;
extern	cvar_t		*cm_accel;
extern	cvar_t		*cm_subBSPTree;
extern	cvar_t		*cm_accelVerify;

// The moving trace volume as a segment plus half-extents, for swept culling against boxes.
typedef struct cmSweep_s {
	int		use;			// 0 for position tests / when disabled
	float	start[3];
	float	delta[3];		// end - start
	float	invDelta[3];	// 1/delta, unused where delta is 0
	float	ext[3];			// half-size of the trace volume plus a safety margin
} cmSweep_t;

#define	CM_SWEEP_MARGIN		1.0f	// > SURFACE_CLIP_EPSILON; brushes are effectively that much larger


// true when the swept volume can touch the box [mins,maxs]
static inline bool CM_SweepTouchesBox( const cmSweep_t *sw, const float *mins, const float *maxs ) {
	float tmin = 0.0f, tmax = 1.0f;

	for ( int a = 0; a < 3; a++ ) {
		const float lo = mins[a] - sw->ext[a] - sw->start[a];
		const float hi = maxs[a] + sw->ext[a] - sw->start[a];

		if ( sw->delta[a] == 0.0f ) {
			if ( lo > 0.0f || hi < 0.0f ) {
				return false;
			}
			continue;
		}
		float t0 = lo * sw->invDelta[a];
		float t1 = hi * sw->invDelta[a];
		if ( t0 > t1 ) {
			const float t = t0; t0 = t1; t1 = t;
		}
		if ( t0 > tmin ) tmin = t0;
		if ( t1 < tmax ) tmax = t1;
		if ( tmin > tmax ) {
			return false;
		}
	}
	return true;
}

void	CM_BuildLeafAccel( clipMap_t &cm );
int		CM_AccelCollect( const clipMap_t *local, const cLeaf_t *leaf, const vec3_t mins, const vec3_t maxs, const cmSweep_t *sweep, int *out, int maxOut, uint64_t *nodeCount );

// cm_test.c

// Used for oriented capsule collision detection
typedef struct sphere_s {
	qboolean	use;
	float		radius;
	float		halfheight;
	vec3_t		offset;
} sphere_t;

// "already tested in this trace" stamps of one job thread for one clip map (the main thread uses
// the checkcount fields in the brushes and patches themselves)
typedef struct cmStamp_s {
	int			count;
	int			numBrushes;
	int			numSurfaces;
	int			*brush;
	int			*surf;
	const cbrush_t *brushes;	// what the arrays were made for
} cmStamp_t;

typedef struct traceWork_s { //rwwRMG - modified
	vec3_t		start;
	vec3_t		end;
	vec3_t		size[2];	// size of the box being swept through the model
	vec3_t		offsets[8];	// [signbits][x] = either size[0][x] or size[1][x]
	float		maxOffset;	// longest corner length from origin
	vec3_t		extents;	// greatest of abs(size[0]) and abs(size[1])
	vec3_t		modelOrigin;// origin of the model tracing through
	int			contents;	// ored contents of the model tracing through
	qboolean	isPoint;	// optimized case
//	trace_t		trace;		// returned from trace call
	sphere_t	sphere;		// sphere for oriendted capsule collision

	//rwwRMG - added:
	vec3pair_t		bounds;			// enclosing box of start and end surrounding by size
	vec3pair_t		localBounds;	// enclosing box of start and end surrounding by size for a segment

	float			baseEnterFrac;	// global enter fraction (before processing subsections of the brush)
	float			baseLeaveFrac;	// global leave fraction (before processing subsections of the brush)
	float			enterFrac;		// fraction where the ray enters the brush
	float			leaveFrac;		// fraction where the ray leaves the brush
	cbrushside_t	*leadside;
	cplane_t		*clipplane;
	bool			startout;
	bool			getout;

	// per-trace state that used to be global (so that job threads can trace at the same time)
	int				thread;			// job thread index, 0 = main thread
	cmStamp_t		*stamps;		// [1 + MAX_SUB_BSP] stamps of a job thread, NULL on the main thread
	comProfCm_t		*prof;			// collision counters of this thread
	int				accelActive;	// leaf walks may use the per-leaf brush hierarchy
	cmSweep_t		sweep;			// moving volume for swept culling (accelerated path only)
} traceWork_t;

extern clipMap_t	SubBSP[MAX_SUB_BSP];

static inline cmStamp_t *CM_StampFor( const traceWork_t *tw, const clipMap_t *local ) {
	return tw->stamps ? &tw->stamps[ local == &cmg ? 0 : 1 + (int)( local - SubBSP ) ] : NULL;
}

// true when this brush was already tested in this trace; marks it tested otherwise
static inline bool CM_BrushSeen( cmStamp_t *st, clipMap_t *local, int brushnum, cbrush_t *b ) {
	if ( !st ) {
		if ( b->checkcount == local->checkcount ) {
			return true;
		}
		b->checkcount = local->checkcount;
		return false;
	}
	if ( st->brush[brushnum] == st->count ) {
		return true;
	}
	st->brush[brushnum] = st->count;
	return false;
}

static inline bool CM_PatchSeen( cmStamp_t *st, clipMap_t *local, int surfnum, cPatch_t *patch ) {
	if ( !st ) {
		if ( patch->checkcount == local->checkcount ) {
			return true;
		}
		patch->checkcount = local->checkcount;
		return false;
	}
	if ( st->surf[surfnum] == st->count ) {
		return true;
	}
	st->surf[surfnum] = st->count;
	return false;
}

// starts a new "already tested" generation for this trace
static inline void CM_NewCheck( traceWork_t *tw, clipMap_t *local ) {
	cmStamp_t *st = CM_StampFor( tw, local );
	if ( st ) {
		st->count++;
	} else {
		local->checkcount++;
	}
}

// job threads: stamps for the clip maps loaded now; NULL when CM_ThreadScratchEnsure did not run
cmStamp_t	*CM_ThreadStamps( int thread );

typedef struct leafList_s {
	int		count;
	int		maxcount;
	qboolean	overflowed;
	int		*list;
	vec3_t	bounds[2];
	int		lastLeaf;		// for overflows where each leaf can't be stored individually
	void	(*storeLeafs)( struct leafList_s *ll, int nodenum );
} leafList_t;

void CM_StoreLeafs( leafList_t *ll, int nodenum );
void CM_StoreBrushes( leafList_t *ll, int nodenum );

void CM_BoxLeafnums_r( leafList_t *ll, int nodenum );

cmodel_t	*CM_ClipHandleToModel( clipHandle_t handle, clipMap_t **clipMap = 0 );

// cm_patch.c

struct patchCollide_s	*CM_GeneratePatchCollide( int width, int height, vec3_t *points );
void CM_TraceThroughPatchCollide( traceWork_t *tw, trace_t &trace, const struct patchCollide_s *pc );
qboolean CM_PositionTestInPatchCollide( traceWork_t *tw, const struct patchCollide_s *pc );
void CM_ClearLevelPatches( void );

// cm_shader.cpp
void CM_SetupShaderProperties( void );
void CM_ShutdownShaderProperties(void);
CCMShader *CM_GetShaderInfo( const char *name );
CCMShader *CM_GetShaderInfo( int shaderNum );
void		CM_GetModelFormalName ( const char* model, const char* skin, char* name, int size );

// cm_load.cpp
void CM_GetWorldBounds ( vec3_t mins, vec3_t maxs );
