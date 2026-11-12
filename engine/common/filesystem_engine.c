/*
filesystem.c - game filesystem based on DP fs
Copyright (C) 2003-2006 Mathieu Olivier
Copyright (C) 2000-2007 DarkPlaces contributors
Copyright (C) 2007 Uncle Mike
Copyright (C) 2015-2023 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#if XASH_SDL == 2
#include <SDL.h> // SDL_GetBasePath
#elif XASH_SDL == 3
#include <SDL3/SDL.h>
#endif

#include <errno.h>
#include <sys/stat.h>
#include "common.h"
#include "library.h"
#include "platform/platform.h"

#if XASH_WIN32
#include <direct.h>
#endif

static CVAR_DEFINE_AUTO( fs_mount_hd, "0", FCVAR_PRIVILEGED, "mount high definition content folder" );
static CVAR_DEFINE_AUTO( fs_mount_lv, "0", FCVAR_PRIVILEGED, "mount low violence models content folder" );
static CVAR_DEFINE_AUTO( fs_mount_addon, "0", FCVAR_PRIVILEGED, "mount addon content folder" );
static CVAR_DEFINE_AUTO( fs_mount_l10n, "0", FCVAR_PRIVILEGED, "mount localization content folder" );
static CVAR_DEFINE_AUTO( ui_language, "english", FCVAR_PRIVILEGED, "selected game language" );

fs_api_t g_fsapi;
fs_globals_t *FI;

static pfnCreateInterface_t fs_pfnCreateInterface;
static HINSTANCE fs_hInstance;

search_t *FS_Search( const char *pattern, int caseinsensitive, int gamedironly )
{
	return g_fsapi.Search( pattern, caseinsensitive, gamedironly );
}

int FS_Close( file_t *file )
{
	return g_fsapi.Close( file );
}

file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly )
{
	return g_fsapi.Open( filepath, mode, gamedironly );
}

byte *FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
	return g_fsapi.LoadFile( path, filesizeptr, gamedironly );
}

byte *FS_LoadDirectFile( const char *path, fs_offset_t *filesizeptr )
{
	return g_fsapi.LoadDirectFile( path, filesizeptr );
}

static void COM_StripDirectorySlash( char *pname )
{
	size_t len = Q_strlen( pname );
	if( len > 0 && pname[len - 1] == '/' )
		pname[len - 1] = 0;
}

void *FS_GetNativeObject( const char *obj )
{
	if( fs_pfnCreateInterface )
		return fs_pfnCreateInterface( obj, NULL );

	return NULL;
}

static uint32_t fs_mounted; // the flags the search paths were last built with

static uint32_t FS_MountFlags( void )
{
	uint32_t flags = 0;

	// FIXME: VFS shouldn't care about this, allow engine to mount gamedirs
	if( fs_mount_lv.value ) SetBits( flags, FS_MOUNT_LV );
	if( fs_mount_hd.value ) SetBits( flags, FS_MOUNT_HD );
	if( fs_mount_addon.value ) SetBits( flags, FS_MOUNT_ADDON );
	if( fs_mount_l10n.value ) SetBits( flags, FS_MOUNT_L10N );

	return flags;
}

void FS_Rescan_f( void )
{
	fs_mounted = FS_MountFlags();
	g_fsapi.Rescan( fs_mounted, ui_language.string );
}

// a content folder switched on or off in the menu during a game: only safe to mount with no model loaded
qboolean FS_MountsChanged( void )
{
	return FS_MountFlags() != fs_mounted;
}

static void FS_LoadVFSConfig( const char *gamedir )
{
	string parm;

	if( Host_IsDedicated( ))
		return;

	Cbuf_AddTextf( "exec %s/vfs.cfg\n", gamedir );
	Cbuf_Execute();

	if( Sys_GetParmFromCmdLine( "-language", parm ))
	{
		Cvar_DirectSet( &ui_language, parm );
		Cvar_DirectSet( &fs_mount_l10n, "1" );
	}

	ClearBits( fs_mount_hd.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_lv.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_l10n.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_addon.flags, FCVAR_CHANGED );
	ClearBits( ui_language.flags, FCVAR_CHANGED );
}

void FS_SaveVFSConfig( void )
{
	const qboolean force_save = !FS_FileExists( "vfs.cfg", true );

	if( !force_save && !FBitSet( fs_mount_hd.flags|fs_mount_lv.flags|fs_mount_l10n.flags|fs_mount_addon.flags|ui_language.flags, FCVAR_CHANGED ))
	{
		Con_Reportf( "%s: no need to save vfs.cfg\n", __func__ );
		return;
	}

	Con_Printf( "%s()\n", __func__ );

	file_t *f = FS_Open( "vfs.cfg.new", "w", true );
	if( !f )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, "vfs.cfg.new" );
		return;
	}

	FS_Printf( f, "%s \"%d\"\n", fs_mount_hd.name, (int)fs_mount_hd.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_lv.name, (int)fs_mount_lv.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_l10n.name, (int)fs_mount_l10n.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_addon.name, (int)fs_mount_addon.value );
	FS_Printf( f, "%s \"%s\"\n", ui_language.name, ui_language.string );

	Host_FinalizeConfig( f, "vfs.cfg" );

	ClearBits( fs_mount_hd.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_lv.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_l10n.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_addon.flags, FCVAR_CHANGED );
	ClearBits( ui_language.flags, FCVAR_CHANGED );
}

void FS_LoadGameInfo( void )
{
	FS_LoadVFSConfig( g_fsapi.Gamedir( ));

	fs_mounted = FS_MountFlags();
	g_fsapi.LoadGameInfo( fs_mounted, ui_language.string );
}

static void FS_ClearPaths_f( void )
{
	FS_ClearSearchPath();
}

static void FS_Path_f_( void )
{
	FS_Path_f();
}

static void FS_FindFile_f_( void )
{
	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "fs_find <filepath>\n" );
		return;
	}
	g_fsapi.FindFile_f( Cmd_Argv( 1 ));
}

static void FS_MakeGameInfo_f( void )
{
	g_fsapi.MakeGameInfo();
}

static const fs_interface_t fs_memfuncs =
{
	Con_Printf,
	Con_DPrintf,
	Con_Reportf,
	Sys_Error,

	_Mem_AllocPool,
	_Mem_FreePool,
	_Mem_Alloc,
	_Mem_Realloc,
	_Mem_Free,

	Sys_GetNativeObject,
};

static void FS_UnloadProgs( void )
{
	if( fs_hInstance )
	{
		COM_FreeLibrary( fs_hInstance );
		fs_hInstance = 0;
	}
}

#ifdef XASH_INTERNAL_GAMELIBS
#define FILESYSTEM_STDIO_DLL "filesystem_stdio"
#elif XASH_ANDROID
#define FILESYSTEM_STDIO_DLL "libfilesystem_stdio.so"
#else
#define FILESYSTEM_STDIO_DLL "filesystem_stdio." OS_LIB_EXT
#endif

static qboolean FS_LoadProgs( void )
{
	const char *name = FILESYSTEM_STDIO_DLL;

	fs_hInstance = COM_LoadLibrary( name, false, true );

	if( !fs_hInstance )
	{
		Sys_Error( "%s: can't load filesystem library %s: %s\n", __func__, name, COM_GetLibraryError() );
		return false;
	}

	FSAPI GetFSAPI;
	if( !( GetFSAPI = (FSAPI)COM_GetProcAddress( fs_hInstance, GET_FS_API )))
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't find GetFSAPI entry point in %s\n", __func__, name );
		return false;
	}

	if( GetFSAPI( FS_API_VERSION, &g_fsapi, &FI, &fs_memfuncs ) != FS_API_VERSION )
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't initialize filesystem API: wrong version\n", __func__ );
		return false;
	}

	if( !( fs_pfnCreateInterface = (pfnCreateInterface_t)COM_GetProcAddress( fs_hInstance, "CreateInterface" )))
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't find CreateInterface entry point in %s\n", __func__, name );
		return false;
	}

	Con_DPrintf( "%s: filesystem_stdio successfully loaded\n", __func__ );
	return true;
}

static qboolean FS_DetermineRootDirectory( char *out, size_t size )
{
	const char *path = getenv( "XASH3D_BASEDIR" );

	if( !COM_StringEmptyOrNULL( path ))
	{
		Q_strncpy( out, path, size );
		return true;
	}

#if XASH_IOS
	Q_strncpy( out, IOS_GetDocsDir(), size );
	return true;
#elif XASH_PSVITA
	if( PSVita_GetBasePath( out, size ))
		return true;
	Sys_Error( "couldn't find %s data directory", XASH_ENGINE_NAME );
	return false;
#elif XASH_PS4
	if( PS4_GetBasePath( out, size ))
		return true;
	Sys_Error( "couldn't find %s data directory", XASH_ENGINE_NAME );
	return false;
#elif ( XASH_SDL >= 2 ) && !XASH_NSWITCH // GetBasePath not impl'd in switch-sdl2
	// SDL2 hands the base path over to us, SDL3 keeps ownership and frees it on quit
	qboolean owned = XASH_SDL == 2;

	path = SDL_GetBasePath();

#if XASH_APPLE
	if( path != NULL && Q_stristr( path, ".app" ))
	{
		if( owned )
			SDL_free((void *)path );

		// the pref path is ours to free in both versions
		path = SDL_GetPrefPath( NULL, XASH_ENGINE_NAME );
		owned = true;
	}
#endif

	if( path != NULL )
	{
		Q_strncpy( out, path, size );

		if( owned )
			SDL_free((void *)path );
		return true;
	}

#if XASH_POSIX || XASH_WIN32
	if( getcwd( out, size ))
		return true;
	Sys_Error( "couldn't determine current directory: %s, getcwd: %s", SDL_GetError(), strerror( errno ));
#else // !( XASH_POSIX || XASH_WIN32 )
	Sys_Error( "couldn't determine current directory: %s", SDL_GetError( ));
#endif // !( XASH_POSIX || XASH_WIN32 )
	return false;
#else // generic case
	if( getcwd( out, size ))
		return true;

	Sys_Error( "couldn't determine current directory: %s", strerror( errno ));
	return false;
#endif // generic case
}

static qboolean FS_DetermineReadOnlyRootDirectory( char *out, size_t size )
{
	const char *env_rodir = getenv( "XASH3D_RODIR" );

	if( _Sys_GetParmFromCmdLine( "-rodir", out, size ))
		return true;

	if( !COM_StringEmptyOrNULL( env_rodir ))
	{
		Q_strncpy( out, env_rodir, size );
		return true;
	}

#if XASH_IOS
	Q_strncpy( out, IOS_GetExecDir(), size );
	return true;
#endif

	return false;
}

/*
The engine's paths are UTF-8; on Windows the C library's narrow calls take the ANSI code page instead, so a library
in a folder with a non-ASCII name would not be found (filesystem/sys.c goes through wide characters for the same reason).
*/
static qboolean FS_SysPathExists( const char *path, qboolean dir )
{
#if XASH_WIN32
	wchar_t wide[MAX_OSPATH];
	struct _stat st;

	if( !MultiByteToWideChar( CP_UTF8, 0, path, -1, wide, ARRAYSIZE( wide )) || _wstat( wide, &st ) != 0 )
		return false;
#else
	struct stat st;

	if( stat( path, &st ) != 0 )
		return false;
#endif
	return !dir || ( st.st_mode & S_IFDIR );
}

