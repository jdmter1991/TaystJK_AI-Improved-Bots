/*
===========================================================================
Game frame profiler ("g_profile <seconds>", 0 = off).

Splits G_RunFrame into zones (client think / Pmove, saber collision, Force powers, missiles,
entity thinks, ...). Zones nest; each one reports its self time, so the zones add up to the
whole game frame. Per zone the report gives the average, the 99th percentile and the maximum
of the per-frame self time, so a category that is cheap on average but spikes is visible.

While off every zone costs one integer test.
===========================================================================
*/
#pragma once

typedef enum {
	GPZ_FRAME,			// G_RunFrame itself: setup, entity loop overhead, per-client upkeep not below
	GPZ_CLIENT,			// ClientThink_real for bots and players: command handling around Pmove
	GPZ_PMOVE,			// Pmove (movement physics, its traces, animation state)
	GPZ_CLEVENTS,		// ClientEvents (footsteps, falling damage, weapon fire)
	GPZ_TRIGGERS,		// G_TouchTriggers
	GPZ_SABERPOS,		// WP_SaberPositionUpdate: blade positions (Ghoul2 bolts), saber state
	GPZ_SABERDMG,		// CheckSaberDamage: saber collision traces (Ghoul2 hit tests) and their effects
	GPZ_SABERBLOCK,		// WP_SaberStartMissileBlockCheck
	GPZ_FORCE,			// WP_ForcePowersUpdate
	GPZ_MISSILE,		// G_RunMissile
	GPZ_ITEM,			// G_RunItem (and physics objects)
	GPZ_MOVER,			// G_RunMover
	GPZ_THINK,			// G_RunThink (entity think functions, logical entities)
	GPZ_ENDFRAME,		// ClientEndFrame for every client
	GPZ_CHECKS,			// rules, votes, score messages, kill queue at the end of the frame
	GPZ_ICARUS,			// ICARUS task manager per client, ROFF
	GPZ_MAX
} gProfZone_t;

extern int gProfEnabled;

void GProf_Push( int zone );
void GProf_Pop( void );
void GProf_FrameBegin( void );	// top of G_RunFrame
void GProf_FrameEnd( void );	// bottom of G_RunFrame

#define GPZ_PUSH( z )	do { if ( gProfEnabled ) GProf_Push( z ); } while ( 0 )
#define GPZ_POP()		do { if ( gProfEnabled ) GProf_Pop(); } while ( 0 )
