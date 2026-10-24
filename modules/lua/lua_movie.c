/***
 Movie functions
 
 @author Magic Lantern Team
 @copyright 2014
 @license GPL
 @module movie
 */

#include <dryos.h>
#include <string.h>
#include <fps.h>
#include <shoot.h>
#include <property.h>

#include "lua_common.h"
#ifdef CONFIG_R6_LUA
#include "lua_r6.h"             /* M16.1 (spec m16/m161): the core facade r6_api_abi.h */
#endif

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 6.3): only with the mode dial at Video; one REC press / release pair, no wait (RECORDING is always 0 on the
 * R6: PROP_MVR_REC_START is denied); returns true, "sent, state unknown". The script remembers it (r6_script_end's hint, R-4) */
static int lua_r6_rec(lua_State * L, const char * what)
{
    struct lua_r6_call k = { .op = LR6_GUI, .a = R6SG_REC };
    if (lua_r6_do(L, &k)) return luaL_error(L, "%s: %s", what, k.why);
    lua_r6_flag(L, LUA_R6F_REC, 1);
    lua_pushboolean(L, 1);
    lua_pushstring(L, k.why);
    return 2;
}

static int luaCB_movie_start(lua_State* L) { return lua_r6_rec(L, "movie.start"); }
static int luaCB_movie_stop(lua_State* L) { return lua_r6_rec(L, "movie.stop"); }

static int luaCB_movie_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if(!strcmp(key, "recording"))
    {
        if (lua_r6_once(L, LUA_R6_ONCE_REC)) printf("[%s] movie.recording: unknown on R6 (M16.2)\n", lua_get_script_filename(L));
        lua_pushnil(L);
    }
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_movie_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if(!strcmp(key, "recording"))
    {
        LUA_PARAM_BOOL(value, 3);
        lua_r6_rec(L, value ? "movie.start" : "movie.stop");
        lua_pop(L, 2);
    }
    else
    {
        lua_rawset(L, 1);
    }
    return 0;
}
#else
/***
 Start recording a movie.
 @function start
 */
static int luaCB_movie_start(lua_State* L)
{
    if (!is_movie_mode())   return luaL_error(L, "Not in movie mode.");
    if (!lv)                return luaL_error(L, "Not in LiveView.");
    if (gui_menu_shown())   return luaL_error(L, "Please close ML menu.");
    if (RECORDING)          return luaL_error(L, "Already recording.");

    movie_start();
    return 0;
}

/***
 Stop recording a movie.
 @function stop
 */
static int luaCB_movie_stop(lua_State* L)
{
    if (!is_movie_mode())   return luaL_error(L, "Not in movie mode.");
    if (!lv)                return luaL_error(L, "Not in LiveView.");
    if (gui_menu_shown())   return luaL_error(L, "Please close ML menu.");
    if (!RECORDING)         return luaL_error(L, "Not recording.");

    movie_end();
    return 0;
}

static int luaCB_movie_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Get whether or not the camera is recording a movie.
    // @tfield bool recording readonly
    if(!strcmp(key, "recording")) lua_pushboolean(L, RECORDING);
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_movie_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if(!strcmp(key, "recording"))
    {
        LUA_PARAM_BOOL(value, 3);
        if(value) luaCB_movie_start(L);
        else luaCB_movie_stop(L);
    }
    else
    {
        lua_rawset(L, 1);
    }
    return 0;
}

#endif

static const char * lua_movie_fields[] =
{
    "recording",
    NULL
};

const luaL_Reg movielib[] =
{
    {"start", luaCB_movie_start},
    {"stop", luaCB_movie_stop},
    {NULL, NULL}
};

LUA_LIB(movie)
