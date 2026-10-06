/*
shots.c - fixed-camera screenshots of maps, for comparing renderers
Copyright (C) 2026 18woldemar

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
shots <file>

The file lists shots, one per line: <name> <map> <x> <y> <z> <pitch> <yaw> <roll> [via <landmark>]
[cmd "<command>"]. For each shot the map is loaded (unless it is the current one), the player is moved to the camera so the server sends what the camera
sees, the game is paused, and the view is forced to the camera. Then "shots: ready <name>" goes to the
log and the picture stays for shots_hold seconds, long enough for tools/shots to grab the window.
The run quits after the last shot.

Both builds must show the same moment: host_framerate fixes the time step, so a fixed number of frames after
the map starts gives the same game time, light style phase and texture animation frame on each. HUD, view
model, overlays and everything but the world and brush entities are hidden, unless shots_models is set:
then models, sprites, effects and the view model stay. The optional command runs when the player is placed
(e.g. cmd "impulse 101; weapon_crowbar"), and the shot waits three seconds longer for it.
With via, the map is entered the way a level transition enters it: the player moves to the named landmark of
the current map and changelevel2 carries the level state over (tools/tour.txt plays through chapters).
The -randomseed command line option fixes the random tiling of '-' textures, which the renderer picks at start.
*/

#include "common.h"

#if !XASH_DEDICATED && !defined( MASTER )

#include "client.h"
#include "server.h"

#define SHOTS_MAX          256
#define SHOTS_START_FRAMES 60 // after the map is active
#define SHOTS_PLACE_FRAMES 30 // after the player is moved
#define SHOTS_PAUSE_FRAMES 10 // after the pause arrived
#define SHOTS_CMD_FRAMES   150 // more after a command: client commands go out on real time, and weapons deploy

typedef struct
{
	char   name[64];
	char   map[MAX_QPATH];
	vec3_t origin;
	vec3_t angles;
	char   via[64];
	char   cmd[256];
} shot_t;

typedef enum
{
	SHOTS_IDLE = 0,
	SHOTS_LOAD,
	SHOTS_START,
	SHOTS_PLACE,
	SHOTS_PAUSE,
	SHOTS_HOLD,
} shotstate_t;

static CVAR_DEFINE_AUTO( shots_hold, "4", 0, "seconds each shots picture stays on screen" );
static CVAR_DEFINE_AUTO( shots_models, "0", 0, "shots keeps models, sprites, effects and the view model" );
static CVAR_DEFINE_AUTO( shots_quit, "1", 0, "shots leaves the game at the end; 0 returns to the menu" );

static struct
{
	shot_t     *shots;
	int         count;
	int         current;
	shotstate_t state;
	int         frames;
	double      hold_start;
	double      frame_last, frame_sum, frame_max;	// the held picture, for the per-map frame cost
	int         frame_count;
} shots;

// hidden while shots run, restored at the end so config.cfg keeps the user's values
static struct
{
	const char *name;
	const char *value;
	qboolean    world_only; // only when models are hidden
	char        saved[64];
} shots_cvars[] =
{
	{ "host_framerate", "0.02" },
	{ "hud_draw", "0" },
	{ "crosshair", "0" },
	{ "r_drawviewmodel", "0", true },
	{ "r_drawparticles", "0", true },
	{ "r_drawbeams", "0", true },
	{ "r_drawtracers", "0", true },
	{ "scr_drawversion", "0" },
	{ "con_notifytime", "-1" }, // lines printed while paused have the current time
	{ "showpause", "0" },
	{ "v_dark", "0" },
};

static void Shots_SetCvars( qboolean enable )
{
	for( int i = 0; i < ARRAYSIZE( shots_cvars ); i++ )
	{
		if( shots_cvars[i].world_only && shots_models.value )
			continue;

		if( enable )
		{
			Q_strncpy( shots_cvars[i].saved, Cvar_VariableString( shots_cvars[i].name ), sizeof( shots_cvars[i].saved ));
			Cvar_Set( shots_cvars[i].name, shots_cvars[i].value );
		}
		else Cvar_Set( shots_cvars[i].name, shots_cvars[i].saved );
	}
}

