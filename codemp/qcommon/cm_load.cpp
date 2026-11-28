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

// cmodel.c -- model loading
#include "cm_local.h"
#include "com_profile.h"
#include "q_jobs.h"
#include "qcommon/qfiles.h"

// (a big map's tables pass 2 GB: planes, brush sides - every allocation here takes a 64-bit size)
#define Hunk_Alloc( size, pref ) Hunk_AllocLarge( (size_t)( size ), pref )

#ifdef BSPC

#include "../bspc/l_qfiles.h"

void SetPlaneSignbits (cplane_t *out) {
	int	bits, j;

	// for fast box on planeside test
	bits = 0;
	for (j=0 ; j<3 ; j++) {
		if (out->normal[j] < 0) {
			bits |= 1<<j;
		}
	}
	out->signbits = bits;
}
#endif //BSPC

// to allow boxes to be treated as brush models, we allocate
// some extra indexes along with those needed by the map
#define	BOX_BRUSHES		1
#define	BOX_SIDES		6
#define	BOX_LEAFS		2
#define	BOX_PLANES		12

#define	LL(x) x=LittleLong(x)


clipMap_t	cmg; //rwwRMG - changed from cm
int			c_pointcontents;
int			c_traces, c_brush_traces, c_patch_traces;
comProfile_t	com_prof;
comProfThread_t	com_profThread[JOBS_MAX_THREADS];

comProfCm_t *CM_ProfCounters( void ) {
	const int t = Com_JobsThreadIndex();
	return t ? &com_profThread[t].cm : &com_prof.cm;
}


byte		*cmod_base;

#ifndef BSPC
cvar_t		*cm_noAreas;
cvar_t		*cm_noCurves;
cvar_t		*cm_playerCurveClip;
cvar_t		*cm_extraVerbose;
cvar_t		*cm_accel;
cvar_t		*cm_subBSPTree;
cvar_t		*cm_accelVerify;
#endif

// one temporary box per job thread (index 0 = main thread, the original layout). Written on every
// trace against a non-brush entity, so each thread's box sits on its own cache lines: the brushes
// CM_BOX_BRUSH_STRIDE slots apart in the brush array, planes, sides and model in aligned storage.
struct alignas( 64 ) cmBoxStore_t {
	cmodel_t		model;
	cplane_t		planes[BOX_PLANES];
	cbrushside_t	sides[BOX_SIDES];
};
static cmBoxStore_t	box_store[JOBS_MAX_THREADS];
static cplane_t	*box_planesT[JOBS_MAX_THREADS];
static cbrush_t	*box_brushT[JOBS_MAX_THREADS];



void	CM_InitBoxHull (void);
void	CM_FloodAreaConnections (clipMap_t &cm);

//rwwRMG - added:
clipMap_t	SubBSP[MAX_SUB_BSP];
int			NumSubBSP, TotalSubModels;

/*
===============================================================================

					MAP LOADING

===============================================================================
*/

/*
=================
CMod_LoadShaders
=================
*/
static void CMod_LoadShaders( const lump_t *l, clipMap_t &cm )
{
	dshader_t	*in;
	int			i, count;
	CCMShader	*out;

	in = (dshader_t *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in)) {
		Com_Error (ERR_DROP, "CMod_LoadShaders: funny lump size");
	}
	count = l->filelen / sizeof(*in);

	if (count < 1) {
		Com_Error (ERR_DROP, "Map with no shaders");
	}
	cm.shaders = (CCMShader *)Hunk_Alloc( (1+count) * sizeof( *cm.shaders ), h_high );
	cm.numShaders = count;

	out = cm.shaders;
	for ( i = 0; i < count; i++, in++, out++ )
	{
		Q_strncpyz(out->shader, in->shader, MAX_QPATH);
		out->contentFlags = LittleLong( in->contentFlags );
		out->surfaceFlags = LittleLong( in->surfaceFlags );
	}
}


/*
=================
CMod_LoadSubmodels
=================
*/
static void CMod_LoadSubmodels( const lump_t *l, clipMap_t &cm ) {
	dmodel_t	*in;
	cmodel_t	*out;
	int			i, j, count;
	int			*indexes;

	in = (dmodel_t *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "CMod_LoadSubmodels: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map with no models");
	cm.cmodels = (struct cmodel_s *)Hunk_Alloc( count * sizeof( *cm.cmodels ), h_high );
	cm.numSubModels = count;

	if ( count > MAX_SUBMODELS ) {
		Com_Error( ERR_DROP, "MAX_SUBMODELS exceeded" );
	}

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out = &cm.cmodels[i];

		for (j=0 ; j<3 ; j++)
		{	// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat (in->mins[j]) - 1;
			out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
		}

		//rwwRMG - sof2 doesn't have to add this &cm == &cmg check.
		//Are they getting leaf data elsewhere? (the reason this needs to be done is
		//in sub bsp instances the first brush model isn't necessary a world model and might be
		//real architecture)
		if ( i == 0 && &cm == &cmg ) {
			out->firstNode = 0;
			continue;	// world model doesn't need other info
		}

		// make a "leaf" just to hold the model's brushes and surfaces
		out->firstNode = -1;
		// the world model of a misc_bsp part has a node tree of its own (the part's clipMap): traces use it, see
		// cm_subBSPTree. Its single leaf below is kept, CM_ModelContents reads the brushes from it.
		if ( i == 0 && cm.numLeafs > 0 )
			out->firstNode = 0;

		// make a "leaf" just to hold the model's brushes and surfaces
		out->leaf.numLeafBrushes = LittleLong( in->numBrushes );
		indexes = (int *)Hunk_Alloc( out->leaf.numLeafBrushes * 4, h_high );
		out->leaf.firstLeafBrush = indexes - cm.leafbrushes;
		for ( j = 0 ; j < out->leaf.numLeafBrushes ; j++ ) {
			indexes[j] = LittleLong( in->firstBrush ) + j;
		}

		out->leaf.numLeafSurfaces = LittleLong( in->numSurfaces );
		indexes = (int *)Hunk_Alloc( out->leaf.numLeafSurfaces * 4, h_high );
		out->leaf.firstLeafSurface = indexes - cm.leafsurfaces;
		for ( j = 0 ; j < out->leaf.numLeafSurfaces ; j++ ) {
			indexes[j] = LittleLong( in->firstSurface ) + j;
		}
	}
}


