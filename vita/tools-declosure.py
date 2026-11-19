"""Rewrite `StateControllerBase(sc).run(c, func(P byte, exp []BytecodeExp) bool { BODY })`
as a plain loop over the parameters (TinyGo heap-allocates every captured
variable of such closures, on every controller run)."""
import re, sys
p = sys.argv[1]
s = open(p).read()
pat = re.compile(r'\n(\t+)StateControllerBase\(sc\)\.run\(c, func\((\w+) byte, exp \[\]BytecodeExp\) bool \{\n')
out = []; pos = 0; n = 0; skipped = 0; lab = 0
for m in pat.finditer(s):
    if m.start() < pos: continue
    ind, pid = m.group(1), m.group(2)
    i = m.end(); depth = 1; j = i
    # brace match (ignore braces in strings/runes/comments roughly)
    while depth:
        ch = s[j]
        if ch == '"':
            j += 1
            while s[j] != '"':
                if s[j] == '\\': j += 1
                j += 1
        elif ch == '`':
            j = s.index('`', j + 1)
        elif ch == "'":
            j += 1
            while s[j] != "'":
                if s[j] == '\\': j += 1
                j += 1
        elif s.startswith('//', j):
            j = s.index('\n', j) - 1
        elif ch == '{': depth += 1
        elif ch == '}': depth -= 1
        j += 1
    body = s[i:j - 1]
    if not s.startswith(')', j):
        skipped += 1; continue
    end = j + 1  # after ')'
    if 'func(' in body or 'defer ' in body or re.search(r'\breturn\b(?! (true|false)\b)', body):
        skipped += 1; continue
    lab += 1; L = 'scParams%d' % lab
    uses_ret = bool(re.search(r'\breturn (true|false)\b', body))
    # a trailing "return true" at the closure's top level is just the loop end
    body2 = re.sub(r'\n%s\treturn true\n%s$' % (ind, ind), '\n' + ind, body)
    body2 = body2.replace('return true', 'continue ' + L).replace('return false', 'break ' + L)
    used_label = L in body2
    hdr = '\n'
    if used_label: hdr += ind + L + ':\n'
    hdr += ind + 'for _, scParam := range StateControllerBase(sc) {\n'
    hdr += ind + '\t%s, exp := scParam.id, scParam.exp\n' % pid
    hdr += ind + '\t_, _ = %s, exp\n' % pid
    out.append(s[pos:m.start()]); out.append(hdr + body2 + '}')
    pos = end; n += 1
out.append(s[pos:])
open(p, 'w').write(''.join(out))
print('converted', n, 'skipped', skipped)
