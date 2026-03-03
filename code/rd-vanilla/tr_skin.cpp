/*
===========================================================================
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

#include "tr_local.h"

#include <map>

#include "../qcommon/sstring.h"

static qboolean RE_ResolveSOF2G2Skin( const char *name, char *resolvedName, int resolvedNameSize ) {
	char candidate[MAX_QPATH];
	char aliasCandidate[MAX_QPATH];
	char *suffix;
	const char *ext;

	if ( !name || !name[0] ) {
		return qfalse;
	}

	if ( Q_stricmpn( name, "models/characters/skins/", 24 ) != 0 ) {
		return qfalse;
	}

	ext = COM_GetExtension( name );
	if ( ext[0] && Q_stricmp( ext, "skin" ) != 0 ) {
		return qfalse;
	}

	COM_StripExtension( name, candidate, sizeof( candidate ) );
	COM_DefaultExtension( candidate, sizeof( candidate ), ".g2skin" );

	if ( ri.FS_ReadFile( candidate, NULL ) <= 0 ) {
		Q_strncpyz( aliasCandidate, candidate, sizeof( aliasCandidate ) );
		suffix = aliasCandidate + strlen( aliasCandidate ) - strlen( ".g2skin" );

		if ( suffix > aliasCandidate + 3 && Q_stricmpn( suffix - 3, "_a0", 3 ) == 0 ) {
			memmove( suffix - 3, suffix, strlen( suffix ) + 1 );
			if ( ri.FS_ReadFile( aliasCandidate, NULL ) > 0 ) {
				Q_strncpyz( resolvedName, aliasCandidate, resolvedNameSize );
				return qtrue;
			}
		}

		return qfalse;
	}

	Q_strncpyz( resolvedName, candidate, resolvedNameSize );
	return qtrue;
}

/*
============================================================================

SKINS

============================================================================
*/

/*
==================
CommaParse

This is unfortunate, but the skin files aren't
compatible with our normal parsing rules.
==================
*/
static char *CommaParse( char **data_p ) {
	int c = 0, len;
	char *data;
	static	char	com_token[MAX_TOKEN_CHARS];

	data = *data_p;
	len = 0;
	com_token[0] = 0;

	// make sure incoming data is valid
	if ( !data ) {
		*data_p = NULL;
		return com_token;
	}

	while ( 1 ) {
		// skip whitespace
		while( (c = *(const unsigned char* /*eurofix*/)data) <= ' ') {
			if( !c ) {
				break;
			}
			data++;
		}


		c = *data;

		// skip double slash comments
		if ( c == '/' && data[1] == '/' )
		{
			while (*data && *data != '\n')
				data++;
		}
		// skip /* */ comments
		else if ( c=='/' && data[1] == '*' )
		{
			while ( *data && ( *data != '*' || data[1] != '/' ) )
			{
				data++;
			}
			if ( *data )
			{
				data += 2;
			}
		}
		else
		{
			break;
		}
	}

	if ( c == 0 ) {
		return "";
	}

	// handle quoted strings
	if (c == '\"')
	{
		data++;
		while (1)
		{
			c = *data++;
			if (c=='\"' || !c)
			{
				com_token[len] = 0;
				*data_p = ( char * ) data;
				return com_token;
			}
			if (len < MAX_TOKEN_CHARS - 1)
			{
				com_token[len] = c;
				len++;
			}
		}
	}

	// parse a regular word
	do
	{
		if (len < MAX_TOKEN_CHARS - 1)
		{
			com_token[len] = c;
			len++;
		}
		data++;
		c = *data;
	} while (c>32 && c != ',' );

	com_token[len] = 0;

	*data_p = ( char * ) data;
	return com_token;
}

/*
class CStringComparator
{
public:
	bool operator()(const char *s1, const char *s2) const { return(stricmp(s1, s2) < 0); }
};
*/
typedef std::map<sstring_t,char * /*, CStringComparator*/ >	AnimationCFGs_t;
													AnimationCFGs_t AnimationCFGs;