/*
=================
CMod_LoadNodes

=================
*/
static void CMod_LoadNodes( const lump_t *l, clipMap_t &cm ) {
	dnode_t		*in;
	int			child;
	cNode_t		*out;
	int			i, j, count;

	in = (dnode_t *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "CMod_LoadNodes: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map has no nodes");
	cm.nodes = (cNode_t *)Hunk_Alloc( count * sizeof( *cm.nodes ), h_high );
	cm.numNodes = count;

	out = cm.nodes;

	for (i=0 ; i<count ; i++, out++, in++)
	{
		out->plane = cm.planes + LittleLong( in->planeNum );
		for (j=0 ; j<2 ; j++)
		{
			child = LittleLong (in->children[j]);
			out->children[j] = child;
		}
	}

}

/*
=================
CM_BoundBrush

=================
*/
void CM_BoundBrush( cbrush_t *b ) {
	b->bounds[0][0] = -b->sides[0].plane->dist;
	b->bounds[1][0] = b->sides[1].plane->dist;

	b->bounds[0][1] = -b->sides[2].plane->dist;
	b->bounds[1][1] = b->sides[3].plane->dist;

	b->bounds[0][2] = -b->sides[4].plane->dist;
	b->bounds[1][2] = b->sides[5].plane->dist;
}


/*
=================
CMod_LoadBrushes

=================
*/
void CMod_LoadBrushes( const lump_t *l, clipMap_t &cm ) {
	dbrush_t	*in;
	cbrush_t	*out;
	int			i, count;

	in = (dbrush_t *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in)) {
		Com_Error (ERR_DROP, "CMod_LoadBrushes: funny lump size");
	}
	count = l->filelen / sizeof(*in);

	cm.brushes = (cbrush_t *)Hunk_Alloc( ( CM_BOX_BRUSH_SLOTS + count ) * sizeof( *cm.brushes ), h_high );
	cm.numBrushes = count;

	out = cm.brushes;

	for ( i=0 ; i<count ; i++, out++, in++ ) {
		out->sides = cm.brushsides + LittleLong(in->firstSide);
		out->numsides = LittleLong(in->numSides);

		out->shaderNum = LittleLong( in->shaderNum );
		if ( out->shaderNum < 0 || out->shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushes: bad shaderNum: %i", out->shaderNum );
		}
		out->contents = cm.shaders[out->shaderNum].contentFlags;

		CM_BoundBrush( out );
	}

}

/*
=================
CMod_LoadLeafs
=================
*/
static void CMod_LoadLeafs (const lump_t *l, clipMap_t &cm)
{
	int			i;
	cLeaf_t		*out;
	dleaf_t 	*in;
	int			count;

	in = (dleaf_t *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "CMod_LoadLeafs: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map with no leafs");

	cm.leafs = (cLeaf_t *)Hunk_Alloc( ( BOX_LEAFS + count ) * sizeof( *cm.leafs ), h_high );
	cm.numLeafs = count;

	out = cm.leafs;
	for ( i=0 ; i<count ; i++, in++, out++)
	{
		out->cluster = LittleLong (in->cluster);
		out->area = LittleLong (in->area);
		out->firstLeafBrush = LittleLong (in->firstLeafBrush);
		out->numLeafBrushes = LittleLong (in->numLeafBrushes);
		out->firstLeafSurface = LittleLong (in->firstLeafSurface);
		out->numLeafSurfaces = LittleLong (in->numLeafSurfaces);

		if (out->cluster >= cm.numClusters)
			cm.numClusters = out->cluster + 1;
		if (out->area >= cm.numAreas)
			cm.numAreas = out->area + 1;
	}

	cm.areas = (cArea_t *)Hunk_Alloc( cm.numAreas * sizeof( *cm.areas ), h_high );
	cm.areaPortals = (int *)Hunk_Alloc( cm.numAreas * cm.numAreas * sizeof( *cm.areaPortals ), h_high );
}

