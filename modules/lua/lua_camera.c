/***
 Basic camera operations and properties
 
 @author Magic Lantern Team
 @copyright 2014
 @license GPL
 @module camera
 */

#include <dryos.h>
#include <string.h>
#include <lens.h>
#include <shoot.h>
#include <bmp.h>
#include <imath.h>
#include <math.h>
#include <zebra.h>

#include "lua_common.h"
#ifdef CONFIG_R6_LUA
#include "lua_r6.h"             /* M16.1 (spec m16/m161 3.4, 4.1, 4.5, 6.1): the core facade r6_api_abi.h */
#endif

static int luaCB_shutter_index(lua_State * L);
static int luaCB_shutter_newindex(lua_State * L);
static int luaCB_shutter_tostring(lua_State * L);

static int luaCB_aperture_index(lua_State * L);
static int luaCB_aperture_newindex(lua_State * L);
static int luaCB_aperture_tostring(lua_State * L);

static int luaCB_min_aperture_index(lua_State * L);
static int luaCB_min_aperture_tostring(lua_State * L);
static int luaCB_max_aperture_index(lua_State * L);
static int luaCB_max_aperture_tostring(lua_State * L);

static int luaCB_iso_index(lua_State * L);
static int luaCB_iso_newindex(lua_State * L);
static int luaCB_iso_tostring(lua_State * L);

static int luaCB_ec_index(lua_State * L);
static int luaCB_ec_newindex(lua_State * L);
static int luaCB_ec_tostring(lua_State * L);

static int luaCB_fec_index(lua_State * L);
static int luaCB_fec_newindex(lua_State * L);
static int luaCB_fec_tostring(lua_State * L);

static int luaCB_gui_index(lua_State * L);
static int luaCB_gui_newindex(lua_State * L);

static const char * lua_camera_shutter_fields[] =
{
    "raw",
    "apex",
    "ms",
    "value",
    NULL
};

static const char * lua_camera_aperture_fields[] =
{
    "raw",
    "apex",
    "value",
    "min",
    "max",
    NULL
};

static const char * lua_camera_aperture_minmax_fields[] =
{
    "raw",
    "apex",
    "value",
    NULL
};

static const char * lua_camera_iso_fields[] =
{
    "raw",
    "apex",
    "value",
    NULL
};

static const char * lua_camera_ec_fields[] =
{
    "raw",
    "value",
    NULL
};

static const char * lua_camera_gui_fields[] =
{
    "menu",
    "play",
    "play_photo",
    "play_movie",
    "qr",
    "idle",
    NULL
};

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 4.5): Canon's own Tv / Av / Sv / compensation (Code768, the settings sheet of r6_script_expo) as ML raw
 * values (2.4); not readable: lens_info as upstream, one console line per script */
static int lua_r6_expo_raw(lua_State * L, int what)
{
    struct r6_expo e;
    if (r6_script_expo(&e))
    {
        if (what == 0) return r6_c768_to_raw(e.tv, 56);
        if (what == 1) return e.sv ? r6_c768_to_raw(e.sv - 3840, 72) : 0;
        return r6_c768_to_raw(e.comp, 0);
    }
    if (lua_r6_once(L, LUA_R6_ONCE_EXPO)) printf("[%s] camera: Canon's exposure values not readable, lens_info used\n", lua_get_script_filename(L));
    return what == 0 ? lens_info.raw_shutter : what == 1 ? lens_info.raw_iso : lens_info.ae;
}

/* 4.1 / 2.4: the Code768 of a shutter assignment (raw: 1/3 grid; apex / ms / value: the nearest third, no clamping); window
 * 1/8000 .. 30 s; the errors come from here (no job) */
static int lua_r6_tv_code(lua_State * L, const char * key, int * c)
{
    if (!strcmp(key, "raw"))
    {
        LUA_PARAM_INT(value, 3);
        int k = r6_raw_to_c768(value, 56, c);
        if (k == 1) return luaL_error(L, "set 'camera.shutter.raw' failed: 1/2 step");
        if (k) return luaL_error(L, "set 'camera.shutter.raw' failed: not on the 1/3 grid");
    }
    else
    {
        LUA_PARAM_NUMBER(value, 3);
        float apex = value;
        if (strcmp(key, "apex"))
        {
            float t = !strcmp(key, "ms") ? value / 1000.0f : value;
            if (!(t > 0)) return luaL_error(L, "set 'camera.shutter.%s' failed: outside 1/8000 .. 30 s", key);
            apex = -log2f(t);
        }
        /* review R1-5: lua.mo is built with -ffast-math (isnan / isfinite fold away, !(t > 0) lets NaN through): NaN and inf by their
         * bits, then the range as a float before roundf (a huge value saturates or wraps into the window as an int) */
        uint32_t bits;
        memcpy(&bits, &apex, sizeof(bits));
        if ((bits & 0x7F800000u) == 0x7F800000u || !(apex > -5.2f && apex < 13.2f))
            return luaL_error(L, "set 'camera.shutter.%s' failed: outside 1/8000 .. 30 s", key);
        *c = (int) roundf(apex * 3.0f) * 256;
    }
    if (*c < -3840 || *c > 9984) return luaL_error(L, "set 'camera.shutter.%s' failed: outside 1/8000 .. 30 s", key);
    return 0;
}