// I added this function for development purposes (and it's VM-safe) so we don't have problems
//	with our use of cached models but uncached animation.cfg files (so frame sequences are out of sync
//	if someone rebuild the model while you're ingame and you change levels)...
//
// Usage:  call with psDest == NULL for a size enquire (for malloc),
//				then with NZ ptr for it to copy to your supplied buffer...
//
int RE_GetAnimationCFG(const char *psCFGFilename, char *psDest, int iDestSize)
{
	char *psText = NULL;

	AnimationCFGs_t::iterator it = AnimationCFGs.find(psCFGFilename);
	if (it != AnimationCFGs.end())
	{
		psText = (*it).second;
	}
	else
	{
		// not found, so load it...
		//
		fileHandle_t f;
		int iLen = ri.FS_FOpenFileRead( psCFGFilename, &f, qfalse );
		if (iLen <= 0)
		{
			return 0;
		}

		psText = (char *) R_Malloc( iLen+1, TAG_ANIMATION_CFG, qfalse );

		ri.FS_Read( psText, iLen, f );
		psText[iLen] = '\0';
		ri.FS_FCloseFile( f );

		AnimationCFGs[psCFGFilename] = psText;
	}

	if (psText)	// sanity, but should always be NZ
	{
		if (psDest)
		{
			Q_strncpyz(psDest,psText,iDestSize);
		}

		return strlen(psText);
	}

	return 0;
}

// only called from devmapbsp, devmapall, or ...
//
void RE_AnimationCFGs_DeleteAll(void)
{
	for (AnimationCFGs_t::iterator it = AnimationCFGs.begin(); it != AnimationCFGs.end(); ++it)
	{
		char *psText = (*it).second;
		R_Free(psText);
	}

	AnimationCFGs.clear();
}

/*
===============
RE_SplitSkins
input = skinname, possibly being a macro for three skins
return= true if three part skins found
output= qualified names to three skins if return is true, undefined if false
===============
*/
bool RE_SplitSkins(const char *INname, char *skinhead, char *skintorso, char *skinlower)
{	//INname= "models/players/jedi_tf/|head01_skin1|torso01|lower01";
	if (strchr(INname, '|'))
	{
		char name[MAX_QPATH];
		strcpy(name, INname);
		char *p = strchr(name, '|');
		*p=0;
		p++;
		//fill in the base path
		strcpy (skinhead, name);
		strcpy (skintorso, name);
		strcpy (skinlower, name);

		//now get the the individual files

		//advance to second
		char *p2 = strchr(p, '|');
		if (!p2)
		{
			return false;
		}
		*p2=0;
		p2++;
		strcat (skinhead, p);
		strcat (skinhead, ".skin");


		//advance to third
		p = strchr(p2, '|');
		if (!p)
		{
			return false;
		}
		*p=0;
		p++;
		strcat (skintorso,p2);
		strcat (skintorso, ".skin");

		strcat (skinlower,p);
		strcat (skinlower, ".skin");

		return true;
	}
	return false;
}

// given a name, go get the skin we want and return
qhandle_t RE_RegisterIndividualSkin( const char *name , qhandle_t hSkin)
{
	skin_t		*skin;
	skinSurface_t	*surf;
	char		*text, *text_p;
	char		*token;
	char		surfName[MAX_QPATH];

	// load and parse the skin file
    ri.FS_ReadFile( name, (void **)&text );
	if ( !text ) {
		ri.Printf( PRINT_WARNING, "WARNING: RE_RegisterSkin( '%s' ) failed to load!\n", name );
		return 0;
	}

	assert (tr.skins[hSkin]);	//should already be setup, but might be an 3part append

	skin = tr.skins[hSkin];

	text_p = text;
	while ( text_p && *text_p ) {
		// get surface name
		token = CommaParse( &text_p );
		Q_strncpyz( surfName, token, sizeof( surfName ) );

		if ( !token[0] ) {
			break;
		}
		// lowercase the surface name so skin compares are faster
		Q_strlwr( surfName );

		if ( *text_p == ',' ) {
			text_p++;
		}

		if ( !strncmp( token, "tag_", 4 ) ) {	//these aren't in there, but just in case you load an id style one...
			continue;
		}

		// parse the shader name
		token = CommaParse( &text_p );

#ifndef JK2_MODE
		if ( !strcmp( &surfName[strlen(surfName)-4], "_off") )
		{
			if ( !strcmp( token ,"*off" ) )
			{
				continue;	//don't need these double offs
			}
			surfName[strlen(surfName)-4] = 0;	//remove the "_off"
		}
#endif
		if ( (unsigned)skin->numSurfaces >= ARRAY_LEN( skin->surfaces ) )
		{
			assert( ARRAY_LEN( skin->surfaces ) > (unsigned)skin->numSurfaces );
			ri.Printf( PRINT_WARNING, "WARNING: RE_RegisterSkin( '%s' ) more than %u surfaces!\n", name, (unsigned int)ARRAY_LEN(skin->surfaces) );
			break;
		}
		surf = skin->surfaces[ skin->numSurfaces ] = (skinSurface_t *) R_Hunk_Alloc( sizeof( *skin->surfaces[0] ), qtrue );
		Q_strncpyz( surf->name, surfName, sizeof( surf->name ) );
		surf->shader = R_FindShader( token, lightmapsNone, stylesDefault, qtrue );
		skin->numSurfaces++;
	}

	ri.FS_FreeFile( text );


	// never let a skin have 0 shaders
	if ( skin->numSurfaces == 0 ) {
		return 0;		// use default skin
	}

	return hSkin;
}