/*
=================
CMod_LoadPlanes
=================
*/
static void CMod_LoadPlanes (const lump_t *l, clipMap_t &cm)
{
	int			i, j;
	cplane_t	*out;
	dplane_t 	*in;
	int			count;
	int			bits;

	in = (dplane_t *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "CMod_LoadPlanes: funny lump size");
	count = l->filelen / sizeof(*in);

	if (count < 1)
		Com_Error (ERR_DROP, "Map with no planes");
	cm.planes = (struct cplane_s *)Hunk_Alloc( ( BOX_PLANES * JOBS_MAX_THREADS + count ) * sizeof( *cm.planes ), h_high );
	cm.numPlanes = count;

	out = cm.planes;

	for ( i=0 ; i<count ; i++, in++, out++)
	{
		bits = 0;
		for (j=0 ; j<3 ; j++)
		{
			out->normal[j] = LittleFloat (in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1<<j;
		}

		out->dist = LittleFloat (in->dist);
		out->type = PlaneTypeForNormal( out->normal );
		out->signbits = bits;
	}
}

/*
=================
CMod_LoadLeafBrushes
=================
*/
static void CMod_LoadLeafBrushes (const lump_t *l, clipMap_t &cm)
{
	int			i;
	int			*out;
	int		 	*in;
	int			count;

	in = (int *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "CMod_LoadLeafBrushes: funny lump size");
	count = l->filelen / sizeof(*in);

	cm.leafbrushes = (int *)Hunk_Alloc( (count + BOX_BRUSHES * JOBS_MAX_THREADS) * sizeof( *cm.leafbrushes ), h_high );
	cm.numLeafBrushes = count;

	out = cm.leafbrushes;

	for ( i=0 ; i<count ; i++, in++, out++) {
		*out = LittleLong (*in);
	}
}

/*
=================
CMod_LoadLeafSurfaces
=================
*/
static void CMod_LoadLeafSurfaces( const lump_t *l, clipMap_t &cm )
{
	int			i;
	int			*out;
	int		 	*in;
	int			count;

	in = (int *)(cmod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "CMod_LoadLeafSurfaces: funny lump size");
	count = l->filelen / sizeof(*in);

	cm.leafsurfaces = (int *)Hunk_Alloc( count * sizeof( *cm.leafsurfaces ), h_high );
	cm.numLeafSurfaces = count;

	out = cm.leafsurfaces;

	for ( i=0 ; i<count ; i++, in++, out++) {
		*out = LittleLong (*in);
	}
}

/*
=================
CMod_LoadBrushSides
=================
*/
static void CMod_LoadBrushSides (const lump_t *l, clipMap_t &cm)
{
	int				i;
	cbrushside_t	*out;
	dbrushside_t 	*in;
	int				count;
	int				num;

	in = (dbrushside_t *)(cmod_base + l->fileofs);
	if ( l->filelen % sizeof(*in) ) {
		Com_Error (ERR_DROP, "CMod_LoadBrushSides: funny lump size");
	}
	count = l->filelen / sizeof(*in);

	cm.brushsides = (cbrushside_t *)Hunk_Alloc( ( BOX_SIDES * JOBS_MAX_THREADS + count ) * sizeof( *cm.brushsides ), h_high );
	cm.numBrushSides = count;

	out = cm.brushsides;

	for ( i=0 ; i<count ; i++, in++, out++) {
		num = LittleLong( in->planeNum );
		out->plane = &cm.planes[num];
		out->shaderNum = LittleLong( in->shaderNum );
		if ( out->shaderNum < 0 || out->shaderNum >= cm.numShaders ) {
			Com_Error( ERR_DROP, "CMod_LoadBrushSides: bad shaderNum: %i", out->shaderNum );
		}
	}
}


/*
=================
CMod_LoadEntityString
=================
*/
static void CMod_LoadEntityString( const lump_t *l, clipMap_t &cm, const char* name ) {
	fileHandle_t h;
	char entName[MAX_QPATH];

	// Attempt to load entities from an external .ent file if available
	Q_strncpyz(entName, name, sizeof(entName));
	const size_t entNameLen = strlen(entName);
	entName[entNameLen - 3] = 'e';
	entName[entNameLen - 2] = 'n';
	entName[entNameLen - 1] = 't';
	const int iEntityFileLen = FS_FOpenFileRead(entName, &h, qfalse);
	if (h)
	{
		cm.entityString = (char *)Hunk_Alloc(iEntityFileLen + 1, h_high);
		cm.numEntityChars = iEntityFileLen + 1;
		FS_Read(cm.entityString, iEntityFileLen, h);
		FS_FCloseFile(h);
		cm.entityString[iEntityFileLen] = '\0';
		Com_Printf("Loaded entities from %s\n", entName);
		return;
	}

	cm.entityString = (char *)Hunk_Alloc( l->filelen, h_high );
	cm.numEntityChars = l->filelen;
	Com_Memcpy (cm.entityString, cmod_base + l->fileofs, l->filelen);
}

/*
=================
CMod_LoadVisibility
=================
*/
#define	VIS_HEADER	8
static void CMod_LoadVisibility( const lump_t *l, clipMap_t &cm ) {
	int		len;
	byte	*buf;

    len = l->filelen;
	if ( !len ) {
		cm.clusterBytes = ( cm.numClusters + 31 ) & ~31;
		cm.visibility = (unsigned char *)Hunk_Alloc( cm.clusterBytes, h_high );
		Com_Memset( cm.visibility, 255, cm.clusterBytes );
		return;
	}
	buf = cmod_base + l->fileofs;

	cm.vised = qtrue;
	cm.visibility = (unsigned char *)Hunk_Alloc( len, h_high );
	cm.numClusters = LittleLong( ((int *)buf)[0] );
	cm.clusterBytes = LittleLong( ((int *)buf)[1] );
	Com_Memcpy (cm.visibility, buf + VIS_HEADER, len - VIS_HEADER );
}

//==================================================================


/*
=================
CMod_LoadPatches
=================
*/
#define	MAX_PATCH_VERTS		1024
static void CMod_LoadPatches( const lump_t *surfs, const lump_t *verts, clipMap_t &cm ) {
	drawVert_t	*dv, *dv_p;
	dsurface_t	*in;
	int			count;
	int			i, j;
	int			c;
	cPatch_t	*patch;
	vec3_t		points[MAX_PATCH_VERTS];
	int			width, height;
	int			shaderNum;

	in = (dsurface_t *)(cmod_base + surfs->fileofs);
	if (surfs->filelen % sizeof(*in))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");
	cm.numSurfaces = count = surfs->filelen / sizeof(*in);
	cm.surfaces = (cPatch_t ** )Hunk_Alloc( cm.numSurfaces * sizeof( cm.surfaces[0] ), h_high );

	dv = (drawVert_t *)(cmod_base + verts->fileofs);
	if (verts->filelen % sizeof(*dv))
		Com_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size");

	// scan through all the surfaces, but only load patches,
	// not planar faces
	for ( i = 0 ; i < count ; i++, in++ ) {
		if ( LittleLong( in->surfaceType ) != MST_PATCH ) {
			continue;		// ignore other surfaces
		}
		// FIXME: check for non-colliding patches

		cm.surfaces[ i ] = patch = (cPatch_t *)Hunk_Alloc( sizeof( *patch ), h_high );

		// load the full drawverts onto the stack
		width = LittleLong( in->patchWidth );
		height = LittleLong( in->patchHeight );
		c = width * height;
		if ( c > MAX_PATCH_VERTS ) {
			Com_Error( ERR_DROP, "ParseMesh: MAX_PATCH_VERTS" );
		}

		dv_p = dv + LittleLong( in->firstVert );
		for ( j = 0 ; j < c ; j++, dv_p++ ) {
			points[j][0] = LittleFloat( dv_p->xyz[0] );
			points[j][1] = LittleFloat( dv_p->xyz[1] );
			points[j][2] = LittleFloat( dv_p->xyz[2] );
		}

		shaderNum = LittleLong( in->shaderNum );
		patch->contents = cm.shaders[shaderNum].contentFlags;
		patch->surfaceFlags = cm.shaders[shaderNum].surfaceFlags;

		// create the internal facet structure
		patch->pc = CM_GeneratePatchCollide( width, height, points );
	}
}

//==================================================================

/*
==================
CM_LoadMap

Loads in the map and all submodels
==================
*/
void *gpvCachedMapDiskImage = NULL;
char  gsCachedMapDiskImage[MAX_QPATH];
qboolean gbUsingCachedMapDataRightNow = qfalse;	// if true, signifies that you can't delete this at the moment!! (used during z_malloc()-fail recovery attempt)

// called in response to a "devmapbsp blah" or "devmapall blah" command, do NOT use inside CM_Load unless you pass in qtrue
//
// new bool return used to see if anything was freed, used during z_malloc failure re-try
//
qboolean CM_DeleteCachedMap(qboolean bGuaranteedOkToDelete)
{
	qboolean bActuallyFreedSomething = qfalse;

	if (bGuaranteedOkToDelete || !gbUsingCachedMapDataRightNow)
	{
		// dump cached disk image...
		//
		if (gpvCachedMapDiskImage)
		{
			Z_Free(	gpvCachedMapDiskImage );
					gpvCachedMapDiskImage = NULL;

			bActuallyFreedSomething = qtrue;
		}
		gsCachedMapDiskImage[0] = '\0';

		// force map loader to ignore cached internal BSP structures for next level CM_LoadMap() call...
		//
		cmg.name[0] = '\0';
	}

	return bActuallyFreedSomething;
}





static void CM_LoadMap_Actual( const char *name, qboolean clientload, int *checksum, clipMap_t &cm )
{ //rwwRMG - function needs heavy modification
	int				*buf;
	dheader_t		header;
	static unsigned	last_checksum;
	char			origName[MAX_OSPATH];
	void			*newBuff = 0;

	if ( !name || !name[0] ) {
		Com_Error( ERR_DROP, "CM_LoadMap: NULL name" );
	}

#ifndef BSPC
	cm_noAreas = Cvar_Get ("cm_noAreas", "0", CVAR_CHEAT);
	cm_noCurves = Cvar_Get ("cm_noCurves", "0", CVAR_CHEAT);
	cm_playerCurveClip = Cvar_Get ("cm_playerCurveClip", "1", CVAR_ARCHIVE_ND|CVAR_CHEAT );
	cm_extraVerbose = Cvar_Get ("cm_extraVerbose", "0", CVAR_TEMP );
	cm_subBSPTree = Cvar_Get ("cm_subBSPTree", "1", CVAR_TEMP, "Trace misc_bsp parts through their own BSP tree; 0 tests every brush of the part on each trace (the old behaviour)" );
	cm_accel = Cvar_Get ("cm_accel", "1", CVAR_TEMP, "Use the per-leaf brush hierarchy for world traces (results are identical to the linear walk)" );
	cm_accelVerify = Cvar_Get ("cm_accelVerify", "0", CVAR_TEMP, "Run every trace with and without cm_accel and report any difference (slow, debugging)" );
#endif
	Com_DPrintf( "CM_LoadMap( %s, %i )\n", name, clientload );

	if ( !strcmp( cm.name, name ) && clientload ) {
		if ( checksum )
			*checksum = last_checksum;
		return;
	}

	strcpy(origName, name);

	if (&cm == &cmg)
	{
		// free old stuff
		CM_ClearMap();
		CM_ClearLevelPatches();
	}

	// free old stuff
	Com_Memset( &cm, 0, sizeof( cm ) );

	if ( !name[0] ) {
		cm.numLeafs = 1;
		cm.numClusters = 1;
		cm.numAreas = 1;
		cm.cmodels = (struct cmodel_s *)Hunk_Alloc( sizeof( *cm.cmodels ), h_high );
		if ( checksum )
			*checksum = 0;
		return;
	}

	//
	// load the file
	//
	//rww - Doesn't this sort of defeat the purpose? We're clearing it even if the map is the same as the last one!
	//Not touching it though in case I'm just overlooking something.
	if (gpvCachedMapDiskImage && &cm == &cmg)	// MP code: this'll only be NZ if we got an ERR_DROP during last map load,
	{							//	so it's really just a safety measure.
		Z_Free(	gpvCachedMapDiskImage);
				gpvCachedMapDiskImage = NULL;
	}

#ifndef BSPC
	//
	// load the file into a buffer that we either discard as usual at the bottom, or if we've got enough memory
	//	then keep it long enough to save the renderer re-loading it (if not dedicated server),
	//	then discard it after that...
	//
	buf = NULL;
	fileHandle_t h;
	// 64-bit length and read: a big map's .bsp can be over 2 GB (lumps are found by unsigned 32-bit positions, so
	// 4 GB is the format's limit); a .bsp that large can only be a loose file, since the pk3 reader has no zip64
	const int64_t iBSPLen = FS_FOpenFileReadLarge( name, &h );
	if (h)
	{
		if ( iBSPLen < (int64_t)sizeof( dheader_t ) || iBSPLen > 0xffffffffLL ) {
			FS_FCloseFile( h );
			Com_Error( ERR_DROP, "CM_LoadMap: %s is %lld bytes; a .bsp must be under 4 GB", name, (long long)iBSPLen );
		}
		newBuff = Z_MallocLarge( (size_t)iBSPLen, TAG_BSP_DISKIMAGE );
		if ( FS_ReadLarge( newBuff, iBSPLen, h ) != iBSPLen ) {
			FS_FCloseFile( h );
			Z_Free( newBuff );
			Com_Error( ERR_DROP, "CM_LoadMap: could not read all %lld bytes of %s", (long long)iBSPLen, name );
		}
		FS_FCloseFile( h );
		if ( iBSPLen > 0x7fffffffLL ) {
			Com_Printf( "CM_LoadMap: %s is a large .bsp (%.2f GB)\n", name, iBSPLen / 1073741824.0 );
		}

		buf = (int*) newBuff;	// so the rest of the code works as normal
		if (&cm == &cmg)
		{
			gpvCachedMapDiskImage = newBuff;
			newBuff = 0;
		}

		// carry on as before...
		//
	}
#else
	const int iBSPLen = LoadQuakeFile((quakefile_t *) name, (void **)&buf);
#endif

	if ( !buf ) {
		Com_Error (ERR_DROP, "Couldn't load %s", name);
	}

	last_checksum = LittleLong (Com_BlockChecksumLarge (buf, (size_t)iBSPLen));
	if ( checksum )
		*checksum = last_checksum;

	header = *(dheader_t *)buf;
	for (size_t i=0 ; i<sizeof(dheader_t)/4 ; i++) {
		((int *)&header)[i] = LittleLong ( ((int *)&header)[i]);
	}

	if ( header.version != BSP_VERSION ) {
		Z_Free(	gpvCachedMapDiskImage);
				gpvCachedMapDiskImage = NULL;

		Com_Error (ERR_DROP, "CM_LoadMap: %s has wrong version number (%i should be %i)"
		, name, header.version, BSP_VERSION );
	}

	cmod_base = (byte *)buf;

	// load into heap
	CMod_LoadShaders( &header.lumps[LUMP_SHADERS], cm );
	CMod_LoadLeafs (&header.lumps[LUMP_LEAFS], cm);
	CMod_LoadLeafBrushes (&header.lumps[LUMP_LEAFBRUSHES], cm);
	CMod_LoadLeafSurfaces (&header.lumps[LUMP_LEAFSURFACES], cm);
	CMod_LoadPlanes (&header.lumps[LUMP_PLANES], cm);
	CMod_LoadBrushSides (&header.lumps[LUMP_BRUSHSIDES], cm);
	CMod_LoadBrushes (&header.lumps[LUMP_BRUSHES], cm);
	CMod_LoadSubmodels (&header.lumps[LUMP_MODELS], cm);
	CMod_LoadNodes (&header.lumps[LUMP_NODES], cm);
	CMod_LoadEntityString (&header.lumps[LUMP_ENTITIES], cm, name);
	CMod_LoadVisibility( &header.lumps[LUMP_VISIBILITY], cm );
	CMod_LoadPatches( &header.lumps[LUMP_SURFACES], &header.lumps[LUMP_DRAWVERTS], cm );
#ifndef BSPC
	CM_BuildLeafAccel( cm );
#endif

	TotalSubModels += cm.numSubModels;

	if (&cm == &cmg)
	{
		// Load in the shader text - return instantly if already loaded
		CM_InitBoxHull ();
		CM_SharedLoad( name );		// (its shared collision files, when it has them)
	}

#ifndef BSPC	// I hope we can lose this crap soon
	//
	// if we've got enough memory, and it's not a dedicated-server, then keep the loaded map binary around
	//	for the renderer to chew on... (but not if this gets ported to a big-endian machine, because some of the
	//	map data will have been Little-Long'd, but some hasn't).
	//
	// For sub BSPs, always free immediately since caching only applies to main maps
	if ( newBuff && newBuff != gpvCachedMapDiskImage )
	{
		Z_Free(newBuff);
	}
	else if ((Sys_LowPhysicalMemory() && iBSPLen <= 0x7fffffffLL)	// a .bsp of 2 GB or more is kept: the renderer's re-read (FS_ReadFile) is 32-bit
		|| com_dedicated->integer
//		|| we're on a big-endian machine
		)
	{
		Z_Free(	gpvCachedMapDiskImage );
				gpvCachedMapDiskImage = NULL;
	}
	else
	{
		// ... do nothing, and let the renderer free it after it's finished playing with it...
		//
	}
#else
	FS_FreeFile (buf);
#endif

	CM_FloodAreaConnections (cm);

	// allow this to be cached if it is loaded by the server
	if ( !clientload ) {
		Q_strncpyz( cm.name, origName, sizeof( cm.name ) );
	}
}


// need a wrapper function around this because of multiple returns, need to ensure bool is correct...
//
void CM_LoadMap( const char *name, qboolean clientload, int *checksum )
{
	gbUsingCachedMapDataRightNow = qtrue;	// !!!!!!!!!!!!!!!!!!

		CM_LoadMap_Actual( name, clientload, checksum, cmg );

	gbUsingCachedMapDataRightNow = qfalse;	// !!!!!!!!!!!!!!!!!!
}



/*
==================
Per-thread collision scratch for the job system

A job thread cannot use the checkcount fields in the brushes and patches (the main thread and the
other job threads use them at the same time), so each job thread has its own stamp arrays per clip
map. They are allocated before a parallel loop when the loaded maps or the thread count changed,
never while a loop runs.
==================
*/
static cmStamp_t	*cm_threadStamps[JOBS_MAX_THREADS];	// [thread][1 + MAX_SUB_BSP]
static int			cm_threadStampThreads = 0;

static bool CM_StampsMatch( const cmStamp_t *st, const clipMap_t *m ) {
	return st->brushes == m->brushes && st->numBrushes == m->numBrushes + CM_BOX_BRUSH_SLOTS && st->numSurfaces == m->numSurfaces;
}

void CM_ThreadScratchFree( void ) {
	for ( int t = 0; t < JOBS_MAX_THREADS; t++ ) {
		if ( !cm_threadStamps[t] ) {
			continue;
		}
		for ( int m = 0; m < 1 + MAX_SUB_BSP; m++ ) {
			if ( cm_threadStamps[t][m].brush ) Z_Free( cm_threadStamps[t][m].brush );
			if ( cm_threadStamps[t][m].surf ) Z_Free( cm_threadStamps[t][m].surf );
		}
		Z_Free( cm_threadStamps[t] );
		cm_threadStamps[t] = NULL;
	}
	cm_threadStampThreads = 0;
	CM_SharedThreadScratchFree();
}

void CM_ThreadScratchEnsure( int threads ) {
	if ( threads > JOBS_MAX_THREADS ) {
		threads = JOBS_MAX_THREADS;
	}
	bool ok = threads <= cm_threadStampThreads;
	for ( int t = 1; ok && t < threads; t++ ) {
		if ( !CM_StampsMatch( &cm_threadStamps[t][0], &cmg ) ) ok = false;
		for ( int m = 0; ok && m < NumSubBSP; m++ ) {
			if ( !CM_StampsMatch( &cm_threadStamps[t][1 + m], &SubBSP[m] ) ) ok = false;
		}
	}
	if ( ok && !CM_SharedThreadScratchOk( threads ) ) {
		ok = false;
	}
	if ( ok ) {
		return;
	}
	CM_ThreadScratchFree();
	for ( int t = 1; t < threads; t++ ) {
		cm_threadStamps[t] = (cmStamp_t *)Z_Malloc( ( 1 + MAX_SUB_BSP ) * sizeof( cmStamp_t ), TAG_GENERAL, qtrue );
		for ( int m = 0; m < 1 + NumSubBSP; m++ ) {
			const clipMap_t *map = m ? &SubBSP[m - 1] : &cmg;
			cmStamp_t *st = &cm_threadStamps[t][m];
			st->brushes = map->brushes;
			st->numBrushes = map->numBrushes + CM_BOX_BRUSH_SLOTS;
			st->numSurfaces = map->numSurfaces;
			st->brush = (int *)Z_Malloc( st->numBrushes * sizeof( int ), TAG_GENERAL, qtrue );
			st->surf = (int *)Z_Malloc( ( st->numSurfaces + 1 ) * sizeof( int ), TAG_GENERAL, qtrue );
		}
	}
	cm_threadStampThreads = threads;
	CM_SharedThreadScratch( threads );
}

cmStamp_t *CM_ThreadStamps( int thread ) {
	return cm_threadStamps[thread];
}

/*
==================
CM_ClearMap
==================
*/
void CM_ClearMap( void )
{
	int		i;

	CM_ThreadScratchFree();

	Com_Memset( &cmg, 0, sizeof( cmg ) );
	CM_ClearLevelPatches();
	CM_SharedClear();

	for(i = 0; i < NumSubBSP; i++)
	{
		memset(&SubBSP[i], 0, sizeof(SubBSP[0]));
	}
	NumSubBSP = 0;
	TotalSubModels = 0;
}

/*
==================
CM_ClipHandleToModel
==================
*/
cmodel_t	*CM_ClipHandleToModel( clipHandle_t handle, clipMap_t **clipMap ) {
	int		i;
	int		count;

	if ( handle < 0 )
	{
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle );
	}
	if ( handle < cmg.numSubModels )
	{
		if (clipMap)
		{
			*clipMap = &cmg;
		}
		return &cmg.cmodels[handle];
	}
	if ( handle == BOX_MODEL_HANDLE )
	{
		if (clipMap)
		{
			*clipMap = &cmg;
		}
		return &box_store[Com_JobsThreadIndex()].model;
	}

	count = cmg.numSubModels;
	for(i = 0; i < NumSubBSP; i++)
	{
		if (handle < count + SubBSP[i].numSubModels)
		{
			if (clipMap)
			{
				*clipMap = &SubBSP[i];
			}
			return &SubBSP[i].cmodels[handle - count];
		}
		count += SubBSP[i].numSubModels;
	}

	if ( handle < MAX_SUBMODELS )
	{
		Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle (count: %i) < (handle: %i) < (max: %i)",
			cmg.numSubModels, handle, MAX_SUBMODELS );
	}
	Com_Error( ERR_DROP, "CM_ClipHandleToModel: bad handle %i", handle + MAX_SUBMODELS );

	return NULL;
}

