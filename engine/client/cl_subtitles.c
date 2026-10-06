/*
cl_subtitles.c - captions for the game's speech
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

// Half-Life never had subtitles for its speech. The text comes from Fograin/hl-subs-mod through
// tools/mksubs, which writes subtitles.txt in the syntax of titles.txt, so the file is read with the
// parser the engine already has. A caption starts where a sentence starts, in VOX_LoadSound, and lives as
// long as that channel keeps the sentence: when the speaker is killed, the level changes or a save is
// loaded the channel goes and the caption goes with it, with no bookkeeping of its own.
// The look follows Half-Life 2's closed captions - a dark panel low on the screen that grows to the lines
// it holds, a caption appearing as a whole phrase - in Half-Life's own colours.

#include "common.h"
#include "client.h"
#include "sound.h"

#define SUB_MAX_ITEMS     3      // captions on screen at once; a fourth pushes the oldest out
#define SUB_MAX_LINES     9      // wrapped lines one caption may take: the longest line the game
                                 // speaks is 256 characters, which takes nine of them at the largest size
#define SUB_LINE_MAX      256
#define SUB_HIDDEN        0.2    // the panel grows first and the text follows, as Half-Life 2 does
#define SUB_FADE_IN       0.15
#define SUB_FADE_OUT      0.3
#define SUB_LINGER        1.0    // how long a caption stays after its sentence stopped
#define SUB_GROW_TIME     0.25   // the panel's height follows the lines this slowly
#define SUB_SAFE_AREA     0.05f  // the edges a TV may cut off
#define SUB_WIDTH_FRAC    0.59f  // Half-Life 2's panel is 500 of its 640 wide units, 750 px at 720p
#define SUB_PADDING       8
#define SUB_PANEL_ALPHA   128    // Half-Life 2's BgAlpha
#define SUB_HUD_ROOM      0.12f  // the bottom of the screen, where the HUD keeps its numbers
#define SUB_LINE_BUDGET   9      // lines the panel may hold before the oldest caption is pushed out
#define SUB_NOREPEAT      4.0    // how long a group of lines waits before it may caption again
#define SUB_REPEATS       16     // groups remembered for that
#define SUB_CLOCK_BACK    1.0    // a backwards step of the client's clock that means a new map, not interpolation

typedef struct
{
	const client_textmessage_t *msg;
	const channel_t            *ch;       // the sound that started this caption
	const sfx_t                *sfx;      // set for a lone sound: a sentence changes the channel's sfx per word
	char                        chname[16]; // how the channel spells the sentence, to notice a channel reused
	char                        key[64];  // the name of the part on screen, to find the part after it
	double                      start;    // when the caption appeared
	double                      partstart; // when the part on screen appeared
	double                      finished; // when the sentence stopped, 0 while it still plays
	qboolean                    heard;    // the channel lived at least one frame, so the line is audible
	rgba_t                      color;
} subtitle_t;

static client_textmessage_t *cl_captions;
static int                   cl_numcaptions;
static int                  *cl_caption_next;      // the caption after this one in its bucket, or -1
static int                   cl_caption_bucket[256];
static subtitle_t            cl_subs[SUB_MAX_ITEMS];
static cl_font_t             cl_subfont;
static float                 cl_subfont_scale;
static int                   cl_numsubs;
static float                 cl_panel_height;

// Half-Life's talking monsters pick a random line out of a group whenever they idle, so two scientists in
// a room caption each other over and over. Source answers that with cc_sentencecaptionnorepeat, keyed by
// the group's name without the number at its end (SC_QUESTION20 -> SC_QUESTION), and so do we.
static struct
{
	char   group[32];
	double time;
} cl_subrepeats[SUB_REPEATS];

CVAR_DEFINE_AUTO( cl_subtitles, "0", FCVAR_ARCHIVE, "show captions for the game's speech" );
// how large the captions are depends on the screen and how far away it is, so it is a knob and not a constant
CVAR_DEFINE_AUTO( cl_subtitles_scale, "1.25", FCVAR_ARCHIVE, "size of the captions, in HUD fonts" );

/*
====================
CL_SubtitleFont

The HUD font at twice its size, drawn in screen pixels rather than through the HUD scale, so the panel
and its text stay together. Loaded where SCR_LoadCreditsFont loads its own, so a video restart brings
both back.
====================
*/
static cl_font_t *CL_SubtitleFont( void )
{
	return cl_subfont.valid ? &cl_subfont : &cls.creditsFont;
}