static void RE_AddSOF2OffSurface( skin_t *skin, const char *surfaceName ) {
	skinSurface_t *surf;

	if ( !surfaceName || !surfaceName[0] ) {
		return;
	}

	if ( (unsigned)skin->numSurfaces >= ARRAY_LEN( skin->surfaces ) ) {
		return;
	}

	surf = skin->surfaces[ skin->numSurfaces ] =
		(skinSurface_t *)R_Hunk_Alloc( sizeof( *skin->surfaces[0] ), qtrue );
	skin->numSurfaces++;
	Q_strncpyz( surf->name, surfaceName, sizeof( surf->name ) );
	Q_strlwr( surf->name );
	surf->shader = R_FindShader( "*off", lightmapsNone, stylesDefault, qtrue );
}

static void RE_SkipSOF2Block( const char **text_p ) {
	const char *token;
	int depth;

	token = COM_ParseExt( text_p, qtrue );
	if ( strcmp( token, "{" ) ) {
		return;
	}

	depth = 1;
	while ( depth > 0 ) {
		token = COM_ParseExt( text_p, qtrue );
		if ( !token[0] ) {
			return;
		}
		if ( !strcmp( token, "{" ) ) {
			depth++;
		}
		else if ( !strcmp( token, "}" ) ) {
			depth--;
		}
	}
}

static void RE_ParseSOF2SurfaceList( const char **text_p, skin_t *skin ) {
	const char *token;

	token = COM_ParseExt( text_p, qtrue );
	if ( strcmp( token, "{" ) ) {
		return;
	}

	while ( 1 ) {
		token = COM_ParseExt( text_p, qtrue );
		if ( !token[0] || !strcmp( token, "}" ) ) {
			return;
		}

		if ( Q_stricmpn( token, "name", 4 ) == 0 ) {
			token = COM_ParseExt( text_p, qtrue );
			RE_AddSOF2OffSurface( skin, token );
		}
		else if ( !strcmp( token, "{" ) ) {
			RE_SkipSOF2Block( text_p );
		}
	}
}

static void RE_AddSOF2MaterialSurface( skin_t *skin, const char *surfaceName, const char *shaderName ) {
	skinSurface_t *surf;
	static int sof2MaterialLogCount = 0;

	if ( !surfaceName || !surfaceName[0] || !shaderName || !shaderName[0] ) {
		return;
	}

	if ( (unsigned)skin->numSurfaces >= ARRAY_LEN( skin->surfaces ) ) {
		return;
	}

	surf = skin->surfaces[ skin->numSurfaces ] =
		(skinSurface_t *)R_Hunk_Alloc( sizeof( *skin->surfaces[0] ), qtrue );
	skin->numSurfaces++;
	Q_strncpyz( surf->name, surfaceName, sizeof( surf->name ) );
	Q_strlwr( surf->name );
	surf->shader = R_FindShader( shaderName, lightmapsNone, stylesDefault, qtrue );

	if ( sof2MaterialLogCount < 64 ) {
		ri.Printf( PRINT_ALL, "[SOF2 skin] surface='%s' shader='%s' resolved='%s'\n",
			surf->name,
			shaderName,
			surf->shader ? surf->shader->name : "<null>" );
		sof2MaterialLogCount++;
	}
}

static void RE_ParseSOF2MaterialGroup( const char **text_p, char *shaderName, int shaderNameSize ) {
	const char *token;

	token = COM_ParseExt( text_p, qtrue );
	if ( strcmp( token, "{" ) ) {
		return;
	}

	while ( 1 ) {
		token = COM_ParseExt( text_p, qtrue );
		if ( !token[0] || !strcmp( token, "}" ) ) {
			return;
		}

		if ( !Q_stricmp( token, "shader1" ) || !Q_stricmp( token, "texture1" ) ) {
			token = COM_ParseExt( text_p, qtrue );
			if ( token[0] && !shaderName[0] ) {
				Q_strncpyz( shaderName, token, shaderNameSize );
			}
		}
		else if ( !strcmp( token, "{" ) ) {
			RE_SkipSOF2Block( text_p );
		}
	}
}