/*
==================
CM_InlineModel
==================
*/
clipHandle_t	CM_InlineModel( int index ) {
	if ( index < 0 || index >= TotalSubModels ) {
		Com_Error( ERR_DROP, "CM_InlineModel: bad number: %d >= %d (may need to re-BSP map?)", index, TotalSubModels );
	}
	return index;
}

int		CM_NumInlineModels( void ) {
	return cmg.numSubModels;
}

char	*CM_EntityString( void ) {
	return cmg.entityString;
}

char *CM_SubBSPEntityString( int index )
{
	return SubBSP[index].entityString;
}

int		CM_LeafCluster( int leafnum ) {
	if (leafnum < 0 || leafnum >= cmg.numLeafs) {
		Com_Error (ERR_DROP, "CM_LeafCluster: bad number");
	}
	return cmg.leafs[leafnum].cluster;
}

int		CM_LeafArea( int leafnum ) {
	if ( leafnum < 0 || leafnum >= cmg.numLeafs ) {
		Com_Error (ERR_DROP, "CM_LeafArea: bad number");
	}
	return cmg.leafs[leafnum].area;
}

//=======================================================================


/*
===================
CM_InitBoxHull

Set up the planes and nodes so that the six floats of a bounding box
can just be stored out and get a proper clipping hull structure.
===================
*/
void CM_InitBoxHull (void)
{
	for ( int t = 0; t < JOBS_MAX_THREADS; t++ ) {
	int			i;
	int			side;
	cplane_t	*p;
	cbrushside_t	*s;
	cplane_t	*box_planes;
	cbrush_t	*box_brush;
	cmodel_t	&box_model = box_store[t].model;
	cbrushside_t	*box_sides = t ? box_store[t].sides : cmg.brushsides + cmg.numBrushSides;

	box_planes = box_planesT[t] = t ? box_store[t].planes : &cmg.planes[cmg.numPlanes];

	box_brush = box_brushT[t] = &cmg.brushes[cmg.numBrushes + t * CM_BOX_BRUSH_STRIDE];
	box_brush->numsides = 6;
	box_brush->sides = box_sides;
	box_brush->contents = CONTENTS_BODY;

	memset( &box_model, 0, sizeof( box_model ) );
	box_model.firstNode = -1;
	box_model.leaf.numLeafBrushes = 1;
//	box_model.leaf.firstLeafBrush = cmg.numBrushes;
	box_model.leaf.firstLeafBrush = cmg.numLeafBrushes + t * BOX_BRUSHES;
	cmg.leafbrushes[cmg.numLeafBrushes + t * BOX_BRUSHES] = cmg.numBrushes + t * CM_BOX_BRUSH_STRIDE;

	for (i=0 ; i<6 ; i++)
	{
		side = i&1;

		// brush sides
		s = &box_sides[i];
		s->plane = 	box_planes + (i*2+side);
		s->shaderNum = cmg.numShaders;

		// planes
		p = &box_planes[i*2];
		p->type = i>>1;
		p->signbits = 0;
		VectorClear (p->normal);
		p->normal[i>>1] = 1;

		p = &box_planes[i*2+1];
		p->type = 3 + (i>>1);
		p->signbits = 0;
		VectorClear (p->normal);
		p->normal[i>>1] = -1;

		SetPlaneSignbits( p );
	}
	}
}