void CL_SubtitlesLoadFont( void )
{
	cl_subfont_scale = bound( 1.0f, cl_subtitles_scale.value, 4.0f );

	CL_FreeFont( &cl_subfont );

	// TF_FONT lets gl_texture_nearest point the glyphs, which is what the software style does to every
	// font. Subtitles are read rather than looked at, so they keep their filtering in every style.
	Con_LoadVariableWidthFont( "gfx/creditsfont.fnt", &cl_subfont, hud_fontscale.value * cl_subfont_scale,
		&hud_fontrender, TF_NOMIPMAP | TF_CLAMP );
}

/*
====================
CL_SubtitleColor

Valve marks the speaker with a colour tag in the caption file itself; ours has none, so the name of the
sentence tells us: the suit and the hazard course's holographic assistant speak in the HUD's amber, the
transit system and the announcements named after their maps (C1A0_0 and such) in grey, people in white.
====================
*/
static void CL_SubtitleColor( const char *name, rgba_t color )
{
	const char *slash = Q_strrchr( name, '/' );

	// speech that is a lone wav is named after its directory, and the directory says who speaks
	if( slash )
	{
		if( !Q_strnicmp( name, "holo/", 5 ))
		{
			MakeRGBA( color, 255, 160, 0, 255 );
			return;
		}

		if( !Q_strnicmp( name, "tride/", 6 ))
		{
			MakeRGBA( color, 180, 180, 180, 255 );
			return;
		}

		MakeRGBA( color, 255, 255, 255, 255 );
		return;
	}

	if( !Q_strnicmp( name, "HEV_", 4 ) || !Q_strnicmp( name, "HOLO_", 5 ))
		MakeRGBA( color, 255, 160, 0, 255 );
	else if( !Q_strnicmp( name, "TR_", 3 ) || !Q_strnicmp( name, "POWER_", 6 ) || !Q_strnicmp( name, "WE3", 3 ) ||
		( name[0] == 'C' && name[1] >= '0' && name[1] <= '9' && name[2] == 'A' )) // C1A0_0 and the other announcements
		MakeRGBA( color, 180, 180, 180, 255 );
	else MakeRGBA( color, 255, 255, 255, 255 );
}

/*
====================
CL_SubtitleHash

A caption's name, case and all, in one byte.
====================
*/
static int CL_SubtitleHash( const char *name )
{
	uint hash = 0;

	for( ; *name; name++ )
		hash = ( hash * 31 ) + (byte)( *name & ~0x20 ); // letters of either case land in the same bucket

	return hash & 0xFF;
}

/*
====================
CL_SubtitleFind

Every sound the game starts asks this, not only speech, so the names are bucketed by their hash: a miss
costs three or four comparisons instead of a walk of all 954.
====================
*/
static const client_textmessage_t *CL_SubtitleFind( const char *name )
{
	int i;

	if( !cl_caption_next )
		return NULL;

	for( i = cl_caption_bucket[CL_SubtitleHash( name )]; i >= 0; i = cl_caption_next[i] )
	{
		if( !Q_stricmp( cl_captions[i].pName, name ))
			return &cl_captions[i];
	}

	return NULL;
}

/*
====================
CL_SubtitleNextName

The mod splits a long line into KEY, KEY_PT2, KEY_PT3, so the part after this one is a name away.
====================
*/
static void CL_SubtitleNextName( const char *key, char *out, size_t size )
{
	string root;
	char *p;
	int part = 1;

	Q_strncpy( root, key, sizeof( root ));

	if(( p = Q_strrchr( root, '_' )) != NULL && !Q_strnicmp( p, "_PT", 3 ) && Q_isdigit( p + 3 ))
	{
		part = Q_atoi( p + 3 );
		*p = '\0';
	}

	Q_snprintf( out, size, "%s_PT%i", root, part + 1 );
}

