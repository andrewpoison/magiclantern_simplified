/***
 Intervalometer operations and properties.
 
 @author Magic Lantern Team
 @copyright 2014
 @license GPL
 @module interval
 */

#include <dryos.h>
#include <string.h>
#include <shoot.h>

#include "lua_common.h"
#ifdef CONFIG_R6_LUA
#include "lua_r6.h"             /* M16.1 (spec m16/m161): the core facade r6_api_abi.h */
#endif

static int luaCB_interval_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Get/Set the interval time (in seconds).
    // @tfield int time
    if(!strcmp(key, "time")) lua_pushinteger(L, get_interval_time());
    /// Get the current number of pictures that have been taken.
    // @tfield int count readonly
    else if(!strcmp(key, "count")) lua_pushinteger(L, get_interval_count());
    /// Get whether or not the intervalometer is currently running.
    // @tfield bool running readonly
#ifdef CONFIG_R6_LUA
    /* M16.1 (spec m16/m161 5.6): R6 Interval (a series, a test, a Motion run, or queued) */
    else if(!strcmp(key, "running")) lua_pushboolean(L, r6_iv_running());
#else
    else if(!strcmp(key, "running")) lua_pushboolean(L, is_intervalometer_running());
#endif
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_interval_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if(!strcmp(key, "time"))
    {
        LUA_PARAM_INT(value, 3);
        set_interval_time(value);
    }
    else if(!strcmp(key, "running") || !strcmp(key, "count"))
    {
        return luaL_error(L, "'%s' is readonly!", key);
    }
    else
    {
        lua_rawset(L, 1);
    }
    return 0;
}

/***
 Stop the intervalometer
 @function stop
 */
#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 5.6, 5.1): the R6 commands Stop (never the toggle), Start, Test: ok, text */
static int lua_r6_iv_cmd(lua_State * L, int cmd)
{
    struct lua_r6_call k = { .op = LR6_CMD, .a = cmd };
    int r = lua_r6_do(L, &k);
    lua_pushboolean(L, r == R6API_OK);
    lua_pushstring(L, k.why);
    return 2;
}
static int luaCB_interval_stop(lua_State * L) { return lua_r6_iv_cmd(L, R6SC_IV_STOP); }
static int luaCB_interval_start(lua_State * L) { return lua_r6_iv_cmd(L, R6SC_IV_START); }
static int luaCB_interval_test(lua_State * L) { return lua_r6_iv_cmd(L, R6SC_IV_TEST); }
#else
static int luaCB_interval_stop(lua_State * L)
{
    intervalometer_stop();
    return 0;
}
#endif

static const char * lua_interval_fields[] =
{
    "time",
    "count",
    "running",
    NULL
};

#ifdef CONFIG_R6_LUA
static const luaL_Reg intervallib[] =
{
    { "stop", luaCB_interval_stop },
    { "start", luaCB_interval_start },
    { "test", luaCB_interval_test },
    { NULL, NULL }
};
#else
static const luaL_Reg intervallib[] =
{
    { "stop", luaCB_interval_stop },
    { NULL, NULL }
};
#endif

LUA_LIB(interval)
