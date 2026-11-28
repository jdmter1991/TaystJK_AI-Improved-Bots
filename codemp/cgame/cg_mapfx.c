/*
===========================================================================
cg_mapfx.c -- a map's ambient effects played by the client (maps/<name>.tfx)

A map may ship maps/<name>.tfx beside its .bsp (written by the JKA Mod Studio for SWTOR maps): every ambient effect
it has - steam, fire, sparks, insects - as a position, angles, effect file and interval. They are not game entities
(fx_runner): no entity slot (2,048 in all), nothing in the map's first message to a client (MAX_MSGLEN), no effect
name in the server's table (MAX_FX 64) and no network traffic. Each client plays the ones near its view and in its
PVS, every delay + random ms - what CG_FX does for an fx_runner. The same file gives every player the same effects.

Format (little endian): "TFX1", u32 numFiles, u32 numEffects; numFiles x char[64] effect file names;
numEffects x { float origin[3], float angles[3], u16 file, u16 flags, float delay (ms), float random (ms) } (36 bytes).

cg_mapFx 0 turns them off; cg_mapFxDistance (units) is how near the view an effect has to be to play.
===========================================================================
*/

#include "cg_local.h"

#define MAPFX_MAX_FILES		512
#define MAPFX_MAX			32768
#define MAPFX_RECORD		36

typedef struct {
	vec3_t	origin;
	vec3_t	dir;
	int		fx;			// FX_RegisterEffect's id, 0 = not loaded
	int		next;		// cg.time it plays next
	float	delay, random;
} mapFx_t;

static mapFx_t	s_fx[MAPFX_MAX];
static int		s_numFx;
static vmCvar_t	cg_mapFx, cg_mapFxDistance;

static unsigned int MapFX_U32( const byte *p ) {
	return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ) | ( (unsigned int)p[2] << 16 ) | ( (unsigned int)p[3] << 24 );
}

static float MapFX_F32( const byte *p ) {
	unsigned int u = MapFX_U32( p );
	float f;
	memcpy( &f, &u, 4 );
	return f;
}

/*
=================
CG_MapFX_Load

After the map has loaded: its .tfx, when it has one.
=================
*/
void CG_MapFX_Load( void ) {
	char			name[MAX_QPATH];
	fileHandle_t	f;
	int				len, files[MAPFX_MAX_FILES];
	byte			*buf;
	unsigned int	numFiles, numFx, i;

	s_numFx = 0;
	trap->Cvar_Register( &cg_mapFx, "cg_mapFx", "1", CVAR_ARCHIVE );
	trap->Cvar_Register( &cg_mapFxDistance, "cg_mapFxDistance", "4096", CVAR_ARCHIVE );

	COM_StripExtension( cgs.mapname, name, sizeof( name ) );
	Q_strcat( name, sizeof( name ), ".tfx" );
	len = trap->FS_Open( name, &f, FS_READ );
	if ( len <= 0 || !f ) {
		return;
	}
	if ( len < 12 ) {
		trap->FS_Close( f );
		return;
	}
	buf = (byte *)malloc( len );
	if ( !buf ) {
		trap->FS_Close( f );
		return;
	}
	trap->FS_Read( buf, len, f );
	trap->FS_Close( f );

	numFiles = MapFX_U32( buf + 4 );
	numFx = MapFX_U32( buf + 8 );
	// (the size in 64 bits: a huge count must not wrap round to the file's length)
	if ( memcmp( buf, "TFX1", 4 ) || numFiles > MAPFX_MAX_FILES
		|| (unsigned long long)len != 12ull + (unsigned long long)numFiles * 64ull + (unsigned long long)numFx * MAPFX_RECORD ) {
		Com_Printf( S_COLOR_YELLOW "CG_MapFX_Load: %s is not an effects file this build reads - not used\n", name );
		free( buf );
		return;
	}
	for ( i = 0; i < numFiles; i++ ) {
		char fx[65];
		memcpy( fx, buf + 12 + i * 64, 64 );
		fx[64] = 0;
		files[i] = fx[0] ? trap->FX_RegisterEffect( fx ) : 0;
	}
	if ( numFx > MAPFX_MAX ) {
		numFx = MAPFX_MAX;
	}
	for ( i = 0; i < numFx; i++ ) {
		const byte	*r = buf + 12 + numFiles * 64 + i * MAPFX_RECORD;
		mapFx_t		*e = &s_fx[s_numFx];
		vec3_t		angles;
		unsigned int file = r[24] | ( r[25] << 8 );
		int			k;

		for ( k = 0; k < 3; k++ ) {
			e->origin[k] = MapFX_F32( r + 4 * k );
			angles[k] = MapFX_F32( r + 12 + 4 * k );
		}
		e->fx = file < numFiles ? files[file] : 0;
		if ( !e->fx ) {
			continue;
		}
		AngleVectors( angles, e->dir, NULL, NULL );
		if ( !e->dir[0] && !e->dir[1] && !e->dir[2] ) {
			e->dir[1] = 1;
		}
		e->delay = MapFX_F32( r + 28 );
		e->random = MapFX_F32( r + 32 );
		// (a damaged value - not a number, or beyond an hour - is taken as the shortest: cg.time + it stays an int)
		if ( !( e->delay >= 50.0f && e->delay <= 3600000.0f ) ) {
			e->delay = 50.0f;
		}
		if ( !( e->random >= 0.0f && e->random <= 3600000.0f ) ) {
			e->random = 0.0f;
		}
		e->next = 0;
		s_numFx++;
	}
	free( buf );
	Com_Printf( "CG_MapFX_Load: %s: %i effects, %u effect files\n", name, s_numFx, numFiles );
}

/*
=================
CG_MapFX_Add

Every frame: the map's effects near the view and in its PVS that are due, played.
=================
*/
void CG_MapFX_Add( void ) {
	int		i;
	float	reach;

	if ( !s_numFx || !cg.snap ) {
		return;
	}
	trap->Cvar_Update( &cg_mapFx );
	trap->Cvar_Update( &cg_mapFxDistance );
	if ( !cg_mapFx.integer ) {
		return;
	}
	reach = cg_mapFxDistance.value > 0.0f ? cg_mapFxDistance.value : 4096.0f;
	reach *= reach;
	for ( i = 0; i < s_numFx; i++ ) {
		mapFx_t *e = &s_fx[i];
		if ( e->next > cg.time ) {
			continue;
		}
		if ( DistanceSquared( e->origin, cg.refdef.vieworg ) > reach
			|| !trap->R_InPVS( cg.refdef.vieworg, e->origin, cg.snap->areamask ) ) {
			// (not now: looked at again in a while - spread out, so effects coming into view do not all start at once)
			e->next = cg.time + 250 + ( i & 255 );
			continue;
		}
		trap->FX_PlayEffectID( e->fx, e->origin, e->dir, -1, -1, qfalse );
		e->next = cg.time + (int)( e->delay + Q_flrand( 0.0f, 1.0f ) * e->random );
	}
}