static void RE_ParseSOF2Material( const char **text_p, skin_t *skin ) {
	const char *token;
	char materialName[MAX_QPATH];
	char shaderName[MAX_QPATH];

	materialName[0] = '\0';
	shaderName[0] = '\0';

	token = COM_ParseExt( text_p, qtrue );
	if ( strcmp( token, "{" ) ) {
		return;
	}

	while ( 1 ) {
		token = COM_ParseExt( text_p, qtrue );
		if ( !token[0] || !strcmp( token, "}" ) ) {
			break;
		}

		if ( !Q_stricmp( token, "name" ) ) {
			token = COM_ParseExt( text_p, qtrue );
			Q_strncpyz( materialName, token, sizeof( materialName ) );
		}
		else if ( !Q_stricmp( token, "group" ) ) {
			RE_ParseSOF2MaterialGroup( text_p, shaderName, sizeof( shaderName ) );
		}
		else if ( !strcmp( token, "{" ) ) {
			RE_SkipSOF2Block( text_p );
		}
	}

	RE_AddSOF2MaterialSurface( skin, materialName, shaderName );
}

static qhandle_t RE_RegisterIndividualG2Skin( const char *name, qhandle_t hSkin ) {
	skin_t *skin;
	char *text;
	const char *text_p;
	const char *token;
	int originalNumSurfaces;

	ri.FS_ReadFile( name, (void **)&text );
	if ( !text ) {
		ri.Printf( PRINT_WARNING, "WARNING: RE_RegisterSkin( '%s' ) failed to load!\n", name );
		return 0;
	}

	assert( tr.skins[hSkin] );
	skin = tr.skins[hSkin];
	originalNumSurfaces = skin->numSurfaces;
	text_p = text;
	COM_BeginParseSession( name );

	while ( 1 ) {
		token = COM_ParseExt( &text_p, qtrue );
		if ( !token[0] ) {
			break;
		}

		if ( !Q_stricmp( token, "material" ) ) {
			RE_ParseSOF2Material( &text_p, skin );
			continue;
		}

		if ( Q_stricmp( token, "prefs" ) ) {
			continue;
		}

		token = COM_ParseExt( &text_p, qtrue );
		if ( strcmp( token, "{" ) ) {
			continue;
		}

		while ( 1 ) {
			token = COM_ParseExt( &text_p, qtrue );
			if ( !token[0] || !strcmp( token, "}" ) ) {
				break;
			}

			if ( !Q_stricmp( token, "surfaces_off" ) || !Q_stricmp( token, "surfaces_offnochildren" ) ) {
				RE_ParseSOF2SurfaceList( &text_p, skin );
			}
			else {
				RE_SkipSOF2Block( &text_p );
			}
		}
	}

	COM_EndParseSession();
	ri.FS_FreeFile( text );

	if ( skin->numSurfaces == 0 ) {
		// Keep a non-zero handle for SOF2 custom skins even when they only carry material swaps.
		skin->surfaces[0] = (skinSurface_t *)R_Hunk_Alloc( sizeof( *skin->surfaces[0] ), qtrue );
		Q_strncpyz( skin->surfaces[0]->name, "__sof2_g2skin__", sizeof( skin->surfaces[0]->name ) );
		skin->surfaces[0]->shader = tr.defaultShader;
		skin->numSurfaces = 1;
	}

	ri.Printf( PRINT_ALL, "[SOF2 skin] register '%s' added=%d total=%d\n",
		name, skin->numSurfaces - originalNumSurfaces, skin->numSurfaces );

	return hSkin;
}