/*
====================
CL_SubtitleRepeats

true when this group had a caption too recently to show another. The slot is stamped only when a caption
is heard, so a line that dies unheard does not silence the next one.
====================
*/
static qboolean CL_SubtitleRepeats( const char *name, qboolean heard )
{
	char group[32];
	size_t len;
	int i, oldest = 0;

	Q_strncpy( group, name, sizeof( group ));

	for( len = Q_strlen( group ); len > 0 && group[len - 1] >= '0' && group[len - 1] <= '9'; len-- )
		group[len - 1] = '\0';

	for( i = 0; i < SUB_REPEATS; i++ )
	{
		if( !Q_stricmp( cl_subrepeats[i].group, group ))
		{
			if( cl.time - cl_subrepeats[i].time < SUB_NOREPEAT && cl.time >= cl_subrepeats[i].time )
				return true;

			if( heard )
				cl_subrepeats[i].time = cl.time;
			return false;
		}

		if( cl_subrepeats[i].time < cl_subrepeats[oldest].time )
			oldest = i;
	}

	if( heard )
	{
		Q_strncpy( cl_subrepeats[oldest].group, group, sizeof( cl_subrepeats[oldest].group ));
		cl_subrepeats[oldest].time = cl.time;
	}

	return false;
}

/*
====================
CL_SubtitleStart

Called from VOX_LoadSound with the sentence a channel just took, and from S_StartSound with a lone sound
whose name the caption file knows: the announcements of the train ride and a few scripted lines are wav
files, not sentences.
====================
*/
void CL_SubtitleStart( const struct channel_s *ch, const char *name, const char *chname, const struct sfx_s *sfx )
{
	const client_textmessage_t *msg;
	subtitle_t *sub;

	if( !cl_subtitles.value || !cl_numcaptions || !ch || !name )
		return;

	if(( msg = CL_SubtitleFind( name )) == NULL )
	{
		// the mod does not cover every line; a developer run says which ones, so a gap can be filled later
		if( !sfx )
			Con_Reportf( "subtitles: no caption for %s\n", name );
		return;
	}

	if( CL_SubtitleRepeats( name, false ))
		return;

	if( cl_numsubs >= SUB_MAX_ITEMS )
	{
		memmove( &cl_subs[0], &cl_subs[1], ( SUB_MAX_ITEMS - 1 ) * sizeof( subtitle_t ));
		cl_numsubs = SUB_MAX_ITEMS - 1;
	}

	sub = &cl_subs[cl_numsubs++];
	memset( sub, 0, sizeof( *sub ));

	sub->msg = msg;
	sub->ch = ch;
	sub->sfx = sfx;
	// the channel is about to be named after the sound the server sent, which for a sentence is its index
	// ("!12"), not the name the caption is keyed by; keep that spelling, or nothing will ever match
	if( chname )
		Q_strncpy( sub->chname, chname, sizeof( sub->chname ));
	Q_strncpy( sub->key, name, sizeof( sub->key ));
	sub->start = sub->partstart = cl.time;
	CL_SubtitleColor( name, sub->color );
}

/*
====================
CL_SubtitleAlive

The channel still carries the sentence this caption belongs to.
====================
*/
static qboolean CL_SubtitleAlive( const subtitle_t *sub )
{
	// a lone sound holds its sfx for as long as it plays, and the channel takes another when it is done
	if( sub->sfx )
		return sub->ch->sfx == sub->sfx;

	if( !sub->ch->sfx || !sub->ch->words )
		return false;

	// the channel keeps the name the sentence came with, '!' and all, cut to the field it lives in
	return !Q_strnicmp( S_SkipSoundChar( sub->ch->name ), sub->chname, sizeof( sub->ch->name ) - 2 );
}

