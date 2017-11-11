#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "game.h"
#include "player.h"

#define Ent_IsValidEdict( e )	( e && !e->free )

// stop any actions with players
static cvar_t mp_enttools_players = { "mp_enttools_players", "0", FCVAR_SERVER };
// prevent ent_fire with entities not created with enttools
static cvar_t mp_enttools_lockmapentities = { "mp_enttools_lockmapentities", "0", FCVAR_SERVER };
static cvar_t mp_enttools_checkowner = { "mp_enttools_checkowner", "0", FCVAR_SERVER };
bool Q_stricmpext( const char *pattern, const char *text );

static bool Q_starcmp( const char *pattern, const char *text )
{
	char		c, c1;
	const char	*p = pattern, *t = text;

	while(( c = *p++ ) == '?' || c == '*' )
	{
		if( c == '?' && *t++ == '\0' )
			return false;
	}

	if( c == '\0' ) return true;

	for( c1 = (( c == '\\' ) ? *p : c ); ; )
	{
		if( tolower( *t ) == c1 && Q_stricmpext( p - 1, t ))
			return true;
		if( *t++ == '\0' ) return false;
	}
}

bool Q_stricmpext( const char *pattern, const char *text )
{
	char	c;

	while(( c = *pattern++ ) != '\0' )
	{
		switch( c )
		{
		case '?':
			if( *text++ == '\0' )
				return false;
			break;
		case '\\':
			if( tolower( *pattern++ ) != tolower( *text++ ))
				return false;
			break;
		case '*':
			return Q_starcmp( pattern, text );
		default:
			if( tolower( c ) != tolower( *text++ ))
				return false;
		}
	}
	return true;
}

bool Q_isdigit( const char *str )
{
	if( str && *str )
	{
		while( isdigit( *str )) str++;
		if( !*str ) return true;
	}
	return false;
}

typedef struct entblacklist_s
{
	struct entblacklist_s *next;
	char pattern[32];
	int limit;
	int behaviour;
} entblacklist_t;

entblacklist_t *entblacklist;

void Ent_AddToBlacklist_f( void )
{
	if( CMD_ARGC() != 4 )
	{
		ALERT( at_console, "Usage: mp_enttools_blacklist <pattern> <per minute limit> <behaviour (0 - block, 1 - kick, 2 - ban)>\n"  );
	}

	entblacklist_t *node = (entblacklist_t *)malloc( sizeof( entblacklist_t ) );

	node->next = entblacklist;
	strncpy( node->pattern, CMD_ARGV(1), 31 );
	node->pattern[32] = 0;
	node->limit = atoi( CMD_ARGV(2) );
	node->behaviour = atoi( CMD_ARGV( 3 ) );
	entblacklist = node;
}

bool Ent_CheckFire( edict_t *player, edict_t *ent, const char *command )
{
	if( !mp_enttools_players.value && ENTINDEX( ent ) < gpGlobals->maxClients + 1 )
		return false;

	CBaseEntity *pEntity = CBaseEntity::Instance( ent );

	if( pEntity )
	{
		if( mp_enttools_lockmapentities.value && !pEntity->enttools_data.enttools )
			return false;

		// only if player online
		if( mp_enttools_checkowner.value == 1 )
		{
			if( GGM_PlayerByID( pEntity->enttools_data.ownerid ) )
				return !strcmp( pEntity->enttools_data.ownerid, GGM_GetPlayerID( player ) );
		}

		if( mp_enttools_checkowner.value == 2 )
		{
				return !strcmp( pEntity->enttools_data.ownerid, GGM_GetPlayerID( player ) );
		}
	}

	return true;
}

