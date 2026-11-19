import re, os, sys, collections, json
root = sys.argv[1]; out = sys.argv[2]
ctrl = collections.Counter(); ctrl_chars = collections.defaultdict(set)
trig = collections.Counter(); params = collections.Counter()
redir = collections.Counter(); files = 0; lines_total = 0
ident = re.compile(r'[A-Za-z_][A-Za-z0-9_.]*')
kw_redirect = {'parent','root','helper','target','partner','enemy','enemynear','playerid','player','p2','stateowner','helperindex','playerindex'}
for dp, dn, fn in os.walk(root, followlinks=True):
    for f in fn:
        fl = f.lower()
        if not (fl.endswith('.st') or fl.endswith('.cns') or fl.endswith('.cmd')): continue
        p = os.path.join(dp, f); files += 1
        ch = os.path.relpath(p, root).split('/')[1] if '/chars/' in p or p.startswith(root+'/chars') else os.path.relpath(p, root).split('/')[0]
        insec = False
        for raw in open(p, encoding='latin-1'):
            lines_total += 1
            line = raw.split(';')[0].strip()
            if not line: continue
            if line.startswith('['):
                insec = bool(re.match(r'(?i)\[state\b', line)); continue
            if not insec or '=' not in line: continue
            k, v = line.split('=', 1); k = k.strip().lower(); v = v.strip()
            if k == 'type':
                t = v.lower().split()[0] if v else ''
                ctrl[t] += 1; ctrl_chars[t].add(ch)
            elif re.match(r'trigger(all|\d+)$', k) or True:
                if re.match(r'trigger(all|\d+)$', k) or k not in ('type',):
                    if not re.match(r'trigger(all|\d+)$', k): params[re.sub(r'\d+$','',k)] += 1
                    v2 = re.sub(r'"[^"]*"', '""', v)
                    for m in ident.finditer(v2):
                        w = m.group(0).lower()
                        if w in kw_redirect: redir[w] += 1
                        trig[w] += 1
json.dump({'files': files, 'lines': lines_total,
           'controllers': ctrl.most_common(), 'controller_chars': {k: len(v) for k, v in ctrl_chars.items()},
           'identifiers': trig.most_common(), 'params': params.most_common(), 'redirects': redir.most_common()},
          open(out, 'w'), indent=1)
print('files', files, 'lines', lines_total, 'controller types', len(ctrl), 'identifiers', len(trig))
print('top controllers:', ', '.join('%s:%d' % kv for kv in ctrl.most_common(40)))