/*
===================
CM_TempBoxModel

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
Capsules are handled differently though.
===================
*/
clipHandle_t CM_TempBoxModel( const vec3_t mins, const vec3_t maxs, int capsule ) {
	const int	t = Com_JobsThreadIndex();
	cmodel_t	&box_model = box_store[t].model;
	cplane_t	*box_planes = box_planesT[t];
	cbrush_t	*box_brush = box_brushT[t];

	VectorCopy( mins, box_model.mins );
	VectorCopy( maxs, box_model.maxs );

	if ( capsule ) {
		return CAPSULE_MODEL_HANDLE;
	}

	box_planes[0].dist = maxs[0];
	box_planes[1].dist = -maxs[0];
	box_planes[2].dist = mins[0];
	box_planes[3].dist = -mins[0];
	box_planes[4].dist = maxs[1];
	box_planes[5].dist = -maxs[1];
	box_planes[6].dist = mins[1];
	box_planes[7].dist = -mins[1];
	box_planes[8].dist = maxs[2];
	box_planes[9].dist = -maxs[2];
	box_planes[10].dist = mins[2];
	box_planes[11].dist = -mins[2];

	VectorCopy( mins, box_brush->bounds[0] );
	VectorCopy( maxs, box_brush->bounds[1] );

	return BOX_MODEL_HANDLE;
}