bool Ent_CheckCreate( edict_t *player, const char *classname )
{
	CBasePlayer *p = (CBasePlayer*)CBaseEntity::Instance(player);
	entblacklist_t *node;

	if( !p )
		return false;

	if( p->gravgunmod_data.m_flEntScope > 1 )
		return false;

	if( gpGlobals->time - p->gravgunmod_data.m_flEntTime > 60 )
	{
		p->gravgunmod_data.m_flEntTime = gpGlobals->time;
		p->gravgunmod_data.m_flEntScope = 0;
	}

	for( node = entblacklist; node; node = node->next )
	{
		if( Q_stricmpext(node->pattern, classname ) )
		{
			if( !node->limit || ( p->gravgunmod_data.m_flEntScope + 1.0f / (float)node->limit > 1 ) )
			{
				// remove all created entities
				Ent_RunGC( false, true, GGM_GetPlayerID( player ) );

				if( node->behaviour == 2 )
				{
					SERVER_COMMAND( UTIL_VarArgs("banid 0 #%d kick\nwriteid\n", GETPLAYERUSERID( player ) ) );
				}
				else if( node->behaviour == 1 )
				{
					SERVER_COMMAND( UTIL_VarArgs("kick #%d\nwriteid\n", GETPLAYERUSERID( player ) ) );
				}
				return false;
			}
			p->gravgunmod_data.m_flEntScope += 1.0f / (float)node->limit;
		}
	}

	return true;
}

void Ent_ClientPrintf( edict_t *player, const char *format, ... )
{
	va_list	argptr;
	char string[256];

	va_start( argptr, format );
	int len = vsnprintf( string, 256, format, argptr );
	va_end( argptr );
	string[len] = 0;

	MESSAGE_BEGIN( MSG_ONE, 8, g_vecZero, player ); // svc_print
		WRITE_BYTE( 0 ); // PRINT_LOW
		WRITE_STRING( string );
	MESSAGE_END();
}


static edict_t *Ent_GetCrossEnt( edict_t *player )
{
	edict_t *ent = g_engfuncs.pfnPEntityOfEntIndex(1);
	edict_t *closest = NULL;
	float flMaxDot = 0.94;
	Vector forward;
	int i;
	float maxLen = 1000;

	UTIL_MakeVectorsPrivate( player->v.v_angle, forward, NULL, NULL );

	Vector viewPos = player->v.origin + player->v.view_ofs;

	// find bmodels by trace
	{
		TraceResult tr;

		UTIL_TraceLine(viewPos, viewPos + forward * 1000, missile, player, &tr);
		closest = tr.pHit;
		maxLen = (viewPos - tr.vecEndPos).Length() + 30;
	}

	// check untraceable entities
	for ( i = 1; i < gpGlobals->maxEntities; i++, ent++ )
	{
		Vector vecLOS;
		Vector vecOrigin;
		float flDot, traceLen;

		if( ent->free )
			continue;

		if( ent->v.solid == SOLID_BSP || ent->v.movetype == MOVETYPE_PUSHSTEP )
			continue; //bsp models will be found by trace later

		// do not touch following weapons
		if( ent->v.movetype == MOVETYPE_FOLLOW )
			continue;

		if( ent == player )
			continue;

		vecOrigin = VecBModelOrigin(&ent->v);

		vecLOS = vecOrigin - viewPos;
		traceLen = vecLOS.Length();

		if( traceLen > maxLen )
			continue;

		vecLOS = UTIL_ClampVectorToBox(vecLOS, ent->v.size * 0.5);

		flDot = DotProduct(vecLOS , forward);
		if ( flDot <= flMaxDot )
			continue;

		TraceResult tr;
		UTIL_TraceLine(viewPos, vecOrigin, missile, player, &tr);
		if( (tr.vecEndPos - viewPos).Length() + 30 < traceLen )
			continue;
		closest = ent, flMaxDot = flDot;
	}

	return closest;
}

