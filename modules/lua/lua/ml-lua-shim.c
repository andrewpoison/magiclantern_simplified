#include "ml-lua-shim.h"

/* undefine realloc, because we'll call the core function here */
#undef realloc
void *realloc(void *ptr, size_t size);

#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fio-ml.h>
#include <errno.h>
#include "umm_malloc/umm_malloc.h"
#ifdef CONFIG_R6_LUA
#include "../lua_r6.h"          /* M16.0 (spec m16/m160 5.6 F1, 5.5, 4.6, 4.8): fd table, allocations, abort, fault hook */
#endif

#undef DEBUG

#ifdef DEBUG
#define dbg_printf(fmt,...) { char msg[256]; snprintf(msg, sizeof(msg), fmt, ## __VA_ARGS__); console_puts(msg); }
#else
#define dbg_printf(fmt,...) {}
#endif

/* note: this does not add newline */
void console_puts(const char* str);

extern const char * format_memory_size( uint64_t size); /* e.g. 2.0GB, 32MB, 2.4kB... */

#ifndef CONFIG_R6_LUA
static uint64_t filesizes[16] = {0};
#endif

/* fixme: FIO functions actually return int, not FILE* */

#ifdef CONFIG_R6_LUA
/* M16.0 (spec m16/m160 5.6 F1): file descriptors 3..18 index a table of FIO handles (lua_r6.c, slots taken with r6_cas32);
 * upstream used the FIO handle itself and dropped every handle outside 3..16 */
int __libc_open(const char * fn, int flags, ...)
{
    dbg_printf("__libc_open(%s, %x) => ", fn, flags);

    uint32_t filesize = 0;
    if (FIO_GetFileSize(fn, &filesize) != 0)
    {
        if (!(flags & O_CREAT))
        {
            dbg_printf("ERR_SIZE\n");
            errno = ENOENT;
            return -1;
        }
    }

    FILE * h =
        (flags & O_CREAT) && (flags & O_APPEND) ?
            FIO_CreateFileOrAppend(fn) :
        (flags & O_CREAT) ?
            FIO_CreateFile(fn) :
            FIO_OpenFile(fn, flags);

    if (!h)
    {
        dbg_printf("ERR_OPEN\n");
        errno = ENOENT;
        return -1;
    }

    int fd = lua_r6_fd_add((uint32_t) h, filesize);
    if (fd < 0)
    {
        fprintf(stderr, "[Lua] too many open files\n");
        FIO_CloseFile(h);
        errno = EMFILE;
        return -1;
    }

    dbg_printf("%d\n", fd);
    return fd;
}

int __libc_close(int fd)
{
    dbg_printf("__libc_close(%d)\n", fd);

    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO)
    {
        /* closing standard I/O streams is not supported */
        errno = ENOTSUP;
        return -1;
    }
    uint32_t h = lua_r6_fd_del(fd);
    if (!h)
    {
        errno = EBADF;
        return -1;
    }
    FIO_CloseFile((FILE *) h);
    return 0;
}

ssize_t __libc_read(int fd, void* buf, size_t count)
{
    dbg_printf("__libc_read(%d,%s)\n", fd, format_memory_size(count));

    switch (fd)
    {
        case STDIN_FILENO:
            /* todo: read from IME? */
            errno = ENOSYS;
            return -1;

        case STDOUT_FILENO:
        case STDERR_FILENO:
            /* idk */
            errno = ENOTSUP;
            return -1;

        default:
        {
            uint32_t h = lua_r6_fd_h(fd);
            if (!h) { errno = EBADF; return -1; }
            return FIO_ReadFile((FILE *) h, buf, count);
        }
    }
}
#else
int __libc_open(const char * fn, int flags, ...)
{
    dbg_printf("__libc_open(%s, %x) => ", fn, flags);

    /* get file size first, since we'll get asked about it later via stat */
    /* fixme: this routine appears to return 32-bit size only */
    uint32_t filesize = 0;
    if (FIO_GetFileSize(fn, &filesize) != 0)
    {
        if (!(flags & O_CREAT))
        {
            dbg_printf("ERR_SIZE\n");
            errno = ENOENT;
            return -1;
        }
    }
    
    /* not sure if correct */
    int fd = 
        (flags & O_CREAT) && (flags & O_APPEND) ?
            (int) FIO_CreateFileOrAppend(fn) :
        (flags & O_CREAT) ?
            (int) FIO_CreateFile(fn) :
            (int) FIO_OpenFile(fn, flags);

    if (!fd)
    {
        dbg_printf("ERR_OPEN\n");
        errno = ENOENT;
        return -1;
    }
    
    if (fd <= STDERR_FILENO || fd > 0x10)
    {
        fprintf(stderr, "fixme: invalid file descriptor (%d)\n", fd);
        FIO_CloseFile((void*)fd);
        errno = ENOENT;
        return -1;
    }
    
    filesizes[fd & 0xF] = filesize;
    
    dbg_printf("%d\n", fd);
    return fd;
}