/*
===================
CM_ModelBounds
===================
*/
void CM_ModelBounds( clipHandle_t model, vec3_t mins, vec3_t maxs ) {
	cmodel_t	*cmod;

	cmod = CM_ClipHandleToModel( model );
	VectorCopy( cmod->mins, mins );
	VectorCopy( cmod->maxs, maxs );
}

int CM_LoadSubBSP(const char *name, qboolean clientload)
{
	int		i;
	int		checksum;
	int		count;

	count = cmg.numSubModels;
	for(i = 0; i < NumSubBSP; i++)
	{
		if (!Q_stricmp(name, SubBSP[i].name))
		{
			return count;
		}
		count += SubBSP[i].numSubModels;
	}

	if (NumSubBSP == MAX_SUB_BSP)
	{
		Com_Error (ERR_DROP, "CM_LoadSubBSP: too many unique sub BSPs");
	}

	CM_LoadMap_Actual(name, clientload, &checksum, SubBSP[NumSubBSP] );
	NumSubBSP++;

	return count;
}

int CM_FindSubBSP(int modelIndex)
{
	int		i;
	int		count;

	count = cmg.numSubModels;
	if (modelIndex < count)
	{	// belongs to the main bsp
		return -1;
	}

	for(i = 0; i < NumSubBSP; i++)
	{
		count += SubBSP[i].numSubModels;
		if (modelIndex < count)
		{
			return i;
		}
	}
	return -1;
}

