"""Extract, from bytecode.go, the stack effect and operand length of each
opcode case in BytecodeExp.run and the run_const/run_ex/run_ex2/run_ex3
sub-opcode switches.

Output: Go source with tables used by the expression compiler:
  bcGenArity[op]       = number of stack inputs (-1 = unknown/unsupported)
  bcGenOperand[op]     = operand bytes after the opcode (-1 = unknown)
  bcGenSub*Arity/Operand for each sub-opcode family.

Only "simple" cases are accepted: the body pushes exactly one value or
replaces the top, pops a fixed number of values, and advances the cursor by
fixed amounts. Anything else (loops over the stack, helper calls that touch
the stack, conditional cursor moves) is marked unsupported (-1); the compiler
then leaves the whole expression to the interpreter.
"""
import re
import sys

src = open(sys.argv[1]).read()


def func_body(name):
    m = re.search(r'^func \(be BytecodeExp\) %s\(' % re.escape(name), src, re.M)
    if not m:
        raise SystemExit('no func ' + name)
    start = src.index('{', m.end()) + 1
    depth = 1
    i = start
    while depth:
        ch = src[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        i += 1
    return src[start:i - 1]


def outer_switch_cases(body):
    """Split the first top-level 'switch opc {' of body into case clauses."""
    best = None
    for m in re.finditer(r'\n\t+switch opc \{', body):
        n = body.count('case ', m.end(), m.end() + 200000)
        j = body.index('{', m.start()) + 1
        d = 1
        k = j
        while d:
            if body[k] == '{':
                d += 1
            elif body[k] == '}':
                d -= 1
            k += 1
        cnt = body[j:k].count('\ncase') + len(re.findall(r'\n\t+case ', body[j:k]))
        if best is None or cnt > best[0]:
            best = (cnt, m)
    if best is None:
        raise SystemExit('no switch')
    m = best[1]
    i = body.index('{', m.start()) + 1
    depth = 1
    sw_start = i
    while depth:
        ch = body[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        i += 1
    sw = body[sw_start:i - 1]
    # case lines at the switch's own indentation
    ind = re.search(r'\n(\t+)case ', sw).group(1)
    parts = re.split(r'\n%s(case [^\n]*:|default:)' % ind, sw)
    cases = []
    for k in range(1, len(parts), 2):
        head, body_ = parts[k], parts[k + 1]
        cases.append((head, body_))
    return cases


def analyse(body, cursor_var):
    """Return (arity, operand_bytes) or None."""
    b = body
    # Reject control flow / helpers that may touch the stack in ways we
    # cannot see.
    bad = ['for ', 'goto', 'continue', 'Swap()', 'Dup()', 'bcStack.Alloc', 'run(', 'run_',
           'pushTrigger', 'cmpConst', 'i = len(be)', 'evalB', 'evalI', 'evalF', 'evalS', 'fallthrough',
           'bcStack[', 'len(sys.bcStack)', 'return']
    for w in bad:
        if w in b:
            return None
    pops = len(re.findall(r'sys\.bcStack\.Pop\(\)', b))
    pushes = len(re.findall(r'sys\.bcStack\.Push[A-Z0-9]*\(', b))
    tops = len(re.findall(r'sys\.bcStack\.Top\(\)', b))
    # Operand reads through helpers: ReadIntAt / ReadPoolStringAt advance the
    # cursor by 4. Any other helper that receives the cursor is unsupported.
    ref = '&i' if cursor_var == 'i' else 'i'
    helper_adv = 4 * len(re.findall(r'\.(?:ReadIntAt|ReadPoolStringAt)\(%s\)' % re.escape(ref), b))
    rest = re.sub(r'\.(?:ReadIntAt|ReadPoolStringAt)\(%s\)' % re.escape(ref), '', b)
    if cursor_var == 'i':
        if '&i' in rest:
            return None
    else:
        if re.search(r'[(,]\s*i\s*[,)]', rest):
            return None
    # Cursor moves
    if cursor_var == 'i':
        adv = 0
        for m in re.finditer(r'\bi \+= (\d+)', b):
            adv += int(m.group(1))
        adv += len(re.findall(r'\bi\+\+', b))
        if re.search(r'\bi \+= [^0-9]', b) or re.search(r'\bi = ', b) or 'PeekLength' in b:
            return None
    else:
        adv = 0
        for m in re.finditer(r'\*i \+= (\d+)', b):
            adv += int(m.group(1))
        adv += len(re.findall(r'\(\*i\)\+\+', b))
        if re.search(r'\*i \+= [^0-9]', b) or re.search(r'\*i = ', b) or 'PeekLength' in b:
            return None
    # Branches with different stack effects are rejected: require the body
    # to be either all-push (no top), or top-replacement.
    if 'if ' in b or 'switch' in b or 'else' in b:
        # allow simple ifs only when every branch pushes/replaces once:
        # conservative heuristic -- count must be consistent
        if pushes > 1 or (pushes and tops):
            return None
    adv += helper_adv
    if pushes == 1 and tops == 0:
        return (pops, adv)
    if pushes == 0 and tops >= 1:
        # *Top() = f(Top()) or be.op(Top(), v2): replaces top
        if re.search(r'\*sys\.bcStack\.Top\(\) =|be\.[a-zA-Z]+\(sys\.bcStack\.Top\(\)|\bsys\.bcStack\.Top\(\)\.Set', b):
            return (pops + 1, adv)
        return None
    return None


def opname_list(head):
    if head == 'default:':
        return []
    return [x.strip() for x in head[len('case '):-1].split(',')]


out = ['// Code generated by genarity.py from bytecode.go. DO NOT EDIT.', '', 'package main', '']


def emit(table, fn, cursor):
    cases = outer_switch_cases(func_body(fn))
    rows = []
    for head, body in cases:
        names = opname_list(head)
        if not names:
            continue
        r = analyse(body, cursor)
        if r is None:
            continue
        for n in names:
            if not re.match(r'^OC_[A-Za-z0-9_]+$', n):
                continue
            rows.append((n, r[0], r[1]))
    out.append('var %s = map[OpCode][2]int8{' % table)
    for n, a, o in rows:
        out.append('\t%s: {%d, %d},' % (n, a, o))
    out.append('}')
    out.append('')
    return len(rows), len(cases)


stats = []
stats.append(('runOC', emit('bcGenMain', 'runOC', 'i')))
for fam, fn in (('Const', 'run_const'), ('Ex', 'run_ex'), ('Ex2', 'run_ex2'), ('Ex3', 'run_ex3')):
    stats.append((fn, emit('bcGenSub' + fam, fn, '*i')))
open(sys.argv[2], 'w').write('\n'.join(out))
for s in stats:
    print(s)