/*
====================
CL_SubtitleWrap

Breaks a caption into lines that fit the panel. Line feeds of the file break a line too.
====================
*/
static int CL_SubtitleWrap( const char *text, int maxwidth, char lines[SUB_MAX_LINES][SUB_LINE_MAX] )
{
	char line[SUB_LINE_MAX], word[SUB_LINE_MAX];
	int count = 0, linelen = 0;

	line[0] = '\0';

	while( *text && count < SUB_MAX_LINES )
	{
		int wordlen = 0, width, height;
		qboolean newline = false;

		while( *text && *text != ' ' && *text != '\n' && *text != '\r' && wordlen < SUB_LINE_MAX - 1 )
			word[wordlen++] = *text++;
		word[wordlen] = '\0';

		while( *text == ' ' || *text == '\n' || *text == '\r' )
		{
			if( *text == '\n' )
				newline = true;
			text++;
		}

		if( !wordlen )
		{
			if( newline && linelen )
			{
				Q_strncpy( lines[count++], line, SUB_LINE_MAX );
				line[0] = '\0';
				linelen = 0;
			}
			continue;
		}

		if( linelen )
		{
			char candidate[SUB_LINE_MAX];

			Q_snprintf( candidate, sizeof( candidate ), "%s %s", line, word );
			CL_DrawStringLen( CL_SubtitleFont(), candidate, &width, &height, 0 );

			if( width <= maxwidth )
			{
				Q_strncpy( line, candidate, sizeof( line ));
				linelen = Q_strlen( line );
			}
			else
			{
				Q_strncpy( lines[count++], line, SUB_LINE_MAX );
				Q_strncpy( line, word, sizeof( line ));
				linelen = wordlen;
			}
		}
		else
		{
			Q_strncpy( line, word, sizeof( line ));
			linelen = wordlen;
		}

		if( newline && linelen && count < SUB_MAX_LINES )
		{
			Q_strncpy( lines[count++], line, SUB_LINE_MAX );
			line[0] = '\0';
			linelen = 0;
		}
	}

	if( linelen && count < SUB_MAX_LINES )
		Q_strncpy( lines[count++], line, SUB_LINE_MAX );

	return count;
}

/*
====================
CL_SubtitleAlpha
====================
*/
static float CL_SubtitleAlpha( const subtitle_t *sub )
{
	double age = cl.time - sub->start - SUB_HIDDEN;
	float alpha = 1.0f;

	if( age < 0.0 )
		alpha = 0.0f;
	else if( age < SUB_FADE_IN )
		alpha = (float)( age / SUB_FADE_IN );

	if( sub->finished != 0.0 )
	{
		double gone = cl.time - ( sub->finished + SUB_LINGER );

		if( gone > 0.0 )
			alpha = Q_min( alpha, 1.0f - (float)( gone / SUB_FADE_OUT ));
	}

	return bound( 0.0f, alpha, 1.0f );
}

/*
====================
CL_SubtitlesUpdate

Drops what has faded out, and walks a long line on to its next part.
====================
*/
static void CL_SubtitlesUpdate( void )
{
	int i;

	for( i = 0; i < cl_numsubs; i++ )
	{
		subtitle_t *sub = &cl_subs[i];

		// the clock went back: a level change or a loaded save this caption never belonged to. Its own
		// times are now in the future, so it would neither show nor age out, and would hold a blank row
		// on the panel until the map caught up with them. Client time is interpolated and steps back by a
		// fraction of a frame all the time, which is not that, so only a jump worth a second counts.
		if( cl.time < sub->start - SUB_CLOCK_BACK )
		{
			memmove( &cl_subs[i], &cl_subs[i + 1], ( cl_numsubs - i - 1 ) * sizeof( subtitle_t ));
			cl_numsubs--;
			i--;
			continue;
		}

		if( CL_SubtitleAlive( sub ))
		{
			if( !sub->heard )
			{
				CL_SubtitleRepeats( sub->key, true );
				Con_Reportf( "subtitles: %s\n", sub->key );
			}
			sub->heard = true;
		}
		else if( sub->finished == 0.0 )
			sub->finished = cl.time;

		// a sentence too far away to hear is freed in the frame it started: no caption for it at all
		if( !sub->heard && sub->finished != 0.0 )
		{
			memmove( &cl_subs[i], &cl_subs[i + 1], ( cl_numsubs - i - 1 ) * sizeof( subtitle_t ));
			cl_numsubs--;
			i--;
			continue;
		}

		if( sub->finished == 0.0 && sub->msg->holdtime > 0.0f && cl.time - sub->partstart > sub->msg->holdtime )
		{
			char next[64];
			const client_textmessage_t *msg;

			CL_SubtitleNextName( sub->key, next, sizeof( next ));
			msg = CL_SubtitleFind( next );

			if( msg )
			{
				sub->msg = msg;
				Q_strncpy( sub->key, next, sizeof( sub->key ));
				sub->partstart = cl.time; // not start: the caption is already on screen, it must not fade in again
			}
		}

		if( CL_SubtitleAlpha( sub ) <= 0.0f && sub->finished != 0.0 && cl.time - sub->start > SUB_HIDDEN )
		{
			memmove( &cl_subs[i], &cl_subs[i + 1], ( cl_numsubs - i - 1 ) * sizeof( subtitle_t ));
			cl_numsubs--;
			i--;
		}
	}
}