void CM_GetWorldBounds ( vec3_t mins, vec3_t maxs )
{
	VectorCopy ( cmg.cmodels[0].mins, mins );
	VectorCopy ( cmg.cmodels[0].maxs, maxs );
}

int CM_ModelContents_Actual( clipHandle_t model, clipMap_t *cm )
{
	cmodel_t	*cmod;
	int			contents = 0;
	int			i;

	if (!cm)
	{
		cm = &cmg;
	}

	cmod = CM_ClipHandleToModel( model, &cm );

	//MCG ADDED - return the contents, too

	for ( i = 0; i < cmod->leaf.numLeafBrushes; i++ )
	{
		int brushNum = cm->leafbrushes[cmod->leaf.firstLeafBrush + i];
		contents |= cm->brushes[brushNum].contents;
	}

	for ( i = 0; i < cmod->leaf.numLeafSurfaces; i++ )
	{
		int surfaceNum = cm->leafsurfaces[cmod->leaf.firstLeafSurface + i];
		if ( cm->surfaces[surfaceNum] != NULL )
		{//HERNH?  How could we have a null surf within our cmod->leaf.numLeafSurfaces?
			contents |= cm->surfaces[surfaceNum]->contents;
		}
	}

	return contents;
}

int CM_ModelContents(  clipHandle_t model, int subBSPIndex )
{
	if (subBSPIndex < 0)
	{
		return CM_ModelContents_Actual(model, NULL);
	}

	return CM_ModelContents_Actual(model, &SubBSP[subBSPIndex]);
}


#ifndef BSPC
/*
===============================================================================

PER-LEAF BRUSH HIERARCHY

Maps whose collision is made of very many small brushes that do not split the
BSP (clip-brush decompositions of meshes) end up with leaves holding thousands
of brushes. The stock code walks every brush of every leaf a trace touches and
rejects almost all of them with the brush bounds test. This builds, for each
such leaf, a bounding volume hierarchy over the leaf's brush list so a trace
only visits the brushes whose bounds overlap the trace bounds.

The hierarchy only selects candidates. Candidates are then processed in their
original leaf order by the unmodified brush code, and the selection predicate
is the same bounds test that code uses to reject a brush, so trace results are
identical to the linear walk.

===============================================================================
*/

#include <vector>
#include <algorithm>
#include <cfloat>
#include <cmath>

typedef struct accelBuildItem_s {
	float	mins[3], maxs[3];
	float	centre[3];
	int		k;
} accelBuildItem_t;

// brush->bounds come from the first six sides (CM_BoundBrush). They are only a valid
// container for the brush when those sides really are the six axial planes.
static bool CM_AccelBrushBoundsUsable( const cbrush_t *b ) {
	static const float expected[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };

	if ( b->numsides < 6 ) {
		return false;
	}
	for ( int i = 0; i < 6; i++ ) {
		const cplane_t *pl = b->sides[i].plane;
		if ( pl->normal[0] != expected[i][0] || pl->normal[1] != expected[i][1] || pl->normal[2] != expected[i][2] ) {
			return false;
		}
	}
	for ( int i = 0; i < 3; i++ ) {
		if ( !std::isfinite( b->bounds[0][i] ) || !std::isfinite( b->bounds[1][i] ) ) {
			return false;
		}
	}
	return true;
}

static void CM_AccelBuild_r( std::vector<cmAccelNode_t> &nodes, std::vector<int> &items, accelBuildItem_t *it, int n, int self ) {
	float	mins[3], maxs[3], cmins[3], cmaxs[3];

	for ( int a = 0; a < 3; a++ ) {
		mins[a] = cmins[a] = FLT_MAX;
		maxs[a] = cmaxs[a] = -FLT_MAX;
	}
	for ( int i = 0; i < n; i++ ) {
		for ( int a = 0; a < 3; a++ ) {
			if ( it[i].mins[a] < mins[a] ) mins[a] = it[i].mins[a];
			if ( it[i].maxs[a] > maxs[a] ) maxs[a] = it[i].maxs[a];
			if ( it[i].centre[a] < cmins[a] ) cmins[a] = it[i].centre[a];
			if ( it[i].centre[a] > cmaxs[a] ) cmaxs[a] = it[i].centre[a];
		}
	}
	VectorCopy( mins, nodes[self].mins );
	VectorCopy( maxs, nodes[self].maxs );

	if ( n <= CM_ACCEL_NODE_ITEMS ) {
		nodes[self].first = (int)items.size();
		nodes[self].count = n;
		for ( int i = 0; i < n; i++ ) {
			items.push_back( it[i].k );
		}
		return;
	}

	int axis = 0;
	if ( cmaxs[1] - cmins[1] > cmaxs[axis] - cmins[axis] ) axis = 1;
	if ( cmaxs[2] - cmins[2] > cmaxs[axis] - cmins[axis] ) axis = 2;

	const int mid = n / 2;
	std::nth_element( it, it + mid, it + n, [axis]( const accelBuildItem_t &a, const accelBuildItem_t &b ) {
		if ( a.centre[axis] != b.centre[axis] ) {
			return a.centre[axis] < b.centre[axis];
		}
		return a.k < b.k;
	} );

	const int child = (int)nodes.size();
	nodes.resize( nodes.size() + 2 );
	nodes[self].first = child;
	nodes[self].count = 0;

	CM_AccelBuild_r( nodes, items, it, mid, child );
	CM_AccelBuild_r( nodes, items, it + mid, n - mid, child + 1 );
}

