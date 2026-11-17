"""Project compatibility check for the effect's params (no AE needed).

AE matches a saved project's values to an effect's params by param ID. This compares the params of an older
version of ae/FourDWarper.cpp (read from git) with the current file: every old param must still exist with the
same ID, the same type and the same default, and no ID may be used twice.

usage: python tools/check_param_ids.py [old_commit]      (default: 112dfc9 = 0.3.2)
"""
import re, subprocess, sys

ADD = re.compile(r'PF_ADD_(LAYER|BUTTON|POPUP|SLIDER|CHECKBOX|FLOAT_SLIDERX|POINT|TOPIC|ARBITRARY2)\((.*?)\);', re.S)
END = re.compile(r'PF_END_TOPIC\((\w+)\)')


def enum_values(src, first_name, base):
    """Values of the enum that starts with first_name (handles '= n' on the first entry only)."""
    i = src.index('enum { ' + first_name)
    body = src[i + 7:src.index('};', i)]
    body = re.sub(r'/\*.*?\*/', '', body)
    body = re.sub(r'//[^\n]*', '', body)
    vals, v = {}, base
    for item in body.split(','):
        item = item.strip()
        if not item:
            continue
        if '=' in item:
            name, num = [x.strip() for x in item.split('=')]
            v = int(num)
        else:
            name = item
        vals[name] = v
        v += 1
    return vals


def params(src):
    start = src.index('static PF_Err ParamsSetup(')
    body = src[start:src.index('out_data->num_params', start)]
    if 'enum { D_SOURCE' in src:
        ids = enum_values(src, 'D_SOURCE', 1)
    else:
        ids = enum_values(src, 'P_INPUT', 0)
    out = []
    for m in list(ADD.finditer(body)) + list(END.finditer(body)):
        if m.re is END:
            kind, args = 'END_TOPIC', [m.group(1)]
        else:
            kind = m.group(1)
            args = [a.strip() for a in re.split(r',(?![^(]*\))', m.group(2))]
        pid = ids[args[6] if kind == 'ARBITRARY2' else args[-1]]   # ARBITRARY2(name, w, h, flags, ui, dflt, ID, refcon)
        name = args[0].strip('"') if kind != 'END_TOPIC' else '(end)'
        dflt = {'POPUP': lambda: args[2], 'SLIDER': lambda: args[5], 'CHECKBOX': lambda: args[2],
                'FLOAT_SLIDERX': lambda: args[5], 'POINT': lambda: args[1] + ',' + args[2]}.get(kind, lambda: '')()
        out.append((m.start(), kind, name, pid, dflt))
    out.sort()
    return [o[1:] for o in out]


def main():
    old_rev = sys.argv[1] if len(sys.argv) > 1 else '112dfc9'
    old_src = subprocess.run(['git', 'show', old_rev + ':ae/FourDWarper.cpp'], capture_output=True, text=True, check=True).stdout
    new_src = open('ae/FourDWarper.cpp', encoding='utf-8').read()
    old, new = params(old_src), params(new_src)
    bad = 0
    ids = [p[2] for p in new]
    for d in sorted(set(i for i in ids if ids.count(i) > 1)):
        print('DUPLICATE ID', d); bad += 1
    by_id = {p[2]: p for p in new}
    for kind, name, pid, dflt in old:
        n = by_id.get(pid)
        if not n:
            print('MISSING  id %2d  %s %r' % (pid, kind, name)); bad += 1
        elif n[0] != kind or n[3] != dflt:
            print('CHANGED  id %2d  %s %r default %s  ->  %s %r default %s' % (pid, kind, name, dflt, n[0], n[1], n[3])); bad += 1
        else:
            note = '' if n[1] == name else '   (renamed from %r)' % name
            print('ok       id %2d  %-13s %r%s' % (pid, kind, n[1], note))
    for kind, name, pid, dflt in new:
        if pid not in {p[2] for p in old}:
            print('new      id %2d  %-13s %r' % (pid, kind, name))
    print('%d old params, %d new params: %s' % (len(old), len(new), 'PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