/* 4.1: a Tv assignment through the facade (a job "Tv" of r6_iv_task, Tor 1 context "script"), with 2.7 */
static int lua_r6_tv_set(lua_State * L, const char * key)
{
    int c = 0;
    lua_r6_tv_code(L, key, &c);
    struct lua_r6_call k = { .op = LR6_TV, .a = c };
    int r = lua_r6_do(L, &k);
    if (r == R6API_OK) return 0;
    if (r == (R6API_TV_RES | R6API_TV_R_TOOK_OVER)) return luaL_error(L, "Tv changed by user (your value stays)");
    if (r == (R6API_TV_LOCK | R6API_TV_L_OFF)) return luaL_error(L, "Tv writes: off (menu.set(\"Interval\", \"Tv writes\", \"ON\"))");
    return luaL_error(L, "set 'camera.shutter.%s' failed: %s", key, k.why);
}

/* 7.3: setters without an analysed R6 way */
static int lua_r6_m162(lua_State * L, const char * name)
{
    return luaL_error(L, "%s: not analysed yet (M16.2)", name);
}
#endif

static int luaCB_camera_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Gets a @{shutter} object that represents the camera's current shutter speed.
    // @tfield shutter shutter
    if(!strcmp(key, "shutter"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_shutter_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaCB_shutter_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_shutter_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_shutter_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    /// Gets an @{aperture} object that represents the lens' aperture.
    // @tfield aperture aperture
    else if(!strcmp(key, "aperture"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_aperture_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaCB_aperture_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_aperture_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_aperture_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    /// Gets an @{iso} object that represents camera's ISO.
    // @tfield iso iso
    else if(!strcmp(key, "iso"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_iso_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaCB_iso_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_iso_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_iso_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    /// Gets an @{ec} object that represents exposure compensation.
    // @tfield ec ec
    else if(!strcmp(key, "ec"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_ec_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaCB_ec_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_ec_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_ec_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    /// Get/set whether the flash is enabled.
    ///
    /// Possible values: true, false, "auto", "unknown".
    ///
    /// Flash can only be changed from script in simple (non-automatic) modes.
    ///
    /// Not tested with external flashes; it may work or it may not.
    // @tfield ?bool|string flash
    else if(!strcmp(key, "flash"))
    {
        if (strobo_firing == 0) {
            lua_pushboolean(L, 1); /* reversed */
        } else if (strobo_firing == 1) {
            lua_pushboolean(L, 0);
        } else if (strobo_firing == 2) {
            lua_pushstring(L, "auto");
        } else {
            lua_pushstring(L, "unknown");
        }
    }
    /// Gets an @{ec} object that represents flash exposure compensation.
    // @tfield ec flash_ec
    else if(!strcmp(key, "flash_ec"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_fec_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaCB_fec_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_fec_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_ec_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    /// Get/Set kelvin white balance.
    // @tfield int kelvin
    else if(!strcmp(key, "kelvin")) lua_pushinteger(L, lens_info.kelvin);
    /// Get the current camera mode, possible values defined in @{constants.MODE}.
    ///
    /// Note: for cameras without a dedicated video mode, it will return MODE.MOVIE
    /// whenever your camera is configured to record videos (usually from Canon menu).
    // @tfield int mode
    else if(!strcmp(key, "mode"))
    {
        lua_pushinteger(L, is_movie_mode() ? SHOOTMODE_MOVIE : shooting_mode);
    }
    /// Get the current metering mode (see PROP\_METERING\_MODE in property.h).
    ///
    /// TODO: add constants and setter.
    // @tfield int metering_mode readonly
    else if(!strcmp(key, "metering_mode")) lua_pushinteger(L, metering_mode);
    /// Get the current drive mode (see PROP\_DRIVE in property.h).
    ///
    /// TODO: add constants and setter.
    // @tfield int drive_mode readonly
    else if(!strcmp(key, "drive_mode")) lua_pushinteger(L, drive_mode);
    /// Get the model name of the camera (e.g.&nbsp;"Canon EOS 60D").
    // @tfield string model readonly
    else if(!strcmp(key, "model")) lua_pushstring(L, camera_model);
    /// Get the shortened model name of the camera (e.g.&nbsp;"5D3").
    // @tfield string model_short readonly
    else if(!strcmp(key, "model_short")) lua_pushstring(L, __camera_model_short);
    /// Get the Canon firmware version string (e.g.&nbsp;"1.2.3").
    // @tfield string firmware readonly
    else if(!strcmp(key, "firmware")) lua_pushstring(L, firmware_version);
    /// Get the temperature from the EFIC chip in raw units.
    ///
    /// TODO: Celsius.
    // @tfield int temperature readonly
#ifdef CONFIG_R6_LUA
    /* M16.1 (spec m16/m161 7.2): TempSvc sensor A in degC as Canon rounds it for EXIF; no valid value: nil */
    else if(!strcmp(key, "temperature"))
    {
        int dc = 0;
        if (r6_script_temp(&dc)) lua_pushnumber(L, dc / 10.0);
        else lua_pushnil(L);
    }
#else
    else if(!strcmp(key, "temperature")) lua_pushinteger(L, efic_temp);
#endif
    /// Gets a @{gui} object that controls Canon GUI state (PLAY, MENU, QR, various dialogs)
    // @tfield gui gui
    else if(!strcmp(key, "gui"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_gui_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_gui_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_gui_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_camera_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    int status = 1;
#ifdef CONFIG_R6_LUA
    /* M16.1 (spec m16/m161 4.1, 7.3): camera.shutter = seconds through the facade; the other setters are not analysed yet */
    if (!strcmp(key, "shutter")) return lua_r6_tv_set(L, "value");
    if (!strcmp(key, "aperture") || !strcmp(key, "iso") || !strcmp(key, "ec") || !strcmp(key, "flash") || !strcmp(key, "flash_ec")
        || !strcmp(key, "kelvin"))
    {
        char n[24];
        snprintf(n, sizeof(n), "camera.%s", key);
        return lua_r6_m162(L, n);
    }
#endif
    
    if(!strcmp(key, "shutter"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = hdr_set_rawshutter(shutterf_to_raw(value));
    }
    else if(!strcmp(key, "aperture"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = hdr_set_rawaperture((int)roundf((log2f(value) * 16) + 8));
    }
    else if(!strcmp(key, "iso"))
    {
        LUA_PARAM_INT(value, 3);
        int raw = value ? (int)roundf(log2f(value/3.125) * 8) + 32 : 0;
        status = hdr_set_rawiso(raw);
    }
    else if(!strcmp(key, "ec"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = lens_set_ae(APEX1000_EC2RAW((int)roundf(value * 1000)));
    }
    else if(!strcmp(key, "flash"))
    {
        if (strobo_firing == 0 || strobo_firing == 1)
        {
            LUA_PARAM_BOOL(value, 3);
            set_flash_firing(value ? 0 : 1);
        }
        else
        {
            return luaL_error(L, "set 'camera.flash' failed");
        }
    }
    else if(!strcmp(key, "flash_ec"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = lens_set_flash_ae(APEX1000_EC2RAW((int)roundf(value * 1000)));
    }
    else if(!strcmp(key, "kelvin"))
    {
        LUA_PARAM_INT(value, 3);
        lens_set_kelvin(value);
    }
    else if(!strcmp(key, "model") || !strcmp(key, "firmware") || !strcmp(key, "mode") || !strcmp(key, "metering_mode") || !strcmp(key, "drive_mode") || !strcmp(key, "temperature") || !strcmp(key, "state"))
    {
        return luaL_error(L, "'%s' is readonly!", key);
    }
    else
    {
        lua_rawset(L, 1);
    }
    if(!status) return luaL_error(L, "set '%s' failed", key);
    return 0;
}

/***
 Take a picture.
 @tparam[opt=false] bool should_af whether or not to use autofocus.
 @function shoot
 */
#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 3.4): a job "picture" of r6_iv_task (the Interval way, its checks); AF true is refused (K5); from a GUI
 * task queued (2000, r6.last_shot). Returns code, text, seq (SL-13) */
static int luaCB_camera_shoot(lua_State * L)
{
    LUA_PARAM_BOOL_OPTIONAL(should_af, 1, 0);
    struct lua_r6_call k = { .op = LR6_SHOOT, .a = should_af ? AF_ENABLE : AF_DISABLE };
    int r = lua_r6_do(L, &k);
    lua_pushinteger(L, r);
    lua_pushstring(L, k.why);
    lua_pushinteger(L, k.seq);
    return 3;
}
#else
static int luaCB_camera_shoot(lua_State * L)
{
    LUA_PARAM_BOOL_OPTIONAL(should_af, 1, 0);
    int result = take_a_pic(should_af);
    lua_pushinteger(L, result);
    return 1;
}
#endif

/***
 Wait until the camera is idle (not processing pictures).
 
 Normally, camera.shoot() returns as soon as the next picture can be taken,
 while image compression and saving happen in background.
 @function wait
 */
#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 3.4): until no script job, no ML picture and Canon's job counter 0, at most 60 s (2.7); a GUI task: error */
static int luaCB_camera_wait(lua_State * L)
{
    struct lua_r6_call k = { .op = LR6_WAITJOBS, .a = 60000 };
    int r = lua_r6_do(L, &k);
    if (r == R6API_TIMEOUT) return luaL_error(L, "camera.wait: timeout (60 s)");
    if (r) return luaL_error(L, "camera.wait: not from a GUI task (use task.create)");
    return 0;
}
#else
static int luaCB_camera_wait(lua_State * L)
{
    while (lens_info.job_state) {
        msleep(20);
    }
    return 0;
}
#endif

/***
 Take N pictures in burst mode.
 
 Note: your camera must be already in some continuous drive mode,
 otherwise the speed will be slow.
 
 @tparam int num_pictures how many pictures to take.
 @function burst
 */
#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 3.4): n script pictures, about 1 per second, stop at the first code != 0; returns made, code, text */
static int luaCB_camera_burst(lua_State * L)
{
    LUA_PARAM_INT(num_pictures, 1);
    struct lua_r6_call k = { .op = LR6_BURST, .a = num_pictures };
    int r = lua_r6_do(L, &k);
    lua_pushinteger(L, k.done);
    lua_pushinteger(L, r);
    lua_pushstring(L, k.why);
    return 3;
}
#else
static int luaCB_camera_burst(lua_State * L)
{
    LUA_PARAM_INT(num_pictures, 1);
    take_fast_pictures(num_pictures);
    return 0;
}
#endif

/***
 Take a picture in bulb mode.
 @tparam float duration bulb duration in seconds.
 @function bulb
 */
#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 3.4): only in B with Canon's bulb timer = duration (+- 0.5 s, 31 s .. 30 min), the D1 way of M9.2;
 * returns code, text */
static int luaCB_camera_bulb(lua_State * L)
{
    LUA_PARAM_NUMBER(duration, 1);
    struct lua_r6_call k = { .op = LR6_BULB, .a = (int) roundf(duration) };
    int r = lua_r6_do(L, &k);
    if (r == R6API_ARG) return luaL_error(L, "camera.bulb: %s", k.why);
    lua_pushinteger(L, r);
    lua_pushstring(L, k.why);
    return 2;
}
#else
static int luaCB_camera_bulb(lua_State * L)
{
    LUA_PARAM_NUMBER(duration, 1);
    bulb_take_pic(duration * 1000);
    return 0;
}
#endif

/***
 Restart the camera.
 @function reboot
 */
#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 7.3, K6): not analysed yet */
static int luaCB_camera_reboot(lua_State * L)
{
    return lua_r6_m162(L, "camera.reboot");
}
#else
static int luaCB_camera_reboot(lua_State * L)
{
    int reboot = 0;
    prop_request_change(PROP_REBOOT, &reboot, 4);
    return 0;

    /* shutdown is probably done in a similar way, but I had no success with nearby properties */
}
#endif

/// Represents the camera's shutter speed setting
//@type shutter

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 4.5): Canon's Tv (Code768) as the ML raw value */
static int luaCB_shutter_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (strcmp(key, "raw") && strcmp(key, "apex") && strcmp(key, "ms") && strcmp(key, "value")) { lua_rawget(L, 1); return 1; }
    int raw = lua_r6_expo_raw(L, 0);
    if(!strcmp(key,"raw")) lua_pushinteger(L, raw);
    else if(!strcmp(key,"apex")) lua_pushnumber(L, (APEX1000_RAW2TV(raw)/1000.0));
    else if(!strcmp(key,"ms")) lua_pushinteger(L, raw2shutter_ms(raw));
    else lua_pushnumber(L, raw2shutterf(raw));
    return 1;
}
#else
static int luaCB_shutter_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Get/Set shutter speed in Canon raw units.
    // @tfield int raw
    if(!strcmp(key,"raw")) lua_pushinteger(L, lens_info.raw_shutter);
    /// Get/Set shutter speed in APEX units (floating point).
    // @tfield number apex
    else if(!strcmp(key,"apex")) lua_pushnumber(L, (APEX1000_RAW2TV(lens_info.raw_shutter)/1000.0));
    /// Get/Set shutter speed in milliseconds.
    // @tfield int ms
    else if(!strcmp(key,"ms")) lua_pushinteger(L, raw2shutter_ms(lens_info.raw_shutter));
    /// Get/Set shutter speed in seconds (floating point).
    // @tfield number value
    else if(!strcmp(key,"value")) lua_pushnumber(L, raw2shutterf(lens_info.raw_shutter));
    else lua_rawget(L, 1);
    return 1;
}
#endif

/// Pretty prints the current shutter speed as a string.
//@treturn string
//@function __tostring
#ifdef CONFIG_R6_LUA
static int luaCB_shutter_tostring(lua_State * L)
{
    int raw = lua_r6_expo_raw(L, 0);
    char s[16];
    int old = cli();
    snprintf(s, sizeof(s), "%s", lens_format_shutter(raw));
    sei(old);
    lua_pushstring(L, s);
    return 1;
}
#else
static int luaCB_shutter_tostring(lua_State * L)
{
    int old = cli();
    lua_pushstring(L, lens_format_shutter(lens_info.raw_shutter));
    sei(old);
    return 1;
}
#endif

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 4.1): raw / apex / ms / value through the facade (Tor 1 context "script") */
static int luaCB_shutter_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (!strcmp(key, "raw") || !strcmp(key, "apex") || !strcmp(key, "ms") || !strcmp(key, "value")) return lua_r6_tv_set(L, key);
    lua_rawset(L, 1);
    return 0;
}
#else
static int luaCB_shutter_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    
    /* this breaks copy2m */
    //~ if(!lens_info.raw_shutter) return luaL_error(L, "Shutter speed is automatic - cannot adjust manually.");
    
    int status = 1;
    if(!strcmp(key, "raw"))
    {
        LUA_PARAM_INT(value, 3);
        status = hdr_set_rawshutter(value);
    }
    else if(!strcmp(key, "apex"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = hdr_set_rawshutter(APEX1000_TV2RAW((int)roundf(value * 1000)));
    }
    else if(!strcmp(key, "ms"))
    {
        LUA_PARAM_INT(value, 3);
        status = hdr_set_rawshutter(shutter_ms_to_raw(value));
    }
    else if(!strcmp(key, "value"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = hdr_set_rawshutter(shutterf_to_raw(value));
    }
    else
    {
        lua_rawset(L, 1);
    }
    if(!status) return luaL_error(L, "set 'camera.shutter.%s' failed", key);
    return 0;
}
#endif

/// Represents the lens's aperture.
//@type aperture

static int luaCB_aperture_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Get/Set aperture in Canon raw units.
    // @tfield int raw
    if(!strcmp(key,"raw")) lua_pushinteger(L, lens_info.raw_aperture);
    /// Get/Set aperture in APEX units (floating point).
    // @tfield number apex
    else if(!strcmp(key,"apex")) lua_pushnumber(L, (APEX1000_RAW2AV(lens_info.raw_aperture) / 1000.0));
    /// Get/Set aperture as f-number (floating point).
    // @tfield number value
    else if(!strcmp(key,"value")) lua_pushnumber(L, lens_info.aperture / 10.0);
    /// Get minimum (wide open) aperture value (aperture object).
    // @tfield aperture min readonly
    else if(!strcmp(key,"min"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_min_aperture_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaCB_min_aperture_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_aperture_minmax_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    /// Get maximum (closed) aperture value (aperture object).
    // @tfield aperture max readonly
    else if(!strcmp(key,"max"))
    {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, luaCB_max_aperture_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, luaCB_max_aperture_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, luaCB_pairs);
        lua_setfield(L, -2, "__pairs");
        lua_pushlightuserdata(L, lua_camera_aperture_minmax_fields);
        lua_setfield(L, -2, "fields");
        lua_setmetatable(L, -2);
    }
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_min_aperture_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    // Get minimum (wide open) aperture in Canon raw units
    // @tfield int raw
    if(!strcmp(key,"raw")) lua_pushinteger(L, lens_info.raw_aperture_min);
    // Get minimum (wide open) aperture in APEX units (floating point)
    // @tfield number apex
    else if(!strcmp(key,"apex")) lua_pushnumber(L, (APEX1000_RAW2AV(lens_info.raw_aperture_min) / 1000.0));
    // Get minimum (wide open) aperture as f-number (floating point)
    // @tfield number value
    else if(!strcmp(key,"value")) lua_pushnumber(L, RAW2VALUE(aperture, lens_info.raw_aperture_min) / 10.0);
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_max_aperture_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    // Get maximum (closed) aperture in Canon raw units
    // @tfield int raw
    if(!strcmp(key,"raw")) lua_pushinteger(L, lens_info.raw_aperture_max);
    // Get maximum (closed) aperture in APEX units (floating point)
    // @tfield number apex
    else if(!strcmp(key,"apex")) lua_pushnumber(L, (APEX1000_RAW2AV(lens_info.raw_aperture_max) / 1000.0));
    // Get maximum (closed) aperture as f-number (floating point)
    // @tfield number value
    else if(!strcmp(key,"value")) lua_pushnumber(L, RAW2VALUE(aperture, lens_info.raw_aperture_max) / 10.0);
    else lua_rawget(L, 1);
    return 1;
}

/// Pretty prints the current aperture as a string.
//@treturn string
//@function __tostring
static int luaCB_aperture_tostring(lua_State * L)
{
    int old = cli();
    lua_pushstring(L, lens_format_aperture(lens_info.raw_aperture));
    sei(old);
    return 1;
}

static int luaCB_min_aperture_tostring(lua_State * L)
{
    int old = cli();
    lua_pushstring(L, lens_format_aperture(lens_info.raw_aperture_min));
    sei(old);
    return 1;
}

static int luaCB_max_aperture_tostring(lua_State * L)
{
    int old = cli();
    lua_pushstring(L, lens_format_aperture(lens_info.raw_aperture_max));
    sei(old);
    return 1;
}

#ifdef CONFIG_R6_LUA
static int luaCB_aperture_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (!strcmp(key, "raw") || !strcmp(key, "apex") || !strcmp(key, "value")) return lua_r6_m162(L, "camera.aperture");
    lua_rawset(L, 1);
    return 0;
}
#else
static int luaCB_aperture_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    
    if (!lens_info.aperture)
    {
        /* is it needed? */
        //~ return luaL_error(L, lens_info.name[0] ? "Aperture is automatic - cannot adjust manually." : "Manual lens - cannot adjust aperture.");
    }
    int status = 1;
    
    if(!strcmp(key, "raw"))
    {
        LUA_PARAM_INT(value, 3);
        status = hdr_set_rawaperture(value);
    }
    else if(!strcmp(key, "apex"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = hdr_set_rawaperture(APEX1000_AV2RAW((int)roundf(value * 1000)));
    }
    else if(!strcmp(key, "value"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = hdr_set_rawaperture((int)roundf((log2f(value) * 16) + 8));
    }
    else
    {
        lua_rawset(L, 1);
    }
    if(!status) return luaL_error(L, "set 'camera.aperture.%s' failed", key);
    return 0;
}
#endif

/// Represents the camera ISO.
//@type iso

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 4.5): Canon's Sv (Code768) as the ML raw value */
static int luaCB_iso_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (strcmp(key, "raw") && strcmp(key, "apex") && strcmp(key, "value")) { lua_rawget(L, 1); return 1; }
    int raw = lua_r6_expo_raw(L, 1);
    if(!strcmp(key,"raw")) lua_pushinteger(L, raw);
    else if(!strcmp(key,"apex")) lua_pushnumber(L, (APEX1000_RAW2SV(raw) / 1000.0));
    else lua_pushinteger(L, raw2iso(raw));
    return 1;
}
#else
static int luaCB_iso_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Get/Set ISO in Canon raw units.
    // @tfield int raw
    if(!strcmp(key,"raw")) lua_pushinteger(L, lens_info.raw_iso);
    /// Get/Set ISO in APEX units (floating point).
    // @tfield number apex
    else if(!strcmp(key,"apex")) lua_pushnumber(L, (APEX1000_RAW2SV(lens_info.raw_iso) / 1000.0));
    /// Get/Set ISO (e.g.&nbsp;100, 200).
    // @tfield int value
    else if(!strcmp(key,"value")) lua_pushinteger(L, raw2iso(lens_info.raw_iso));
    else lua_rawget(L, 1);
    return 1;
}
#endif

/// Pretty prints the current ISO as a string.
//@treturn string
//@function __tostring
#ifdef CONFIG_R6_LUA
static int luaCB_iso_tostring(lua_State * L)
{
    int raw = lua_r6_expo_raw(L, 1);
    char s[16];
    int old = cli();
    snprintf(s, sizeof(s), "%s", lens_format_iso(raw));
    sei(old);
    lua_pushstring(L, s);
    return 1;
}
#else
static int luaCB_iso_tostring(lua_State * L)
{
    int old = cli();
    lua_pushstring(L, lens_format_iso(lens_info.raw_iso));
    sei(old);
    return 1;
}
#endif

#ifdef CONFIG_R6_LUA
static int luaCB_iso_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (!strcmp(key, "raw") || !strcmp(key, "apex") || !strcmp(key, "value")) return lua_r6_m162(L, "camera.iso");
    lua_rawset(L, 1);
    return 0;
}
#else
static int luaCB_iso_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    int status = 1;
    if(!strcmp(key, "raw"))
    {
        LUA_PARAM_INT(value, 3);
        status = hdr_set_rawiso(value);
    }
    else if(!strcmp(key, "apex"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = hdr_set_rawiso(APEX1000_SV2RAW((int)roundf(value * 1000)));
    }
    else if(!strcmp(key, "value"))
    {
        LUA_PARAM_INT(value, 3);
        int raw = value ? (int)roundf(log2f(value/3.125) * 8) + 32 : 0;
        status = hdr_set_rawiso(raw);
    }
    else
    {
        lua_rawset(L, 1);
    }
    if(!status) return luaL_error(L, "set 'camera.iso.%s' failed", key);
    return 0;
}
#endif

/// Represents the exposure compensation.
//@type ec

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 4.5): Canon's compensation (Code768) as the ML raw value */
static int luaCB_ec_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (strcmp(key, "raw") && strcmp(key, "value")) { lua_rawget(L, 1); return 1; }
    int raw = lua_r6_expo_raw(L, 2);
    if(!strcmp(key,"raw")) lua_pushinteger(L, raw);
    else lua_pushnumber(L, APEX1000_RAW2EC(raw) / 1000.0);
    return 1;
}
#else
static int luaCB_ec_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Get/Set exposure compensation in Canon raw units.
    // @tfield int raw
    if(!strcmp(key,"raw")) lua_pushinteger(L, lens_info.ae);
    /// Get/Set exposure compensation in EV or APEX (both are identical here).
    // @tfield number value
    else if(!strcmp(key,"value")) lua_pushnumber(L, APEX1000_RAW2EC(lens_info.ae) / 1000.0);
    else lua_rawget(L, 1);
    return 1;
    
}
#endif

/// Pretty prints the exposure compensation as a string.
//@treturn string
//@function __tostring
#ifdef CONFIG_R6_LUA
static int luaCB_ec_tostring(lua_State * L)
{
    int ec = APEX10_RAW2EC(lua_r6_expo_raw(L, 2));
#else
static int luaCB_ec_tostring(lua_State * L)
{
    int ec = APEX10_RAW2EC(lens_info.ae);
#endif

    lua_pushfstring(L, "%s%d.%d", FMT_FIXEDPOINT1(ec));
    return 1;
}

#ifdef CONFIG_R6_LUA
static int luaCB_ec_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (!strcmp(key, "raw") || !strcmp(key, "value")) return lua_r6_m162(L, "camera.ec");
    lua_rawset(L, 1);
    return 0;
}
#else
static int luaCB_ec_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    int status = 1;
    if(!strcmp(key, "raw"))
    {
        LUA_PARAM_INT(value, 3);
        status = lens_set_ae(value);
    }
    else if(!strcmp(key, "value"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = lens_set_ae(APEX1000_EC2RAW((int)roundf(value * 1000)));
    }
    else
    {
        lua_rawset(L, 1);
    }
    if(!status) return luaL_error(L, "set 'camera.ec.%s' failed", key);
    return 0;
}
#endif

static int luaCB_fec_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if(!strcmp(key,"raw")) lua_pushinteger(L, lens_info.flash_ae);
    else if(!strcmp(key,"value")) lua_pushnumber(L, APEX1000_RAW2EC(lens_info.flash_ae) / 1000.0);
    else lua_rawget(L, 1);
    return 1;
    
}

static int luaCB_fec_tostring(lua_State * L)
{
    int fec = APEX10_RAW2EC(lens_info.flash_ae);
    lua_pushfstring(L, "%s%d.%d", FMT_FIXEDPOINT1(fec));
    return 1;
}

#ifdef CONFIG_R6_LUA
static int luaCB_fec_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if (!strcmp(key, "raw") || !strcmp(key, "value")) return lua_r6_m162(L, "camera.flash_ec");
    lua_rawset(L, 1);
    return 0;
}
#else
static int luaCB_fec_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    int status = 1;
    if(!strcmp(key, "raw"))
    {
        LUA_PARAM_INT(value, 3);
        status = lens_set_flash_ae(value);
    }
    else if(!strcmp(key, "value"))
    {
        LUA_PARAM_NUMBER(value, 3);
        status = lens_set_flash_ae(APEX1000_EC2RAW((int)roundf(value * 1000)));
    }
    else
    {
        lua_rawset(L, 1);
    }
    if(!status) return luaL_error(L, "set 'camera.fec.%s' failed", key);
    return 0;
}
#endif

/// Canon GUI controls (PLAY, MENU, QR and so on) 
//@type gui

static int luaCB_gui_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    /// Get/Set whether Canon menu is active or not.
    ///
    /// Can also be used to enter Canon menu: camera.gui.menu = true;
    // @tfield bool menu
    if(!strcmp(key,"menu")) lua_pushboolean(L, is_menu_mode());
    /// Get/Set whether the camera is in PLAY mode (image or video review).
    ///
    /// Can also be used to enter PLAY mode: camera.gui.menu = true;
    // @tfield bool play
    else if(!strcmp(key,"play")) lua_pushboolean(L, is_play_mode());
    /// Get whether the camera is in PLAY mode with a still photo selected.
    // @tfield bool play_photo
    else if(!strcmp(key,"play_photo")) lua_pushboolean(L, is_pure_play_photo_mode());
    /// Get whether the camera is in PLAY mode with a H.264 movie selected.
    // @tfield bool play_movie
    else if(!strcmp(key,"play_movie")) lua_pushboolean(L, is_pure_play_movie_mode());
    /// Get whether the camera is in QR (QuickReview) mode.
    ///
    /// QuickReview is the image review mode used right after taking a picture.
    ///
    /// It is not the same as the regular PLAY mode, even though they are similar at first sight (both are used for reviewing an image).
    /// Important difference: in QR mode, ML has access to raw image data from the last captured picture; this data is not available in PLAY mode.
    ///
    // @tfield bool qr
    else if(!strcmp(key,"qr")) lua_pushboolean(L, !is_play_mode() && is_play_or_qr_mode());
    /// Get whether the camera is "idle" (in standby, with no other dialogs active)
    // @tfield bool idle
    else if(!strcmp(key,"idle")) lua_pushboolean(L, display_idle());
    /// Get/Set current GUI mode from Canon (model-dependent).
    /// 
    /// On most models, 0 is "idle" (not exactly identical to what ML considers idle), 1 is PLAY, 2 is MENU.
    // @tfield int mode
    else if(!strcmp(key,"mode")) lua_pushinteger(L, get_gui_mode());
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_gui_tostring(lua_State * L)
{
    char status[50] = "";
    if (is_menu_mode())                 { STR_APPEND(status, "MENU"); }
    else if (is_play_mode())            { STR_APPEND(status, "PLAY"); }
    else if (is_pure_play_photo_mode()) { STR_APPEND(status, "PLAY,PH"); }
    else if (is_pure_play_movie_mode()) { STR_APPEND(status, "PLAY,MV"); }
    else if (is_play_mode())            { STR_APPEND(status, "PLAY"); }
    else
    {
        if (display_idle())             { STR_APPEND(status, "IDLE,"); }
        else                            { STR_APPEND(status, "BUSY,"); }

        if (is_movie_mode())            { STR_APPEND(status, "MV"); }
        else if (lv)                    { STR_APPEND(status, "LV"); }
        else                            { STR_APPEND(status, "PH"); }
    }
    lua_pushfstring(L, status);
    return 0;
}

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 6.1): SetGUIRequestMode through the facade (the intent mark, the settle time, 2.7; GUI tasks: error) */
static int luaCB_gui_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    int op = -1, v = 0;
    if (!strcmp(key, "mode")) { LUA_PARAM_INT(value, 3); op = R6SG_MODE; v = value; }
    else if (!strcmp(key, "play")) { LUA_PARAM_BOOL(value, 3); op = R6SG_PLAY; v = value; }
    else if (!strcmp(key, "menu")) { LUA_PARAM_BOOL(value, 3); op = R6SG_MENU; v = value; }
    if (op < 0) return 0;
    struct lua_r6_call k = { .op = LR6_GUI, .a = op, .b = v };
    if (lua_r6_do(L, &k)) return luaL_error(L, "set 'camera.gui.%s' failed: %s", key, k.why);
    return 0;
}
#else
static int luaCB_gui_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    int status = 1;
    if(!strcmp(key, "mode"))
    {
        LUA_PARAM_INT(value, 3);
        SetGUIRequestMode(value);
        msleep(500);
        status = (get_gui_mode() == value);
    }
    else if(!strcmp(key, "play"))
    {
        LUA_PARAM_BOOL(value, 3);
        if (value) enter_play_mode();
        else exit_play_qr_mode();
        status = (is_play_mode() == value);
    }
    else if(!strcmp(key, "menu"))
    {
        LUA_PARAM_BOOL(value, 3);
        if (value) enter_menu_mode();
        else exit_menu_mode();
        status = (is_menu_mode() == value);
    }
    if(!status) return luaL_error(L, "set 'camera.gui.%s' failed", key);
    return 0;
}
#endif

static const char * lua_camera_fields[] =
{
    "shutter",
    "aperture",
    "iso",
    "ec",
    "flash",
    "flash_ec",
    "kelvin",
    "mode",
    "metering_mode",
    "drive_mode",
    "model",
    "model_short",
    "firmware",
    "temperature",
    "gui",
    NULL
};

static const luaL_Reg cameralib[] =
{
    { "shoot", luaCB_camera_shoot },
    { "burst", luaCB_camera_burst },
    { "bulb", luaCB_camera_bulb },
    { "wait", luaCB_camera_wait },
    { "reboot", luaCB_camera_reboot },
    { NULL, NULL }
};

LUA_LIB(camera)