void CM_BuildLeafAccel( clipMap_t &cm ) {
	std::vector<cmAccelNode_t>		nodes;
	std::vector<int>				items;
	std::vector<accelBuildItem_t>	work;
	int		accelLeafs = 0, unusable = 0, maxBrushes = 0;
	const int startMs = Sys_Milliseconds();

	cm.numAccelNodes = cm.numAccelItems = 0;
	cm.accelNodes = NULL;
	cm.accelItems = NULL;

	for ( int i = 0; i < cm.numBrushes; i++ ) {
		cm.brushes[i].boundsUsable = CM_AccelBrushBoundsUsable( &cm.brushes[i] ) ? 1 : 0;
	}

	for ( int l = 0; l < cm.numLeafs; l++ ) {
		cLeaf_t *leaf = &cm.leafs[l];

		leaf->accelRoot = 0;
		if ( leaf->numLeafBrushes < CM_ACCEL_MIN_LEAF_BRUSHES ) {
			continue;
		}
		if ( leaf->firstLeafBrush < 0 || leaf->firstLeafBrush + leaf->numLeafBrushes > cm.numLeafBrushes ) {
			continue;	// malformed leaf, leave it to the stock code
		}

		work.resize( leaf->numLeafBrushes );
		for ( int k = 0; k < leaf->numLeafBrushes; k++ ) {
			const int brushnum = cm.leafbrushes[leaf->firstLeafBrush + k];
			accelBuildItem_t &w = work[k];

			w.k = k;
			if ( brushnum < 0 || brushnum >= cm.numBrushes || !cm.brushes[brushnum].boundsUsable ) {
				// always a candidate
				for ( int a = 0; a < 3; a++ ) {
					w.mins[a] = -FLT_MAX;
					w.maxs[a] = FLT_MAX;
					w.centre[a] = 0.0f;
				}
				unusable++;
				continue;
			}
			const cbrush_t *b = &cm.brushes[brushnum];
			for ( int a = 0; a < 3; a++ ) {
				w.mins[a] = b->bounds[0][a];
				w.maxs[a] = b->bounds[1][a];
				w.centre[a] = 0.5f * ( b->bounds[0][a] + b->bounds[1][a] );
			}
		}

		const int root = (int)nodes.size();
		nodes.resize( nodes.size() + 1 );
		CM_AccelBuild_r( nodes, items, work.data(), leaf->numLeafBrushes, root );
		leaf->accelRoot = root + 1;
		accelLeafs++;
		if ( leaf->numLeafBrushes > maxBrushes ) {
			maxBrushes = leaf->numLeafBrushes;
		}
	}

	if ( nodes.empty() ) {
		return;
	}

	cm.numAccelNodes = (int)nodes.size();
	cm.numAccelItems = (int)items.size();
	cm.accelNodes = (cmAccelNode_t *)Hunk_Alloc( cm.numAccelNodes * sizeof( cmAccelNode_t ), h_high );
	cm.accelItems = (int *)Hunk_Alloc( ( cm.numAccelItems + 1 ) * sizeof( int ), h_high );
	memcpy( cm.accelNodes, nodes.data(), cm.numAccelNodes * sizeof( cmAccelNode_t ) );
	if ( cm.numAccelItems ) {
		memcpy( cm.accelItems, items.data(), cm.numAccelItems * sizeof( int ) );
	}

	Com_Printf( "CM_BuildLeafAccel: %i of %i leafs accelerated (largest %i brushes), %i nodes, %i refs, %.1f MB, %i unbounded refs, %i ms\n",
		accelLeafs, cm.numLeafs, maxBrushes, cm.numAccelNodes, cm.numAccelItems,
		( cm.numAccelNodes * sizeof( cmAccelNode_t ) + cm.numAccelItems * sizeof( int ) ) / ( 1024.0 * 1024.0 ),
		unusable, Sys_Milliseconds() - startMs );
}

/*
==================
CM_AccelCollect

Writes to out[] the leaf-list positions of all brushes of the leaf whose bounds overlap
[mins,maxs] (same inclusive test the brush code uses) and, when sweep is given, whose node
is reached by the swept trace volume. Returns the count, or -1 when the
caller should fall back to walking the whole leaf.
==================
*/
int CM_AccelCollect( const clipMap_t *local, const cLeaf_t *leaf, const vec3_t mins, const vec3_t maxs, const cmSweep_t *sweep, int *out, int maxOut, uint64_t *nodeCount ) {
	int		stack[64];
	int		sp = 0;
	int		n = 0;

	stack[sp++] = leaf->accelRoot - 1;
	while ( sp ) {
		const cmAccelNode_t *node = &local->accelNodes[stack[--sp]];

		( *nodeCount )++;

		if ( mins[0] > node->maxs[0] || mins[1] > node->maxs[1] || mins[2] > node->maxs[2]
			|| maxs[0] < node->mins[0] || maxs[1] < node->mins[1] || maxs[2] < node->mins[2] ) {
			continue;
		}
		// long diagonal traces have a huge bounding box; also require the swept volume itself to
		// reach the node (nodes holding an unbounded brush have infinite bounds and always pass)
		if ( sweep && !CM_SweepTouchesBox( sweep, node->mins, node->maxs ) ) {
			continue;
		}
		if ( node->count ) {
			if ( n + node->count > maxOut ) {
				return -1;
			}
			for ( int i = 0; i < node->count; i++ ) {
				out[n++] = local->accelItems[node->first + i];
			}
		} else {
			if ( sp + 2 > (int)ARRAY_LEN( stack ) ) {
				return -1;
			}
			stack[sp++] = node->first + 1;
			stack[sp++] = node->first;
		}
	}
	return n;
}
#endif // !BSPC
