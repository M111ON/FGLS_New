import json, re
with open('I:/FGLS_new/output/prose.html', 'r') as f:
    html = f.read()
idx = html.index('var PROSE_JSON =')
after = html[idx + len('var PROSE_JSON ='):]
brace = 0
for i, ch in enumerate(after):
    if ch == '{': brace += 1
    elif ch == '}': brace -= 1
    if brace == 0:
        print('Balanced at', i)
        print('After close:', repr(after[i+1:i+5]))
        break
m = re.findall(r'"name": "([^"]+)"', html)
print('Modules:', len(m))
for x in m:
    print(' ', x)