static FILE *FS_SysOpenText( const char *path )
{
#if XASH_WIN32
	wchar_t wide[MAX_OSPATH];

	return MultiByteToWideChar( CP_UTF8, 0, path, -1, wide, ARRAYSIZE( wide )) ? _wfopen( wide, L"r" ) : NULL;
#else
	return fopen( path, "r" );
#endif
}

/*
The Half-Life in one of the Steam libraries that <steam> lists in steamapps/libraryfolders.vdf, <steam> itself
first, into out. tools/launcher.sh does the same on Linux; this is for a start without it.
*/
static qboolean FS_FindSteamGame( const char *steam, char *out, size_t size )
{
	char line[MAX_OSPATH * 2], lib[MAX_OSPATH];
	FILE *f;

	Q_snprintf( out, size, "%s/steamapps/common/Half-Life", steam );
	Q_snprintf( lib, sizeof( lib ), "%s/%s", out, host.default_gamedir );
	if( FS_SysPathExists( lib, true ))
		return true;

	Q_snprintf( line, sizeof( line ), "%s/steamapps/libraryfolders.vdf", steam );
	if( !( f = FS_SysOpenText( line )))
		return false;

	while( fgets( line, sizeof( line ), f ))
	{
		// "path"		"D:\\SteamLibrary": the value is quoted, backslashes escaped
		const char *p = Q_strstr( line, "\"path\"" );
		size_t len = 0;

		if( !p || !( p = Q_strchr( p + 6, '"' )))
			continue;

		for( p++; *p && *p != '"' && len < sizeof( lib ) - 1; p++ )
		{
			if( *p == '\\' && p[1] )
				p++;
			lib[len++] = *p;
		}
		lib[len] = 0;

		Q_snprintf( out, size, "%s/steamapps/common/Half-Life", lib );
		Q_snprintf( lib, sizeof( lib ), "%s/%s", out, host.default_gamedir );
		if( FS_SysPathExists( lib, true ))
		{
			fclose( f );
			return true;
		}
	}

	fclose( f );
	return false;
}