/*
===============
RE_RegisterSkin

===============
*/
qhandle_t RE_RegisterSkin( const char *name) {
	qhandle_t	hSkin;
	skin_t		*skin;
	char		resolvedName[MAX_QPATH];
	const char	*skinName = name;

//	if (!cls.cgameStarted && !cls.uiStarted)
//	{
		//rww - added uiStarted exception because we want ghoul2 models in the menus.
		// gwg well we need our skins to set surfaces on and off, so we gotta get em
		//return 1;	// cope with Ghoul2's calling-the-renderer-before-its-even-started hackery, must be any NZ amount here to trigger configstring setting
//	}

	if (!tr.numSkins)
	{
		R_InitSkins(); //make sure we have numSkins set to at least one.
	}

	if ( !name || !name[0] ) {
		Com_Printf( "Empty name passed to RE_RegisterSkin\n" );
		return 0;
	}

	if ( RE_ResolveSOF2G2Skin( name, resolvedName, sizeof( resolvedName ) ) ) {
		skinName = resolvedName;
	}

	if ( strlen( skinName ) >= MAX_QPATH ) {
		Com_Printf( "Skin name exceeds MAX_QPATH\n" );
		return 0;
	}

	// see if the skin is already loaded
	for ( hSkin = 1; hSkin < tr.numSkins ; hSkin++ ) {
		skin = tr.skins[hSkin];
		if ( !Q_stricmp( skin->name, skinName ) ) {
			if( skin->numSurfaces == 0 ) {
				return 0;		// default skin
			}
			return hSkin;
		}
	}

	if ( tr.numSkins == MAX_SKINS )	{
		ri.Printf( PRINT_WARNING, "WARNING: RE_RegisterSkin( '%s' ) MAX_SKINS hit\n", skinName );
		return 0;
	}
	// allocate a new skin
	tr.numSkins++;
	skin = (skin_t*) R_Hunk_Alloc( sizeof( skin_t ), qtrue );
	tr.skins[hSkin] = skin;
	Q_strncpyz( skin->name, skinName, sizeof( skin->name ) );	//always make one so it won't search for it again

	// If not a .skin file, load as a single shader	- then return
	if ( strlen( skinName ) < 5 || strcmp( skinName + strlen( skinName ) - 5, ".skin" ) ) {
#ifdef JK2_MODE
		skin->numSurfaces = 1;
		skin->surfaces[0] = (skinSurface_t *) R_Hunk_Alloc( sizeof(skin->surfaces[0]), qtrue );
		skin->surfaces[0]->shader = R_FindShader( skinName, lightmapsNone, stylesDefault, qtrue );
		return hSkin;
#endif
/*		skin->numSurfaces = 1;
		skin->surfaces[0] = (skinSurface_t *) R_Hunk_Alloc( sizeof(skin->surfaces[0]), qtrue );
		skin->surfaces[0]->shader = R_FindShader( skinName, lightmapsNone, stylesDefault, qtrue );
		return hSkin;
*/
	}

	char skinhead[MAX_QPATH]={0};
	char skintorso[MAX_QPATH]={0};
	char skinlower[MAX_QPATH]={0};
	if ( RE_SplitSkins(skinName, (char*)&skinhead, (char*)&skintorso, (char*)&skinlower ) )
	{//three part
		hSkin = RE_RegisterIndividualSkin(skinhead, hSkin);
		if (hSkin && strcmp(skinhead, skintorso))
		{
			hSkin = RE_RegisterIndividualSkin(skintorso, hSkin);
		}

		if (hSkin && strcmp(skinhead, skinlower) && strcmp(skintorso, skinlower))
		{
			hSkin = RE_RegisterIndividualSkin(skinlower, hSkin);
		}
	}
	else
	{//single skin
		if ( !Q_stricmp( COM_GetExtension( skinName ), "g2skin" ) ) {
			hSkin = RE_RegisterIndividualG2Skin( skinName, hSkin );
		}
		else {
			hSkin = RE_RegisterIndividualSkin(skinName, hSkin);
		}
	}
	return(hSkin);
}



/*
===============
R_InitSkins
===============
*/
void	R_InitSkins( void ) {
	skin_t		*skin;

	tr.numSkins = 1;

	// make the default skin have all default shaders
	skin = tr.skins[0] = (skin_t*) R_Hunk_Alloc( sizeof( skin_t ), qtrue );
	Q_strncpyz( skin->name, "<default skin>", sizeof( skin->name )  );
	skin->numSurfaces = 1;
	skin->surfaces[0] = (skinSurface_t *) R_Hunk_Alloc( sizeof( *skin->surfaces[0] ), qtrue );
	skin->surfaces[0]->shader = tr.defaultShader;
}

/*
===============
R_GetSkinByHandle
===============
*/
skin_t	*R_GetSkinByHandle( qhandle_t hSkin ) {
	if ( hSkin < 1 || hSkin >= tr.numSkins ) {
		return tr.skins[0];
	}
	return tr.skins[ hSkin ];
}

/*
===============
R_SkinList_f
===============
*/
void	R_SkinList_f (void) {
	int			i, j;
	skin_t		*skin;

	ri.Printf (PRINT_ALL, "------------------\n");

	for ( i = 0 ; i < tr.numSkins ; i++ ) {
		skin = tr.skins[i];
		ri.Printf( PRINT_ALL, "%3i:%s\n", i, skin->name );
		for ( j = 0 ; j < skin->numSurfaces ; j++ ) {
			ri.Printf( PRINT_ALL, "       %s = %s\n",
				skin->surfaces[j]->name, skin->surfaces[j]->shader->name );
		}
	}
	ri.Printf (PRINT_ALL, "------------------\n");
}