int __libc_close(int fd)
{
    dbg_printf("__libc_close(%d)\n", fd);

    switch (fd)
    {
        case STDIN_FILENO:
        case STDOUT_FILENO:
        case STDERR_FILENO:
            /* closing standard I/O streams is not supported */
            errno = ENOTSUP;
            return -1;
        
        case STDERR_FILENO+1 ... 15:
            filesizes[fd] = 0;
            FIO_CloseFile((void*)fd);
            return 0;
        
        default:
            errno = EINVAL;
            return -1;
    }
}

ssize_t __libc_read(int fd, void* buf, size_t count)
{
    dbg_printf("__libc_read(%d,%s)\n", fd, format_memory_size(count));

    switch (fd)
    {
        case STDIN_FILENO:
            /* todo: read from IME? */
            errno = ENOSYS;
            return -1;
        
        case STDOUT_FILENO:
        case STDERR_FILENO:
            /* idk */
            errno = ENOTSUP;
            return -1;
        
        default:
            return FIO_ReadFile((void*) fd, buf, count);
    }
}
#endif

ssize_t __libc_write(int fd, const void* buf, size_t count)
{
    dbg_printf("__libc_write(%d,%s,%s)\n", fd, buf, format_memory_size(count));
    
    switch (fd)
    {
        case STDIN_FILENO:
            /* idk */
            errno = ENOTSUP;
            return -1;
        
        case STDOUT_FILENO:
        case STDERR_FILENO:
        {
            if (fd == STDERR_FILENO)
            {
                /* pop the console on error */
                console_show();
            }

            /* the buffer is not null-terminated */
            if (count > 0)
            {
                int old = cli();
                char* msg = (char*) buf;
                int last_char = msg[count-1];
                msg[count-1] = 0;
                console_puts(msg);
                console_puts((char*) &last_char);
                msg[count-1] = last_char;
                sei(old);
            }

            return count;
        }
        default:
        {
#ifdef CONFIG_R6_LUA
            uint32_t h = lua_r6_fd_h(fd);     /* M16.0 (spec 5.6 F1) */
            if (!h) { errno = EBADF; return -1; }
            return FIO_WriteFile((FILE *) h, buf, count);
#else
            return FIO_WriteFile((void*) fd, buf, count);
#endif
        }
    }
}

ssize_t write(int fd , const void* buf, size_t count)
{
    return __libc_write(fd, buf, count);
}

#ifdef CONFIG_R6_LUA
/* M16.0 (spec 5.6 F1): through the fd table */
off_t lseek(int fd, off_t offset, int whence)
{
    dbg_printf("lseek(%d,%d,%d)\n", fd, offset, whence);
    uint32_t h = lua_r6_fd_h(fd);
    if (!h) { errno = EBADF; return -1; }
    return FIO_SeekSkipFile((FILE *) h, offset, whence);
}

int fstat(int fd, struct stat * buf)
{
    /* fixme: dummy implementation */
    memset(buf, 0, sizeof(*buf));
    buf->st_size = lua_r6_fd_size(fd);
    return 0;
}
#else
off_t lseek(int fd, off_t offset, int whence)
{
    dbg_printf("lseek(%d,%d,%d)\n", fd, offset, whence);
    return FIO_SeekSkipFile((void*) fd, offset, whence);
}

int fstat(int fd, struct stat * buf)
{
    /* fixme: dummy implementation */
    dbg_printf("fstat(%d,%x) size=%d\n", fd, buf, filesizes[fd & 0xF]);
    memset(buf, 0, sizeof(*buf));
    if (fd == (fd & 0xF))
    {
        buf->st_size = filesizes[fd & 0xF];
    }
    return 0;
}
#endif

int __rt_sigprocmask(int how, void* set, void* oldsetm, long nr)
{
    /* idk what I'm supposed to do here */
    printf("__rt_sigprocmask\n");
    return 0;
}

int isatty(int desc)
{
    return desc == STDIN_FILENO  ||
           desc == STDOUT_FILENO ||
           desc == STDERR_FILENO ;
}

char* getenv(const char* name)
{
    //ML doesn't have environment variables
    return NULL;
}

void __thread_doexit(int doexit)
{
    printf("__thread_doexit(%d)\n", doexit);
}

int atexit(void (*f)(void))
{
    printf("atexit(%x)\n", f);
    return 0;
}

FILE* tmpfile(void)
{
    printf("tmpfile\n");
    return 0;
}

extern void * __mem_malloc( size_t len, unsigned int flags, const char *file, unsigned int line);
extern void __mem_free( void * buf);

