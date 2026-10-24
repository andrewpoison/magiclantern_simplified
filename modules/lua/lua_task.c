/***
 Task functions
 
 @author Magic Lantern Team
 @copyright 2014
 @license GPL
 @module task
 */

#include <dryos.h>
#include <fileprefix.h>
#include <string.h>
#include <powersave.h>

#include "lua_common.h"
#ifdef CONFIG_R6_LUA
#include "lua_r6.h"             /* M16.0 (spec m16/m160 3.5, 4.3): task site, E11 limits */
#endif

struct lua_task_func
{
    lua_State * L;
    int function_ref;
    int disable_psave;
};

static int luaCB_card_index(lua_State * L);
static int luaCB_card_newindex(lua_State * L);

#ifdef CONFIG_R6_LUA
/* M16.1 (spec m16/m161 8.2 Ue3): an anchor per Lua task (16 slots by task id, lua_r6.c): lua_r6_park (a dead or closed script at a
 * take) and lua_r6_abort (an unprotected error in this task: the script is dead) jump back here; the task then permits its
 * powersave_prohibit, cancels its open jobs (r6_script_task_end, SL-2), frees its record and ends. Without a free slot as M16.0.
 * M16.0 (spec m16/m160 3.5): the task was counted by luaCB_task_create (lua_set_cant_unload) before it started. */
static void lua_run_task(struct lua_task_func * lua_task_func)
{
    if(lua_task_func)
    {
        lua_State * L = lua_task_func->L;
        const uint32_t gen = lua_r6_gen(L);
        struct lua_r6_tanchor a;
        volatile int psave = 0;
        int anchored = lua_r6_tanchor_set(&a, L);
        if (anchored)
        {
            if (setjmp(a.jb))                   /* review R2-12: setjmp as the whole controlling expression (C11 7.13.1.1) */
            {
                if (psave && lua_r6_psave(L, -1)) powersave_permit();   /* review R2-10: not again after R-4's permits */
                lua_r6_tanchor_clear(&a);
                r6_script_task_end(gen);
                printf("[Lua] task of a dead script ended.\n");
                free(lua_task_func);
                return;
            }
        }
        struct semaphore * sem = NULL;
        if (lua_take_semaphore(L, 0, &sem) == 0)
        {
            ASSERT(sem);
            if (lua_get_cant_yield(L) == -1)
            {
                /* main task was unloaded? continuing would be use after free */
                fprintf(stderr, "[%s] will not start new tasks.\n", lua_get_script_filename(L));
                goto skip;
            }

            if(lua_rawgeti(L, LUA_REGISTRYINDEX, lua_task_func->function_ref) == LUA_TFUNCTION)
            {
                if (lua_task_func->disable_psave)
                {
                    powersave_prohibit();
                    psave = 1;
                    lua_r6_psave(L, 1);
                }

                printf("[%s] task starting.\n", lua_get_script_filename(L));

                if(docall(L, 0, 0))
                {
                    fprintf(stderr, "[%s] task error:\n%s\n", lua_get_script_filename(L), lua_tostring(L, -1));
                    lua_save_last_error(L);
#ifdef CONFIG_R6_LUA
                    lua_pop(L, 1);          /* M16.1: the message off the shared stack (a waiting task of this script checks its top) */
#endif
                }
                luaL_unref(L, LUA_REGISTRYINDEX, lua_task_func->function_ref);

                printf("[%s] task exiting.\n", lua_get_script_filename(L));

                if (lua_task_func->disable_psave)
                {
                    powersave_permit();
                    psave = 0;
                    lua_r6_psave(L, -1);
                }

                r6_script_task_end(gen);       /* SL-2: this task's own open jobs (a picture it gave up at a backstop) */

                /* If all tasks started by the script are finished
                 * _before_ the main task ends, the script can be unloaded.
                 * Note: lua_set_cant_unload keeps a counter of tasks
                 * (number of calls LUA_TASK_UNLOAD_MASK)
                 */
                lua_set_cant_unload(L, 0, LUA_TASK_UNLOAD_MASK);
            }
            else
            {
                /* should be covered in luaCB_task_create */
                ASSERT(0);
            }

        skip:
            give_semaphore(sem);
        }
        else
        {
            printf("[Lua] semaphore error: run task\n");
        }
        if (anchored) lua_r6_tanchor_clear(&a);
        free(lua_task_func);
    }
}
#else
static void lua_run_task(struct lua_task_func * lua_task_func)
{
    if(lua_task_func)
    {
        lua_State * L = lua_task_func->L;
        struct semaphore * sem = NULL;
        if (lua_take_semaphore(L, 0, &sem) == 0)
        {
            ASSERT(sem);
            if (lua_get_cant_yield(L) == -1)
            {
                /* main task was unloaded? continuing would be use after free */
                fprintf(stderr, "[%s] will not start new tasks.\n", lua_get_script_filename(L));
                goto skip;
            }
            
            if(lua_rawgeti(L, LUA_REGISTRYINDEX, lua_task_func->function_ref) == LUA_TFUNCTION)
            {
                /* script created a task;
                 * it can't be unloaded while this task is running */
                lua_set_cant_unload(L, 1, LUA_TASK_UNLOAD_MASK);

                if (lua_task_func->disable_psave)
                {
                    powersave_prohibit();
                }

                printf("[%s] task starting.\n", lua_get_script_filename(L));

                if(docall(L, 0, 0))
                {
                    fprintf(stderr, "[%s] task error:\n%s\n", lua_get_script_filename(L), lua_tostring(L, -1));
                    lua_save_last_error(L);
                }
                luaL_unref(L, LUA_REGISTRYINDEX, lua_task_func->function_ref);

                printf("[%s] task exiting.\n", lua_get_script_filename(L));

                if (lua_task_func->disable_psave)
                {
                    powersave_permit();
                }

                /* If all tasks started by the script are finished
                 * _before_ the main task ends, the script can be unloaded.
                 * Note: lua_set_cant_unload keeps a counter of tasks
                 * (number of calls LUA_TASK_UNLOAD_MASK)
                 */
                lua_set_cant_unload(L, 0, LUA_TASK_UNLOAD_MASK);
            }
            else
            {
                /* should be covered in luaCB_task_create */
                ASSERT(0);
            }

        skip:
            give_semaphore(sem);
        }
        else
        {
            printf("[%s] semaphore error: run task\n", lua_get_script_filename(L));
        }
        free(lua_task_func);
    }
}