/*
==================
CL_SubtitlesFrame

The size is archived and can be changed while a caption is on screen. Rebuilding the font frees a texture,
and a texture may not be released while the frame is being drawn - the commands already recorded can still
be reading it. So the check lives here, before the rendering starts.
==================
*/
void CL_SubtitlesFrame( void )
{
	if( !cl_subtitles.value || !cl_subfont.valid )
		return;

	if( cl_subfont_scale != bound( 1.0f, cl_subtitles_scale.value, 4.0f ))
		CL_SubtitlesLoadFont();
}

/*
====================
CL_DrawSubtitles

Called from V_PostRender, over the HUD.
====================
*/
void CL_DrawSubtitles( void )
{
	char lines[SUB_MAX_ITEMS][SUB_MAX_LINES][SUB_LINE_MAX];
	int numlines[SUB_MAX_ITEMS];
	int i, j, total = 0, maxwidth, panel_w = 0, x, y;
	float goal, bottom, panel_alpha = 0.0f;

	if( !cl_subtitles.value || !cl_numsubs )
	{
		cl_numsubs = 0;
		cl_panel_height = 0.0f;
		return;
	}

	if( !CL_SubtitleFont()->valid )
		return;

	CL_SubtitlesUpdate();

	maxwidth = (int)( refState.width * SUB_WIDTH_FRAC );

	for( i = 0; i < cl_numsubs; i++ )
	{
		numlines[i] = CL_SubtitleWrap( cl_subs[i].msg->pMessage, maxwidth, lines[i] );
		total += numlines[i];
	}

	if( !total )
		return;

	// the panel takes lines, not captions: a long one pushes the oldest out, but one always stays
	while( total > SUB_LINE_BUDGET && cl_numsubs > 1 )
	{
		total -= numlines[0];
		memmove( &cl_subs[0], &cl_subs[1], ( cl_numsubs - 1 ) * sizeof( subtitle_t ));
		memmove( &numlines[0], &numlines[1], ( cl_numsubs - 1 ) * sizeof( numlines[0] ));
		memmove( &lines[0], &lines[1], ( cl_numsubs - 1 ) * sizeof( lines[0] ));
		cl_numsubs--;
	}

	// width and opacity come from what is still on the panel, not from what was pushed off it
	for( i = 0; i < cl_numsubs; i++ )
	{
		// the panel is up while the text inside it is still hidden, so it grows before the words arrive
		panel_alpha = Q_max( panel_alpha,
			cl_subs[i].finished == 0.0 ? 1.0f : CL_SubtitleAlpha( &cl_subs[i] ));

		for( j = 0; j < numlines[i]; j++ )
		{
			int width, height;

			CL_DrawStringLen( CL_SubtitleFont(), lines[i][j], &width, &height, 0 );
			panel_w = Q_max( panel_w, width );
		}
	}

	// the panel follows the lines it holds instead of jumping, as Half-Life 2's does
	goal = (float)( total * CL_SubtitleFont()->charHeight + 2 * SUB_PADDING );

	if( cl_panel_height <= 0.0f )
		cl_panel_height = goal;
	else cl_panel_height += ( goal - cl_panel_height ) * Q_min( 1.0f, (float)( host.frametime / SUB_GROW_TIME ));

	panel_w += 2 * SUB_PADDING;
	x = ( refState.width - panel_w ) / 2;
	bottom = refState.height * ( 1.0f - SUB_SAFE_AREA - SUB_HUD_ROOM );
	y = (int)( bottom - cl_panel_height );

	// the panel fades with the last caption it holds, so it never pops off the screen
	ref.dllFuncs.FillRGBA( kRenderTransTexture, x, y, panel_w, (int)cl_panel_height, 0, 0, 0,
		(int)( SUB_PANEL_ALPHA * panel_alpha ));

	// the words sit where they will stand once the panel has finished growing, not where its edge is now
	y = (int)( bottom - goal ) + SUB_PADDING;

	for( i = 0; i < cl_numsubs; i++ )
	{
		rgba_t color;
		float alpha = CL_SubtitleAlpha( &cl_subs[i] );

		MakeRGBA( color, cl_subs[i].color[0], cl_subs[i].color[1], cl_subs[i].color[2], (byte)( 255 * alpha ));

		for( j = 0; j < numlines[i]; j++ )
		{
			int width, height;

			CL_DrawStringLen( CL_SubtitleFont(), lines[i][j], &width, &height, 0 );
			CL_DrawString( ( refState.width - width ) / 2, y, lines[i][j], color, CL_SubtitleFont(), 0 );
			y += CL_SubtitleFont()->charHeight;
		}
	}
}

