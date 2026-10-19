#!/usr/bin/env python3
# R6.190 M16.0 (spec m16/m160 3.3): the module symbol file (R6_190.sym, "address name" per line, as platform/Makefile writes it)
# gets task_create pointing to r6_task_try_create, so every module task goes through the R6 task model (Canon's task memory
# checked right before task_create, a refusal instead of Err 70). Whole names only (the file also has task_create_* lines).
#   r6_symalias.py --require <file>   exactly one line "task_create" and exactly one "r6_task_try_create", else exit 1
#   r6_symalias.py --optional <file>  without r6_task_try_create the file stays unchanged (QEMU images without CONFIG_R6_MODSUP)
# The file is rewritten sorted (as `sort`, byte order). The loader (src/module.c) checks the alias once more.
import sys

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('--require', '--optional'):
        print('usage: r6_symalias.py --require|--optional <symfile>')
        return 2
    mode, path = sys.argv[1], sys.argv[2]
    lines = open(path, encoding='latin-1').read().splitlines()
    rows = []
    for ln in lines:
        if not ln.strip():
            continue
        parts = ln.split(' ')
        if len(parts) != 2 or not parts[0] or not parts[1]:
            print('r6_symalias: bad line %r' % ln)
            return 1
        rows.append(parts)
    tc = [i for i, (a, n) in enumerate(rows) if n == 'task_create']
    tt = [i for i, (a, n) in enumerate(rows) if n == 'r6_task_try_create']
    if len(tc) != 1:
        print('r6_symalias: %d lines named task_create (want 1)' % len(tc))
        return 1
    if len(tt) == 0 and mode == '--optional':
        print('r6_symalias: no r6_task_try_create, no alias')
        return 0
    if len(tt) != 1:
        print('r6_symalias: %d lines named r6_task_try_create (want 1)' % len(tt))
        return 1
    addr = rows[tt[0]][0]
    try:
        int(addr, 16)
    except ValueError:
        print('r6_symalias: bad address %r' % addr)
        return 1
    rows[tc[0]][0] = addr
    out = sorted('%s %s' % (a, n) for a, n in rows)
    with open(path, 'w', encoding='latin-1', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('r6_symalias: task_create -> r6_task_try_create (0x%s)' % addr)
    return 0

if __name__ == '__main__':
    sys.exit(main())