#endif

/***
 Creates a new task. It will begin executing when you return or call task.yield().
 
 Tasks spawned from Lua can optionally run with powersaving disabled
 (so the camera won't turn off while running them) if you enable `disable_powesave`
 (for example, `task.create(my_func, nil, nil, true)`).
 
 @tparam function f the function to run
 @tparam[opt] int priority DryOS task priority (0x1F = lowest priority; lower values = higher priority; default 0x1C)
 @tparam[opt] int stack_size Memory reserved for this task to be used as stack (default 0x10000)
 @tparam[opt=false] bool disable_powersave Set to `true` to prevent the camera from turning off while running this task.
 @function create
 */
#ifdef CONFIG_R6_LUA
/* M16.0 (spec m16/m160 3.5, 4.3; E11): priority in [R6L_PRIO_MIN, R6L_PRIO_MAX] (below 0x1C: raised; above Canon's maximum:
 * Canon's task_create would assert), each change once per script on the console; stack default R6L_STACK_TASK_DEF, else rounded
 * up to 4 KB and kept in [R6L_STACK_TASK_MIN, R6L_STACK_TASK_MAX]; the R6 task model (refused: an error, nothing leaks).
 * The function is referenced explicitly (upstream referenced the top of the stack, i.e. the last given argument). */