/*
===============
Ent_List_f

Print list of entities to client
===============
*/
void Ent_List_f( edict_t *player )
{
	edict_t	*ent = NULL;
	int	i;

	for( i = 0; i < gpGlobals->maxEntities; i++ )
	{
		ent = g_engfuncs.pfnPEntityOfEntIndex( i );
		if( !Ent_IsValidEdict( ent )) continue;

		// filter by string
		if( CMD_ARGC() > 1 )
			if( !Q_stricmpext( CMD_ARGV( 1 ), STRING( ent->v.classname ) ) && !Q_stricmpext( CMD_ARGV( 1 ), STRING( ent->v.targetname ) ) )
				continue;
		Vector borigin = VecBModelOrigin( &ent->v );

		Ent_ClientPrintf( player, "%5i origin: %.f %.f %.f", i, ent->v.origin[0], ent->v.origin[1], ent->v.origin[2] );
		Ent_ClientPrintf( player, "%5i borigin: %.f %.f %.f", i, borigin[0], borigin[1], borigin[2] );

		if( ent->v.classname )
			Ent_ClientPrintf( player, ", class: %s", STRING( ent->v.classname ));

		if( ent->v.globalname )
			Ent_ClientPrintf( player, ", global: %s", STRING( ent->v.globalname ));

		if( ent->v.targetname )
			Ent_ClientPrintf( player, ", name: %s", STRING( ent->v.targetname ));

		if( ent->v.target )
			Ent_ClientPrintf( player, ", target: %s", STRING( ent->v.target ));

		if( ent->v.model )
			Ent_ClientPrintf( player, ", model: %s", STRING( ent->v.model ));

		Ent_ClientPrintf( player, "\n" );
	}
}