static qboolean FS_FindSteamInstall( char *out, size_t size )
{
#if XASH_WIN32
	static const struct { HKEY root; const char *key; const wchar_t *value; } keys[] =
	{
		{ HKEY_CURRENT_USER, "Software\\Valve\\Steam", L"SteamPath" },
		{ HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath" },
		{ HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", L"InstallPath" },
	};
	int i;

	for( i = 0; i < (int)ARRAYSIZE( keys ); i++ )
	{
		wchar_t wide[MAX_OSPATH];
		char steam[MAX_OSPATH];
		DWORD len = sizeof( wide ) - sizeof( wide[0] ), type;
		HKEY key;
		LONG ret;

		// RegGetValue is Vista and later, the 32-bit build still starts on XP
		if( RegOpenKeyExA( keys[i].root, keys[i].key, 0, KEY_QUERY_VALUE, &key ) != ERROR_SUCCESS )
			continue;

		// the wide value, as UTF-8 like every other path of the engine
		ret = RegQueryValueExW( key, keys[i].value, NULL, &type, (LPBYTE)wide, &len );
		RegCloseKey( key );

		if( ret != ERROR_SUCCESS || type != REG_SZ )
			continue;

		wide[len / sizeof( wide[0] )] = 0;
		if( WideCharToMultiByte( CP_UTF8, 0, wide, -1, steam, sizeof( steam ), NULL, NULL ) && FS_FindSteamGame( steam, out, size ))
			return true;
	}
#elif XASH_LINUX
	static const char *roots[] = { ".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam" };
	const char *home = getenv( "HOME" );
	int i;

	for( i = 0; home && i < (int)ARRAYSIZE( roots ); i++ )
	{
		char steam[MAX_OSPATH];

		Q_snprintf( steam, sizeof( steam ), "%s/%s", home, roots[i] );
		if( FS_FindSteamGame( steam, out, size ))
			return true;
	}
#endif
	return false;
}

/*
================
FS_Init
================
*/
void FS_Init( void )
{
	string gamedir;
	char rodir[MAX_OSPATH], rootdir[MAX_OSPATH];
	rodir[0] = rootdir[0] = 0;

	if( !FS_DetermineRootDirectory( rootdir, sizeof( rootdir )) || COM_StringEmpty( rootdir ))
	{
		Sys_Error( "couldn't determine current directory (empty string)" );
		return;
	}
	COM_FixSlashes( rootdir );
	COM_StripDirectorySlash( rootdir );

	if( !FS_DetermineReadOnlyRootDirectory( rodir, sizeof( rodir )))
	{
		char path[MAX_OSPATH];

		// no game next to the binaries and none named: the one Steam installed. The binaries' own valve/
		// holds only saves, settings and the game code, liblist.gam comes with the game
		Q_snprintf( path, sizeof( path ), "%s/%s/liblist.gam", rootdir, host.default_gamedir );
		if( !FS_SysPathExists( path, false ) && FS_FindSteamInstall( rodir, sizeof( rodir )))
			Con_Reportf( "%s: game found in Steam: %s\n", __func__, rodir );
		else
			rodir[0] = 0;
	}
	COM_FixSlashes( rodir );
	COM_StripDirectorySlash( rodir );

	if( !Sys_GetParmFromCmdLine( "-game", gamedir ))
	{
		char *env = getenv( "XASH3D_GAME" );
		if( env )
			Q_strncpy( gamedir, env, sizeof( gamedir ));
		else
			Q_strncpy( gamedir, host.default_gamedir, sizeof( gamedir )); // gamedir == basedir
	}

	FS_LoadProgs();

	// TODO: this function will cause engine to stop in case of fail
	// when it will have an option to return string error, restore Sys_Error
	// FIXME: why do we call this function before InitStdio?
	// because InitStdio immediately scans all available game directories
	// and this better be reworked at some point
	g_fsapi.SetCurrentDirectory( rootdir );

	if( !g_fsapi.InitStdio( true, rootdir, host.default_gamedir, gamedir, rodir ))
	{
		Sys_Error( "Can't init filesystem_stdio!\n" );
		return;
	}

	Cmd_AddRestrictedCommand( "fs_rescan", FS_Rescan_f, "rescan filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_path", FS_Path_f_, "show filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_find", FS_FindFile_f_, "find file across search pathes and show all occurences" );
	Cmd_AddRestrictedCommand( "fs_clearpaths", FS_ClearPaths_f, "clear filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_make_gameinfo", FS_MakeGameInfo_f, "create gameinfo.txt for current running game" );

	Cvar_RegisterVariable( &fs_mount_hd );
	Cvar_RegisterVariable( &fs_mount_lv );
	Cvar_RegisterVariable( &fs_mount_addon );
	Cvar_RegisterVariable( &fs_mount_l10n );
	Cvar_RegisterVariable( &ui_language );

	if( !Sys_GetParmFromCmdLine( "-dll", host.gamedll ))
		host.gamedll[0] = 0;

	if( !Sys_GetParmFromCmdLine( "-clientlib", host.clientlib ))
		host.clientlib[0] = 0;

	if( !Sys_GetParmFromCmdLine( "-menulib", host.menulib ))
		host.menulib[0] = 0;
}

/*
================
FS_Shutdown
================
*/
void FS_Shutdown( void )
{
	if( g_fsapi.ShutdownStdio )
		g_fsapi.ShutdownStdio();

	FS_UnloadProgs();
}