static qboolean Shots_Load( const char *filename )
{
	char token[MAX_TOKEN];
	byte *data = FS_LoadFile( filename, NULL, false );
	char *p = (char *)data;

	if( !data )
	{
		Con_Printf( S_ERROR "shots: can't load %s\n", filename );
		return false;
	}

	shots.shots = (shot_t *)Mem_Calloc( host.mempool, sizeof( *shots.shots ) * SHOTS_MAX );
	shots.count = 0;

	while(( p = COM_ParseFile( p, token, sizeof( token ))) != NULL )
	{
		shot_t *s = &shots.shots[shots.count];
		int i;

		Q_strncpy( s->name, token, sizeof( s->name ));
		p = COM_ParseFile( p, s->map, sizeof( s->map ));

		for( i = 0; i < 6 && p; i++ )
		{
			p = COM_ParseFile( p, token, sizeof( token ));
			if( i < 3 ) s->origin[i] = Q_atof( token );
			else s->angles[i - 3] = Q_atof( token );
		}

		if( i < 6 || !p )
		{
			Con_Printf( S_ERROR "shots: %s: shot %s is incomplete\n", filename, s->name );
			break;
		}

		// optional: via <landmark>, cmd "..."
		for( ;; )
		{
			char *next = COM_ParseFile( p, token, sizeof( token ));

			if( next && !Q_strcmp( token, "via" ))
				p = COM_ParseFile( next, s->via, sizeof( s->via ));
			else if( next && !Q_strcmp( token, "cmd" ))
				p = COM_ParseFile( next, s->cmd, sizeof( s->cmd ));
			else break;
		}

		if( ++shots.count == SHOTS_MAX )
			break;
	}

	Mem_Free( data );
	if( shots.count > 0 )
		return true;

	Mem_Free( shots.shots );
	shots.shots = NULL;
	return false;
}

static void Shots_Finish( void )
{
	Con_Printf( "shots: done\n" );
	Shots_SetCvars( false );
	Mem_Free( shots.shots );
	memset( &shots, 0, sizeof( shots ));
	Cbuf_AddText( shots_quit.value ? "quit\n" : "disconnect\n" );
}

static void Shots_Next( void )
{
	if( ++shots.current >= shots.count )
	{
		Shots_Finish();
		return;
	}

	shots.state = Q_stricmp( sv.name, shots.shots[shots.current].map ) || !SV_Active() ? SHOTS_LOAD : SHOTS_PLACE;
	shots.frames = 0;
}

// the server sends entities by the player's view, so the player goes where the camera is
static void Shots_PlacePlayer( const vec3_t eye, const vec3_t angles )
{
	edict_t *ed = svs.clients[0].edict;

	VectorSubtract( eye, ed->v.view_ofs, ed->v.origin );
	VectorClear( ed->v.velocity );
	VectorCopy( angles, ed->v.angles );
	VectorCopy( angles, ed->v.v_angle );
	ed->v.fixangle = 1;
	ed->v.movetype = MOVETYPE_NOCLIP;
	ed->v.solid = SOLID_NOT; // no triggers: a changelevel would end the run
	ed->v.flags |= FL_GODMODE | FL_NOTARGET;
	SV_LinkEdict( ed, false );
}

// the player keeps its offset from the landmark across the transition: standing on it, the player arrives
// inside the new map rather than wherever the last camera's offset leads
static qboolean Shots_ChangeLevel( const shot_t *s )
{
	edict_t *player = svs.clients[0].edict;

	for( int e = 1; e < svgame.numEntities; e++ )
	{
		edict_t *ed = SV_EdictNum( e );
		vec3_t eye;

		if( !SV_IsValidEdict( ed ) || Q_strcmp( SV_GetString( ed->v.classname ), "info_landmark" ) || Q_strcmp( SV_GetString( ed->v.targetname ), s->via ))
			continue;

		VectorAdd( ed->v.origin, player->v.view_ofs, eye );
		Shots_PlacePlayer( eye, player->v.v_angle );
		Cbuf_AddText( va( "changelevel2 %s %s\n", s->map, s->via ));
		return true;
	}

	Con_Printf( S_ERROR "shots: %s: no landmark %s in %s\n", s->name, s->via, sv.name );
	return false;
}