void* malloc(size_t size)
{
    /* this appears to be called only from dietlibc stdio
     * if it has round values, assume it's fio_malloc, for buffering files */
    int is_fio = !(size & 0x3FF);
    dbg_printf("%smalloc(%s)\n", is_fio ? "fio_" : "", format_memory_size(size));
#ifdef CONFIG_R6_LUA
    r6_lua_fault(R6LF_MALLOC);   /* M16.0 (spec 4.8): count only */
    r6_lua_stat()->mallocs++;
#endif
    
    void* ans = 0;
    
    if (size < 1024 && !is_fio)
    {
        /* process small requests with umm_malloc */
        ans = umm_malloc(size);
    }
    
    if (ans == 0)
    {
        /* process large and/or failed requests with core malloc wrapper */
#ifdef CONFIG_R6_LUA
        ans = lua_r6_core_alloc(size, is_fio ? 3 : 0);    /* M16.0 (spec 5.5): the 1 MiB floor */
#else
        ans = __mem_malloc(size, is_fio ? 3 : 0, "lua_stdio", 0);
#endif
    }
    
    return ans;
}

void free(void* ptr)
{
    if (umm_ptr_in_heap(ptr))
    {
        umm_free(ptr);
    }
    else
    {
        __mem_free(ptr);
    }
}

int core_reallocs = 0;
int core_reallocs_size = 0;

void* my_realloc(void* ptr, size_t size)
{
#ifdef CONFIG_R6_LUA
    r6_lua_fault(R6LF_MALLOC);   /* M16.0 (spec 4.8): count only (Lua itself goes through lua_r6_realloc) */
    r6_lua_stat()->mallocs++;
#endif
    int use_umm =
        (umm_ptr_in_heap(ptr)) ||
        (ptr == 0 && size < 1024);
    
    void* ans = 0;
    
    if (use_umm)
    {
        ans = umm_realloc(ptr, size);

#if 0
        /* apparently not needed, as Lua will run its garbage collector on realloc failure */
        /* even the large api_test.lua script runs comfortably with just the UMM heap */
        /* todo: if we find the UMM heap is not enough, we may return failure just once,
         * that way Lua will call its garbage collector, hopefully succeeding;
         * however, if the same allocation fails a second time, we may fall back to core malloc
         */
        if (ans == 0 && size > 0)
        {
            /* umm_realloc failed? try again using core malloc */
            ans = __mem_malloc(size, 0, "lua", __LINE__);
            if (ans) memcpy(ans, ptr, size);
            umm_free(ptr);
            core_reallocs++;
            core_reallocs_size += size;
        }
#endif
    }
    else
    {
        ans = realloc(ptr, size);
    }
    
    return ans;
}

void abort()
{
#ifdef CONFIG_R6_LUA
    lua_r6_abort();     /* M16.0 (spec 4.6): back to load_script's anchor, else sleep (no busy loop) */
#else
    //what should we do here?
    printf("abort\n");
    while(1);
#endif
}

//taken from: http://stackoverflow.com/questions/2302969/how-to-implement-char-ftoafloat-num-without-sprintf-library-function-i
//and modified to use float instead of double

static float PRECISION = 0.000001;

int ftoa(char *s, float n) {
    // handle special cases
    if (isnan(n)) {
        strcpy(s, "nan");
    } else if (isinf(n)) {
        strcpy(s, "inf");
    } else if (n == 0.0) {
        strcpy(s, "0");
    } else {
        int digit, m, m1 = 0;
        char *c = s;
        int neg = (n < 0);
        if (neg)
            n = -n;
        // round to our precision
        n += PRECISION / 2;
        // calculate magnitude
        m = log10f(n);
        int useExp = (m >= 14 || (neg && m >= 9) || m <= -9);
        if (neg)
            *(c++) = '-';
        // set up for scientific notation
        if (useExp) {
            if (m < 0)
                m -= 1.0;
            n = n / powf(10.0, m);
            m1 = m;
            m = 0;
        }
        if (m < 1.0) {
            m = 0;
        }
        // convert the number
        while (n > PRECISION || m >= 0) {
            float weight = powf(10.0, m);
            if (weight > 0 && !isinf(weight)) {
                digit = floorf(n / weight);
                n -= (digit * weight);
                *(c++) = '0' + digit;
            }
            if (m == 0 && n > 0)
                *(c++) = '.';
            m--;
        }
        if (useExp) {
            // convert the exponent
            int i, j;
            *(c++) = 'e';
            if (m1 > 0) {
                *(c++) = '+';
            } else {
                *(c++) = '-';
                m1 = -m1;
            }
            m = 0;
            while (m1 > 0) {
                *(c++) = '0' + m1 % 10;
                m1 /= 10;
                m++;
            }
            c -= m;
            for (i = 0, j = m-1; i<j; i++, j--) {
                // swap without temporary
                c[i] ^= c[j];
                c[j] ^= c[i];
                c[i] ^= c[j];
            }
            c += m;
        }
        *(c) = '\0';
    }
    return strlen(s);
}
