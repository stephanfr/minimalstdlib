#  Merge gcov JSON (gcov -j) from every translation unit into per-file line and function coverage for include/ and
#      src/: a line counts as covered if any translation unit executed it.  Usage: coverage_merge.py ROOT JSON_DIR
#      [FILE ...] (the FILE arguments also list their uncovered lines).
import gzip, json, glob, os, sys, collections
root = sys.argv[1]
lines = collections.defaultdict(dict)   # file -> line -> executed
funcs = collections.defaultdict(dict)   # file -> name -> executed
for path in glob.glob(os.path.join(sys.argv[2], '*.gcov.json.gz')):
    data = json.load(gzip.open(path))
    cwd = data.get('current_working_directory', '')
    for f in data['files']:
        name = os.path.normpath(os.path.join(cwd, f['file']))
        rel = os.path.relpath(name, root)
        if not (rel.startswith('include/') or rel.startswith('src/')):
            continue
        for l in f['lines']:
            n = l['line_number']
            lines[rel][n] = lines[rel].get(n, False) or l['count'] > 0
        for fn in f.get('functions', []):
            k = fn['demangled_name']
            funcs[rel][k] = funcs[rel].get(k, False) or fn['execution_count'] > 0
rows = []
tl = te = 0
for rel in sorted(lines):
    n = len(lines[rel]); e = sum(lines[rel].values())
    fn = len(funcs[rel]); fe = sum(funcs[rel].values())
    tl += n; te += e
    rows.append((e / n if n else 1, rel, e, n, fe, fn))
print(f"TOTAL lines {te}/{tl} = {100*te/tl:.1f}%")
for pct, rel, e, n, fe, fn in sorted(rows):
    print(f"{100*pct:5.1f}%  {e:5d}/{n:5d} lines  {fe:4d}/{fn:4d} fns  {rel}")
if len(sys.argv) > 3:
    for rel in sys.argv[3:]:
        missing = sorted(n for n, e in lines[rel].items() if not e)
        print(rel, 'missing:', missing)