void Shots_Frame( void )
{
	const shot_t *s;

	if( shots.state == SHOTS_IDLE )
		return;

	s = &shots.shots[shots.current];

	if( shots.state > SHOTS_START && Q_stricmp( sv.name, s->map ))
	{
		Con_Printf( S_ERROR "shots: %s left the map for %s\n", s->name, sv.name );
		shots.state = SHOTS_LOAD;
	}

	switch( shots.state )
	{
	case SHOTS_LOAD:
		if( !s->via[0] || !SV_Active() || !Shots_ChangeLevel( s ))
			Cbuf_AddText( va( "map %s\n", s->map ));
		shots.state = SHOTS_START;
		shots.frames = 0;
		break;
	case SHOTS_START:
		// the old map may still be up for a frame or two after the map command
		if( cls.state != ca_active || cls.signon != SIGNONS || !cl.video_prepped || Q_stricmp( sv.name, s->map ))
		{
			shots.frames = 0;
			break;
		}

		if( shots.frames == 0 )
		{
			UI_SetActiveMenu( false );
			Key_SetKeyDest( key_game );
		}

		if( ++shots.frames >= SHOTS_START_FRAMES )
		{
			shots.state = SHOTS_PLACE;
			shots.frames = 0;
		}
		break;
	case SHOTS_PLACE:
		if( shots.frames++ == 0 )
		{
			Shots_PlacePlayer( s->origin, s->angles );
			if( s->cmd[0] )
				Cbuf_AddText( va( "%s\n", s->cmd ));
		}

		if( shots.frames >= SHOTS_PLACE_FRAMES + ( s->cmd[0] ? SHOTS_CMD_FRAMES : 0 ))
		{
			SV_TogglePause( NULL );
			shots.state = SHOTS_PAUSE;
			shots.frames = 0;
		}
		break;
	case SHOTS_PAUSE:
		// a fade frozen halfway would darken the picture
		memset( &clgame.fade, 0, sizeof( clgame.fade ));

		if( cl.paused && ++shots.frames >= SHOTS_PAUSE_FRAMES )
		{
			Con_Printf( "shots: ready %s time %.3f\n", s->name, cl.time );
			shots.state = SHOTS_HOLD;
			shots.hold_start = shots.frame_last = host.realtime;
			shots.frame_sum = shots.frame_max = 0.0;
			shots.frame_count = 0;
		}
		break;
	case SHOTS_HOLD:
	{
		double frame = host.realtime - shots.frame_last;

		shots.frame_last = host.realtime;
		if( shots.frame_count++ > 0 ) // the first frame carries the load before it
		{
			shots.frame_sum += frame;
			if( frame > shots.frame_max )
				shots.frame_max = frame;
		}

		if( host.realtime - shots.hold_start >= shots_hold.value )
		{
			if( shots.frame_count > 1 )
			{
				Con_Printf( "shots: %s %d frames, avg %.1f ms, max %.1f ms\n", s->name,
					shots.frame_count - 1, shots.frame_sum / ( shots.frame_count - 1 ) * 1000.0,
					shots.frame_max * 1000.0 );
			}
			SV_TogglePause( NULL );
			Shots_Next();
		}
		break;
	}
	default:
		break;
	}
}

// true while shots hide everything but the world and brush entities
qboolean Shots_Active( void )
{
	return shots.state != SHOTS_IDLE && !shots_models.value;
}

qboolean Shots_View( float *origin, float *angles )
{
	const shot_t *s;

	if( shots.state != SHOTS_PAUSE && shots.state != SHOTS_HOLD )
		return false;

	s = &shots.shots[shots.current];
	VectorCopy( s->origin, origin );
	VectorCopy( s->angles, angles );
	return true;
}

static void Shots_f( void )
{
	if( Cmd_Argc() != 2 )
	{
		Con_Printf( S_USAGE "shots <file>\n" );
		return;
	}

	if( shots.state != SHOTS_IDLE )
	{
		Con_Printf( "shots: already running\n" );
		return;
	}

	if( !Shots_Load( Cmd_Argv( 1 )))
		return;

	Con_Printf( "shots: %d shots\n", shots.count );
	Shots_SetCvars( true );
	shots.current = -1;
	shots.state = SHOTS_LOAD; // anything but idle, Shots_Next decides
	Shots_Next();
}

void Shots_Init( void )
{
	Cvar_RegisterVariable( &shots_hold );
	Cvar_RegisterVariable( &shots_models );
	Cvar_RegisterVariable( &shots_quit );
	Cmd_AddRestrictedCommand( "shots", Shots_f, "show and log fixed camera views listed in a file (for comparing renderers)" );
}

#else

void Shots_Init( void )
{
}

void Shots_Frame( void )
{
}

qboolean Shots_Active( void )
{
	return false;
}

qboolean Shots_View( float *origin, float *angles )
{
	return false;
}

#endif