/*
====================
CL_SubtitlesClear

Called from CL_ClearState: the captions on screen belong to the map that is going away.
====================
*/
void CL_SubtitlesClear( void )
{
	cl_numsubs = 0;
	cl_panel_height = 0.0f;
	memset( cl_subrepeats, 0, sizeof( cl_subrepeats ));
}

/*
====================
CL_SubtitlesInit

The caption file of the language the game is mounted in, parsed like titles.txt.
====================
*/
void CL_SubtitlesInit( void )
{
	fs_offset_t fileSize = 0;
	char *pMemFile;

	CL_SubtitlesShutdown();

	if(( pMemFile = (char *)FS_LoadFile( "subtitles.txt", &fileSize, false )) == NULL )
		return; // a language without captions of its own

	cl_captions = CL_TextMessageParse( clgame.mempool, pMemFile, fileSize, &cl_numcaptions );
	Mem_Free( pMemFile );

	if( cl_captions && cl_numcaptions > 0 )
	{
		int i;

		for( i = 0; i < 256; i++ )
			cl_caption_bucket[i] = -1;

		cl_caption_next = (int *)Mem_Malloc( clgame.mempool, cl_numcaptions * sizeof( int ));

		for( i = 0; i < cl_numcaptions; i++ )
		{
			int bucket = CL_SubtitleHash( cl_captions[i].pName );

			cl_caption_next[i] = cl_caption_bucket[bucket];
			cl_caption_bucket[bucket] = i;
		}
	}
}

/*
====================
CL_SubtitlesShutdown
====================
*/
void CL_SubtitlesShutdown( void )
{
	if( cl_captions )
		Mem_Free( cl_captions );

	if( cl_caption_next )
		Mem_Free( cl_caption_next );

	cl_caption_next = NULL;

	// the font is not freed here: CL_SubtitlesInit comes through this on every map, while the font is
	// loaded once with the HUD's own, and freeing it here left every caption drawn in the small one
	cl_captions = NULL;
	cl_numcaptions = 0;
	cl_numsubs = 0;
	cl_panel_height = 0.0f;
	memset( cl_subrepeats, 0, sizeof( cl_subrepeats ));
}

#if XASH_ENGINE_TESTS
#include "tests.h"

static void Test_SubtitleNextName( void )
{
	const char *data[] =
	{
		"BA_ALIVE", "BA_ALIVE_PT2",
		"BA_ALIVE_PT2", "BA_ALIVE_PT3",
		"BA_ALIVE_PT9", "BA_ALIVE_PT10",
		"HEV_AAx", "HEV_AAx_PT2",
		"C1A0_0", "C1A0_0_PT2", // a name that ends in a number is not a part of anything
	};
	char next[64];
	int i;

	for( i = 0; i < sizeof( data ) / sizeof( data[0] ); i += 2 )
	{
		CL_SubtitleNextName( data[i], next, sizeof( next ));
		TASSERT_STR( next, data[i+1] );
	}
}