static int luaCB_task_create(lua_State * L)
{
    if(!lua_isfunction(L, 1)) return luaL_argerror(L, 1, "function expected");
    LUA_PARAM_INT_OPTIONAL(priority, 2, 0x1c);
    LUA_PARAM_INT_OPTIONAL(stack_size, 3, 0);
    LUA_PARAM_BOOL_OPTIONAL(disable_psave, 4, 0);

    struct r6_lua_stat * st = r6_lua_stat();
    int pmin = lua_r6_cfg(R6L_PRIO_MIN), pmax = lua_r6_cfg(R6L_PRIO_MAX);
    if (priority < pmin || priority > pmax)
    {
        int up = priority < pmin;
        if (lua_r6_once(L, up ? LUA_R6_ONCE_PRIO_UP : LUA_R6_ONCE_PRIO_DOWN))
        {
            printf("[%s] task.create: priority 0x%x %s to 0x%x\n", lua_get_script_filename(L), priority, up ? "raised" : "lowered",
                up ? pmin : pmax);
        }
        priority = up ? pmin : pmax;
        st->prio_clamped++;
    }
    int stack = lua_r6_cfg(R6L_STACK_TASK_DEF);
    if (stack_size > 0)
    {
        uint32_t s = ((uint32_t) stack_size + 0xFFF) & ~0xFFFu;
        uint32_t smin = lua_r6_cfg(R6L_STACK_TASK_MIN), smax = lua_r6_cfg(R6L_STACK_TASK_MAX);
        stack = s < smin ? (int) smin : s > smax ? (int) smax : (int) s;
        if (stack != stack_size) st->stack_clamped++;
    }

    /* M16.1 (spec m16/m161 7.3): the powersave lock is a no-op on the R6 (PROP_ICU_AUTO_POWEROFF is not written, M16.2) */
    if (disable_psave && lua_r6_once(L, LUA_R6_ONCE_PSAVE))
        printf("[%s] task.create: disable_powersave has no effect on R6 (M16.2)\n", lua_get_script_filename(L));

    struct lua_task_func * func = malloc(sizeof(struct lua_task_func));
    if(!func) return luaL_error(L, "malloc error\n");

    func->L = L;
    lua_pushvalue(L, 1);
    func->function_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    func->disable_psave = disable_psave;
    char why[48];
    /* counted before the task runs (the task only decrements at its end): a main chunk that returns before its task ran keeps
     * the script loaded (upstream counted in the task: load_script could lua_close the state first, the task then hung in
     * lua_script()'s ASSERT(0); while(1) at its priority) */
    lua_set_cant_unload(L, 1, LUA_TASK_UNLOAD_MASK);
    uint32_t ret = lua_r6_start(R6LK_TASK, "lua_script_task", priority, stack, lua_run_task, func, why, sizeof(why));
    if (ret == R6TK_REFUSED)
    {
        lua_set_cant_unload(L, 0, LUA_TASK_UNLOAD_MASK);
        luaL_unref(L, LUA_REGISTRYINDEX, func->function_ref);
        free(func);
        return luaL_error(L, "task not started (%s)", why);
    }
    return 0;
}
#else
static int luaCB_task_create(lua_State * L)
{
    if(!lua_isfunction(L, 1)) return luaL_argerror(L, 1, "function expected");
    LUA_PARAM_INT_OPTIONAL(priority, 2, 0x1c);
    LUA_PARAM_INT_OPTIONAL(stack_size, 3, 0x10000);
    LUA_PARAM_BOOL_OPTIONAL(disable_psave, 4, 0);

    struct lua_task_func * func = malloc(sizeof(struct lua_task_func));
    if(!func) return luaL_error(L, "malloc error\n");

    func->L = L;
    func->function_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    func->disable_psave = disable_psave;
    uint32_t ret = (uint32_t) task_create("lua_script_task", priority, stack_size, lua_run_task, func);
    if (ret & 1) return luaL_error(L, "task not started\n");
    return 0;
}
#endif

/***
 Yields execution of this script to other tasks and event handlers
 (also from the same script).
 
 FIXME: once a task executed yield(), other tasks or event handlers
 from the same script, that might interrupt the former, **must not** execute yield().
 Otherwise, an error will be thrown to prevent memory corruption, camera lockup or worse.

 The above limitation is a very dirty hack - a proper fix should be implemented
 by somebody familiar with multitasking in Lua.
 Help is more than welcome, as this topic is
 [not exactly our cup of tea](http://www.magiclantern.fm/forum/index.php?topic=14828.msg179227#msg179227).

 TODO: replace with msleep?
 @tparam int duration how long to sleep for in ms.
 @function yield
 */
static int luaCB_task_yield(lua_State * L)
{
    LUA_PARAM_INT(duration, 1);
#ifdef CONFIG_R6_LUA
    /* review R2-3: only where an anchor ends the wait (the main chunk, a task.create task); a callback (GuiMainTask, menu_task,
     * clock_task, lua_prop_task, shoot_task) would block its task and, if the script died meanwhile, park it for good */
    if (!lua_r6_may_wait(L)) return luaL_error(L, "task.yield: only in the main chunk or a task.create task");
#endif

    /* hack: figure out whether we might be interrupting
     * somebody else who called task.yield() */
    if (lua_get_cant_yield(L))
    {
        return luaL_error(L, "FIXME: cannot use task.yield() from two tasks");
    }

    /* let's hope the stack is not going to be modified by other tasks */
    int t1 = lua_gettop(L);         /* current stack top */
    int v1 = lua_tointeger(L, t1);  /* duration (integer) */

    lua_set_cant_yield(L, 1);
    lua_give_semaphore(L, NULL);
    msleep(duration);
    lua_take_semaphore(L, 0, NULL);
    lua_set_cant_yield(L, 0);

    /* check the stack */
    int t2 = lua_gettop(L);
    int v2 = lua_tointeger(L, t2);
    ASSERT(t1 == t2);
    ASSERT(v1 == v2);
    return 0;
}

static int luaCB_task_index(lua_State * L)
{
    lua_rawget(L, 1);
    return 1;
}

static int luaCB_task_newindex(lua_State * L)
{
    lua_rawset(L, 1);
    return 0;
}

static const char * lua_task_fields[] =
{
    NULL
};

const luaL_Reg tasklib[] =
{
    {"create", luaCB_task_create},
    {"yield", luaCB_task_yield},
    {NULL, NULL}
};

LUA_LIB(task)