edict_t *Ent_FindSingle( edict_t *player, const char *pattern )
{
	edict_t	*ent = NULL;
	int	i = 0;

	if( Q_isdigit( pattern ) )
	{
		i = atoi( pattern );

		if( i >= gpGlobals->maxEntities )
			return NULL;
	}
	else if( !stricmp( pattern, "!cross" ) )
	{
		ent = Ent_GetCrossEnt( player );

		if( !Ent_IsValidEdict( ent ) )
			return NULL;

		i = ENTINDEX( ent );
	}
	else if( pattern[0] == '!' ) // Check for correct instanse with !(num)_(serial)
	{
		const char *p = pattern + 1;
		i = atoi( p );

		while( isdigit(*p) ) p++;

		if( *p++ != '_' )
			return NULL;

		if( i >= gpGlobals->maxEntities )
			return NULL;

		ent = g_engfuncs.pfnPEntityOfEntIndex( i );

		if( ent->serialnumber != atoi( p ) )
			return NULL;
	}
	else
	{
		for( i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
		{
			ent = g_engfuncs.pfnPEntityOfEntIndex( i );

			if( !Ent_IsValidEdict( ent ) )
				continue;

			if( Q_stricmpext( pattern, STRING( ent->v.targetname ) ) )
				break;
		}
	}

	ent = g_engfuncs.pfnPEntityOfEntIndex( i );

	if( !Ent_IsValidEdict( ent ) )
		return NULL;

	return ent;
}


/*
===============
Ent_Info_f

Print specified entity information to client
===============
*/
void Ent_Info_f( edict_t *player )
{
	edict_t	*ent = NULL;

	if( CMD_ARGC() != 2 )
	{
		Ent_ClientPrintf( player, "Use ent_info <index|name|inst>\n" );
		return;
	}

	ent = Ent_FindSingle( player,  CMD_ARGV( 1 ) );

	if( !Ent_IsValidEdict( ent )) return;

	Vector borigin = VecBModelOrigin( &ent->v );

	Ent_ClientPrintf( player, "origin: %.f %.f %.f\n", ent->v.origin[0], ent->v.origin[1], ent->v.origin[2] );

	Ent_ClientPrintf( player, "angles: %.f %.f %.f\n", ent->v.angles[0], ent->v.angles[1], ent->v.angles[2] );

	Ent_ClientPrintf( player, "borigin: %.f %.f %.f\n", borigin[0], borigin[1], borigin[2] );

	if( ent->v.classname )
		Ent_ClientPrintf( player, "class: %s\n", STRING( ent->v.classname ));

	if( ent->v.globalname )
		Ent_ClientPrintf( player, "global: %s\n", STRING( ent->v.globalname ));

	if( ent->v.targetname )
		Ent_ClientPrintf( player, "name: %s\n", STRING( ent->v.targetname ));

	if( ent->v.target )
		Ent_ClientPrintf( player, "target: %s\n", STRING( ent->v.target ));

	if( ent->v.model )
		Ent_ClientPrintf( player, "model: %s\n", STRING( ent->v.model ));

	Ent_ClientPrintf( player, "health: %.f\n", ent->v.health );

	if( ent->v.gravity != 1.0f )
		Ent_ClientPrintf( player, "gravity: %.2f\n", ent->v.gravity );

	Ent_ClientPrintf( player, "movetype: %d\n", ent->v.movetype );

	Ent_ClientPrintf( player, "rendermode: %d\n", ent->v.rendermode );
	Ent_ClientPrintf( player, "renderfx: %d\n", ent->v.renderfx );
	Ent_ClientPrintf( player, "renderamt: %f\n", ent->v.renderamt );
	Ent_ClientPrintf( player, "rendercolor: %f %f %f\n", ent->v.rendercolor[0], ent->v.rendercolor[1], ent->v.rendercolor[2] );

	Ent_ClientPrintf( player, "maxspeed: %f\n", ent->v.maxspeed );

	if( ent->v.solid )
		Ent_ClientPrintf( player, "solid: %d\n", ent->v.solid );
	Ent_ClientPrintf( player, "flags: 0x%x\n", ent->v.flags );
	Ent_ClientPrintf( player, "spawnflags: 0x%x\n", ent->v.spawnflags );
}

void Ent_SendVars( edict_t *player, edict_t *ent )
{
	if( !ent )
		return;

	CLIENT_COMMAND( player, UTIL_VarArgs( "set ent_last_name \"%s\"\n", STRING( ent->v.targetname ) ));
	CLIENT_COMMAND( player, UTIL_VarArgs( "set ent_last_num %i\n", ENTINDEX( ent ) ));
	CLIENT_COMMAND( player, UTIL_VarArgs( "set ent_last_inst !%i_%i\n", ENTINDEX( ent ), ent->serialnumber ));
	CLIENT_COMMAND( player, UTIL_VarArgs( "set ent_last_origin \"%f %f %f\"\n", ent->v.origin[0], ent->v.origin[1], ent->v.origin[2]));
	CLIENT_COMMAND( player, UTIL_VarArgs( "set ent_last_class \"%s\"\n", STRING( ent->v.classname )));
	CLIENT_COMMAND( player, "ent_getvars_cb\n" );
}

void Ent_GetVars_f( edict_t *player )
{
	edict_t *ent = NULL;

	if( CMD_ARGC() != 2 )
	{
		Ent_ClientPrintf( player, "Use ent_getvars <index|name|inst>\n" );
		return;
	}

	ent = Ent_FindSingle( player, CMD_ARGV( 1 ) );
	if( CMD_ARGC() )
	if( !Ent_IsValidEdict( ent )) return;
	Ent_SendVars( player,  ent );
}

/*
===============
Ent_Fire_f

Perform some actions
===============
*/
void Ent_Fire_f( edict_t *player )
{
	edict_t	*ent = NULL;
	int	i = 1, count = 0;
	qboolean single; // true if user specified something that match single entity

	if( CMD_ARGC() < 3 )
	{
		Ent_ClientPrintf( player, "Use ent_fire <index||pattern> <command> [<values>]\n"
			"Use ent_fire 0 help to get command list\n" );
		return;
	}

	if( ( single = Q_isdigit( CMD_ARGV( 1 ) ) ) )
	{
		i = atoi( CMD_ARGV( 1 ) );

		if( i < 0 || i >= gpGlobals->maxEntities )
			return;

		ent = g_engfuncs.pfnPEntityOfEntIndex( i );
	}
	else if( ( single = !stricmp( CMD_ARGV( 1 ), "!cross" ) ) )
	{
		ent = Ent_GetCrossEnt( player );

		if( !Ent_IsValidEdict( ent ) )
			return;

		i = ENTINDEX( ent );
	}
	else if( ( single = ( CMD_ARGV( 1 )[0] == '!') ) ) // Check for correct instanse with !(num)_(serial)
	{
		const char *cmd = CMD_ARGV( 1 ) + 1;
		i = atoi( cmd );

		while( isdigit( *cmd ) ) cmd++;

		if( *cmd++ != '_' )
			return;

		if( i < 0 || i >= gpGlobals->maxEntities )
			return;

		ent = g_engfuncs.pfnPEntityOfEntIndex( i );
		if( ent->serialnumber != atoi( cmd ) )
			return;
	}
	else
	{
		i = gpGlobals->maxClients + 1;
	}

	for( ; ( i <  gpGlobals->maxEntities ) && ( count < mp_enttools_maxfire.value ); i++ )
	{
		ent = g_engfuncs.pfnPEntityOfEntIndex( i );
		if( !Ent_IsValidEdict( ent ))
		{
			// Ent_ClientPrintf( player, "Got invalid entity\n" );
			if( single )
				break;
			continue;
		}

		// if user specified not a number, try find such entity
		if( !single )
		{
			if( !Q_stricmpext( CMD_ARGV( 1 ), STRING( ent->v.targetname ) ) && !Q_stricmpext( CMD_ARGV( 1 ), STRING( ent->v.classname ) ))
				continue;
		}

		if( !Ent_CheckFire( player, ent, CMD_ARGV( 2 ) ) )
			return;

		Ent_ClientPrintf( player, "entity %i\n", i );

		count++;

		if( !stricmp( CMD_ARGV( 2 ), "health" ) )
			ent->v.health = atoi( CMD_ARGV ( 3 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "gravity" ) )
			ent->v.gravity = atof( CMD_ARGV ( 3 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "movetype" ) )
			ent->v.movetype = atoi( CMD_ARGV ( 3 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "solid" ) )
			ent->v.solid = atoi( CMD_ARGV ( 3 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "rename" ) )
			ent->v.targetname = ALLOC_STRING( CMD_ARGV ( 3 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "settarget" ) )
			ent->v.target = ALLOC_STRING( CMD_ARGV ( 3 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "setmodel" ) )
			SET_MODEL( ent, CMD_ARGV( 3 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "set" ) )
		{
			char keyname[256];
			char value[256];
			KeyValueData	pkvd;
			if( CMD_ARGC() != 5 )
				return;
			pkvd.szClassName = (char*)STRING( ent->v.classname );
			strncpy( keyname, CMD_ARGV( 3 ), 256 );
			strncpy( value, CMD_ARGV( 4 ), 256 );
			keyname[255] = value[255] = 0;
			pkvd.szKeyName = keyname;
			pkvd.szValue = value;
			pkvd.fHandled = false;
			DispatchKeyValue( ent, &pkvd );
			if( pkvd.fHandled )
				Ent_ClientPrintf( player, "value set successfully!\n" );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "touch" ) )
		{
			if( CMD_ARGC() == 4 )
			{
				edict_t *other = Ent_FindSingle( player,  CMD_ARGV( 3 ) );
				if( other && other->pvPrivateData )
					DispatchTouch( ent, other  );
			}
			else
				DispatchTouch( ent, player );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "use" ) )
		{
			if( CMD_ARGC() == 4 )
			{
				edict_t *other = Ent_FindSingle( player,  CMD_ARGV( 3 ) );
				if( other && other->pvPrivateData )
					DispatchUse( ent, other );
			}
			else
				DispatchUse( ent, player );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "movehere" ) )
			UTIL_SetOrigin( &ent->v, player->v.origin + Vector( 100 * cos( player->v.angles[1]/180*M_PI ), 100 * sin( player->v.angles[1]/180*M_PI), 25 ) );
		else if( !stricmp( CMD_ARGV( 2 ), "drop2floor" ) )
				DROP_TO_FLOOR( ent );
		else if( !stricmp( CMD_ARGV( 2 ), "moveup" ) )
		{
			float dist = 25;

			if( CMD_ARGC() >= 4 )
				dist = atof( CMD_ARGV( 3 ) );

			ent->v.origin[2] +=  dist;

			if( CMD_ARGC() >= 5 )
				ent->v.origin = ent->v.origin + Vector( cos( player->v.angles[1]/180*M_PI ), sin( player->v.angles[1]/180*M_PI), 0 ) * atof( CMD_ARGV( 4 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "becomeowner" ) )
		{
			if( CMD_ARGC() == 4 )
				ent->v.owner = Ent_FindSingle( player,  CMD_ARGV( 3 ) );
			else
				ent->v.owner = player;
		}
		else if( !stricmp( CMD_ARGV( 2 ), "becomeenemy" ) )
		{
			if( CMD_ARGC() == 4 )
				ent->v.enemy = Ent_FindSingle( player,  CMD_ARGV( 3 ) );
			else
				ent->v.enemy = player;
		}
		else if( !stricmp( CMD_ARGV( 2 ), "becomeaiment" ) )
		{
			if( CMD_ARGC() == 4 )
				ent->v.aiment= Ent_FindSingle( player,  CMD_ARGV( 3 ) );
			else
				ent->v.aiment = player;
		}
		else if( !stricmp( CMD_ARGV( 2 ), "hullmin" ) )
		{
			if( CMD_ARGC() != 6 )
				return;
			ent->v.mins[0] = atof( CMD_ARGV( 3 ) );
			ent->v.mins[1] = atof( CMD_ARGV( 4 ) );
			ent->v.mins[2] = atof( CMD_ARGV( 5 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "hullmax" ) )
		{
			if( CMD_ARGC() != 6 )
				return;
			ent->v.maxs[0] = atof( CMD_ARGV( 3 ) );
			ent->v.maxs[1] = atof( CMD_ARGV( 4 ) );
			ent->v.maxs[2] = atof( CMD_ARGV( 5 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "rendercolor" ) )
		{
			if( CMD_ARGC() != 6 )
				return;
			ent->v.rendercolor[0] = atof( CMD_ARGV( 3 ) );
			ent->v.rendercolor[1] = atof( CMD_ARGV( 4 ) );
			ent->v.rendercolor[2] = atof( CMD_ARGV( 5 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "renderamt" ) )
		{
			ent->v.renderamt = atof( CMD_ARGV( 3 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "renderfx" ) )
		{
			ent->v.renderfx = atoi( CMD_ARGV( 3 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "rendermode" ) )
		{
			ent->v.rendermode = atoi( CMD_ARGV( 3 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "angles" ) )
		{
			ent->v.angles[0] = atof( CMD_ARGV( 3 ) );
			ent->v.angles[1] = atof( CMD_ARGV( 4 ) );
			ent->v.angles[2] = atof( CMD_ARGV( 5 ) );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "setflag" ) )
		{
			ent->v.flags |= 1U << atoi( CMD_ARGV ( 3 ) );
			Ent_ClientPrintf( player, "flags set to 0x%x\n", ent->v.flags );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "clearflag" ) )
		{
			ent->v.flags &= ~( 1U << atoi( CMD_ARGV ( 3 ) ) );
			Ent_ClientPrintf( player, "flags set to 0x%x\n", ent->v.flags );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "setspawnflag" ) )
		{
			ent->v.spawnflags |= 1U << atoi( CMD_ARGV ( 3 ) );
			Ent_ClientPrintf( player, "spawnflags set to 0x%x\n", ent->v.spawnflags );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "clearspawnflag" ) )
		{
			ent->v.spawnflags &= ~( 1U << atoi( CMD_ARGV ( 3 ) ) );
			Ent_ClientPrintf( player, "spawnflags set to 0x%x\n", ent->v.flags );
		}
		else if( !stricmp( CMD_ARGV( 2 ), "help" ) )
		{
			Ent_ClientPrintf( player, "Availiavle commands:\n"
				"Set fields:\n"
				"        (Only set entity field, does not call any functions)\n"
				"    health\n"
				"    gravity\n"
				"    movetype\n"
				"    solid\n"
				"    rendermode\n"
				"    rendercolor (vector)\n"
				"    renderfx\n"
				"    renderamt\n"
				"    hullmin (vector)\n"
				"    hullmax (vector)\n"
				"Actions\n"
				"    rename: set entity targetname\n"
				"    settarget: set entity target (only targetnames)\n"
				"    setmodel: set entity model\n"
				"    set: set <key> <value> by server library\n"
				"        See game FGD to get list.\n"
				"        command takes two arguments\n"
				"    touch: touch entity by current player.\n"
				"    use: use entity by current player.\n"
				"    movehere: place entity in player fov.\n"
				"    drop2floor: place entity to nearest floor surface\n"
				"    moveup: move entity to 25 units up\n"
				"Flags:\n"
				"        (Set/clear specified flag bit, arg is bit number)\n"
				"    setflag\n"
				"    clearflag\n"
				"    setspawnflag\n"
				"    clearspawnflag\n"
			);
			return;
		}
		else
		{
			Ent_ClientPrintf( player, "Unknown command %s!\nUse \"ent_fire 0 help\" to list commands.\n", CMD_ARGV( 2 ) );
			return;
		}
		if( single )
			break;
	}
}

/*
===============
Ent_Create_f

Create new entity with specified name.
===============
*/
void Ent_Create_f( edict_t *player )
{
	edict_t	*ent = NULL;
	int	i = 0;
	string_t classname;

	if( CMD_ARGC() < 2 )
	{
		Ent_ClientPrintf( player, "Use ent_create <classname> <key1> <value1> <key2> <value2> ...\n" );
		return;
	}

	if( !Ent_CheckCreate( player, CMD_ARGV(1) ) )
		return;

	classname = ALLOC_STRING( CMD_ARGV( 1 ) );
	ent = CREATE_NAMED_ENTITY( classname );

	if( !ent )
	{
		Ent_ClientPrintf( player, "Invalid entity!\n" );
		return;
	}

	// choose default origin
	UTIL_SetOrigin( &ent->v, player->v.origin + Vector( 100 * cos( player->v.angles[1]/180*M_PI ), 100 * sin( player->v.angles[1]/180*M_PI), 25 ));

	// apply keyvales
	for( i = 2; i < CMD_ARGC() - 1; i++ )
	{
		KeyValueData	pkvd;

		// allow split keyvalues to prespawn and postspawn
		if( !strcmp( CMD_ARGV( i ), "|" ) )
		{
			i++;
			break;
		}

		pkvd.fHandled = false;
		pkvd.szClassName = (char*)STRING( ent->v.classname );
		pkvd.szKeyName = (char*)CMD_ARGV( i );
		i++;
		pkvd.szValue = (char*)CMD_ARGV( i );

		DispatchKeyValue( ent, &pkvd );
		if( pkvd.fHandled )
			Ent_ClientPrintf( player, "value \"%s\" set to \"%s\"!\n", pkvd.szKeyName, pkvd.szValue );
	}


	// set default targetname
	if( !ent->v.targetname )
	{
		char newname[256], clientname[256];
		int j;

		for( j = 0; j < 32; j++ )
		{
			char c = tolower( (STRING( player->v.netname ))[j] );
			if( c < 'a' || c > 'z' )
				c = '_';
			if( !(STRING( player->v.netname ))[j] )
			{
				clientname[j] = 0;
				break;
			}
			clientname[j] = c;
		}

		// generate name based on nick name and index
		snprintf( newname, 255,  "%s_%i_e%i", clientname, GETPLAYERUSERID( player ), ENTINDEX( ent ) );
		newname[255] = 0;

		// i know, it may break strict aliasing rules
		// but we will not lose anything in this case.
		//Q_strnlwr( newname, newname, 256 );
		ent->v.targetname = ALLOC_STRING( newname );
		Ent_SendVars( player,  ent );
	}

	Ent_ClientPrintf( player, "Created %i: %s, targetname %s\n", ENTINDEX( ent ), CMD_ARGV( 1 ), STRING( ent->v.targetname ) );

	DispatchSpawn( ent );

	// now drop entity to floor.
	g_engfuncs.pfnDropToFloor( ent );

	// force think. Otherwise given weapon may crash server if player touch it before.
	DispatchThink( ent );
	g_engfuncs.pfnDropToFloor( ent );

	// apply postspawn keyvales

	for( i = i; i < CMD_ARGC() - 1; i++ )
	{
		KeyValueData	pkvd;

		pkvd.fHandled = false;
		pkvd.szClassName = (char*)STRING( ent->v.classname );
		pkvd.szKeyName = (char*)CMD_ARGV( i );
		i++;
		pkvd.szValue = (char*)CMD_ARGV( i );
		DispatchKeyValue( ent, &pkvd );
		if( pkvd.fHandled )
			Ent_ClientPrintf( player, "value \"%s\" set to \"%s\"!\n", pkvd.szKeyName, pkvd.szValue );
	}

	CBaseEntity *entity = CBaseEntity::Instance( ent );
	if( entity )
	{
		entity->enttools_data.enttools = true;
		strcpy( entity->enttools_data.ownerid, GGM_GetPlayerID( player ) );
	}
}
typedef struct ucmd_s
{
	const char	*name;
	void		(*func)( edict_t *player );
} ucmd_t;

ucmd_t enttoolscmds[] =
{
{ "ent_list", Ent_List_f },
{ "ent_info", Ent_Info_f },
{ "ent_fire", Ent_Fire_f },
{ "ent_create", Ent_Create_f },
{ "ent_getvars", Ent_GetVars_f },
{ NULL, NULL }
};

bool Ent_ProcessClientCommand( edict_t *player )
{
	ucmd_t	*u;


	if( !mp_enttools_enable.value )
		return false;

	for( u = enttoolscmds; u->name; u++ )
	{
		if( !strcmp( CMD_ARGV( 0 ), u->name ))
		{
			ALERT( at_console, "enttools->%s(): %s\n", u->name, CMD_ARGS() );

			ALERT( at_logged, "\"%s<%i><%s><>\" performed: %s\n", STRING(player->v.netname),
						GETPLAYERUSERID(player), GETPLAYERAUTHID(player), g_engfuncs.pfnInfoKeyValue( g_engfuncs.pfnGetInfoKeyBuffer( player ), "ip" ), CMD_ARGS() );

			if( u->func ) u->func( player );
			return true;
		}
	}

	return false;
}

cvar_t mp_enttools_enable = { "mp_enttools_enable", "0", FCVAR_SERVER };
cvar_t mp_enttools_maxfire = { "mp_enttools_maxfire", "5", FCVAR_SERVER };

void ENT_RegisterCVars( void )
{
	CVAR_REGISTER( &mp_enttools_enable );
	CVAR_REGISTER( &mp_enttools_maxfire );
	CVAR_REGISTER( &mp_enttools_lockmapentities );
	CVAR_REGISTER( &mp_enttools_checkowner );
	CVAR_REGISTER( &mp_enttools_players );
	g_engfuncs.pfnAddServerCommand( "mp_enttools_addblacklist", Ent_AddToBlacklist_f );
}