static void Test_SubtitleColor( void )
{
	rgba_t color;

	CL_SubtitleColor( "HEV_AAx", color );
	TASSERT_EQi( color[0], 255 ); TASSERT_EQi( color[1], 160 ); TASSERT_EQi( color[2], 0 );

	CL_SubtitleColor( "HOLO_INTRO", color );
	TASSERT_EQi( color[1], 160 );

	CL_SubtitleColor( "C1A0_0", color );
	TASSERT_EQi( color[0], 180 );

	CL_SubtitleColor( "TR_GMORN", color );
	TASSERT_EQi( color[0], 180 );

	CL_SubtitleColor( "SC_ALERT", color );
	TASSERT_EQi( color[0], 255 ); TASSERT_EQi( color[1], 255 ); TASSERT_EQi( color[2], 255 );

	CL_SubtitleColor( "BA_POK", color );
	TASSERT_EQi( color[2], 255 );

	// speech that is a lone wav, named after the directory it lives in
	CL_SubtitleColor( "tride/c0a0_tr_gmorn.wav", color );
	TASSERT_EQi( color[0], 180 );

	CL_SubtitleColor( "holo/tr_holo_breath.wav", color );
	TASSERT_EQi( color[1], 160 );

	CL_SubtitleColor( "scientist/c1a0_sci_stall.wav", color );
	TASSERT_EQi( color[0], 255 ); TASSERT_EQi( color[1], 255 );
}

static void Test_SubtitleAlive( void )
{
	channel_t ch;
	subtitle_t sub;

	memset( &ch, 0, sizeof( ch ));
	memset( &sub, 0, sizeof( sub ));

	// the server sends a sentence as its index, so that is what the channel is named after, not the name
	// the caption is keyed by
	ch.sfx = (sfx_t *)0x1;
	ch.words = (voxword_t *)0x1;
	Q_strncpy( ch.name, "!12", sizeof( ch.name ));
	sub.ch = &ch;
	Q_strncpy( sub.chname, "12", sizeof( sub.chname ));
	TASSERT( CL_SubtitleAlive( &sub ));

	Q_strncpy( ch.name, "!13", sizeof( ch.name ));
	TASSERT( !CL_SubtitleAlive( &sub ));

	// a lone sound keeps no name on the channel and is known by its sfx
	memset( &sub, 0, sizeof( sub ));
	sub.ch = &ch;
	sub.sfx = (sfx_t *)0x1;
	TASSERT( CL_SubtitleAlive( &sub ));

	ch.sfx = (sfx_t *)0x2;
	TASSERT( !CL_SubtitleAlive( &sub ));
}

// a level change and a loaded save both start the clock again: what was on screen must go with the map
static void Test_SubtitleRewind( void )
{
	channel_t ch;
	double savedtime = cl.time;

	memset( &ch, 0, sizeof( ch ));
	memset( &cl_subs[0], 0, sizeof( cl_subs[0] ));
	cl_subs[0].ch = &ch;
	cl_subs[0].start = cl_subs[0].partstart = 30.0;
	cl_subs[0].heard = true;
	cl_numsubs = 1;

	// interpolated client time steps back by a fraction of a frame all the time: that is not a new map,
	// and a caption dropped there is a caption the player never sees
	cl.time = 30.0 - 0.02;
	CL_SubtitlesUpdate();
	TASSERT_EQi( cl_numsubs, 1 );

	cl.time = 2.0; // the new map's clock, behind the caption's own times
	CL_SubtitlesUpdate();
	TASSERT_EQi( cl_numsubs, 0 );

	cl.time = savedtime;
}

void Test_RunSubtitles( void )
{
	TRUN( Test_SubtitleNextName() );
	TRUN( Test_SubtitleColor() );
	TRUN( Test_SubtitleAlive() );
	TRUN( Test_SubtitleRewind() );
}
#endif /* XASH_ENGINE_TESTS */
