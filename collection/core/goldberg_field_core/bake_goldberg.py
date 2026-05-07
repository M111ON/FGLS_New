import math, json, itertools

phi = (1+math.sqrt(5))/2

def all_even_perms_signs(a,b,c):
    result = set()
    for p in [(a,b,c),(b,c,a),(c,a,b)]:
        for s in itertools.product([1,-1],[1,-1],[1,-1]):
            result.add((s[0]*p[0],s[1]*p[1],s[2]*p[2]))
    return result

raw = set()
raw |= all_even_perms_signs(0,1,3*phi)
raw |= all_even_perms_signs(2,1+2*phi,phi)
raw |= all_even_perms_signs(1,2+phi,2*phi)

def norm(v):
    l = math.sqrt(sum(x*x for x in v))
    return tuple(x/l for x in v)

def dot(a,b): return sum(x*y for x,y in zip(a,b))
def dist(a,b): return math.sqrt(sum((x-y)**2 for x,y in zip(a,b)))

verts = [norm(v) for v in raw]
# deduplicate
vk = {}; vlist = []
for v in verts:
    k = tuple(round(x,7) for x in v)
    if k not in vk:
        vk[k] = len(vlist)
        vlist.append(v)
verts = vlist
print(f"V={len(verts)}")

# edges by distance
all_d = sorted(set(round(dist(verts[i],verts[j]),6)
    for i in range(len(verts)) for j in range(i+1,len(verts))))
edge_d = all_d[0]
print(f"edge_d={edge_d:.6f}, next={all_d[1]:.6f}")

thresh = (all_d[0]+all_d[1])/2
adj = [[] for _ in range(len(verts))]
for i in range(len(verts)):
    for j in range(i+1,len(verts)):
        if dist(verts[i],verts[j]) < thresh:
            adj[i].append(j)
            adj[j].append(i)

degs = [len(adj[i]) for i in range(len(verts))]
print(f"degrees: {set(degs)}, E={sum(degs)//2}")

# half-edge face tracing
def cross(a,b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def sub(a,b): return tuple(a[i]-b[i] for i in range(3))

def angle_ccw(u,v,w):
    nv = verts[v]
    def proj(p):
        d = dot(sub(p,verts[v]), nv)
        q = sub(p, tuple(d*nv[i] for i in range(3)))
        return q
    ru = proj(verts[u])
    rw = proj(verts[w])
    c = cross(ru,rw)
    co = dot(ru,rw)
    a = math.atan2(math.sqrt(dot(c,c)), co)
    if dot(c,nv) < 0: a = 2*math.pi - a
    if a <= 1e-9: a += 2*math.pi
    return a

def next_hedge(u,v):
    best_w, best_a = None, float('inf')
    for w in adj[v]:
        if w==u: continue
        a = angle_ccw(u,v,w)
        if a < best_a:
            best_a=a; best_w=w
    return best_w

faces=[]; visited=set()
for u in range(len(verts)):
    for v in adj[u]:
        if (u,v) in visited: continue
        face=[u]; a,b=u,v
        for _ in range(8):
            visited.add((a,b))
            w=next_hedge(a,b)
            if w is None or w==u: break
            face.append(b); a,b=b,w
        if 5<=len(face)<=6:
            fk=frozenset(face)
            if fk not in [frozenset(f) for f in faces]:
                faces.append(face)

pen=[f for f in faces if len(f)==5]
hex_=[f for f in faces if len(f)==6]
print(f"pen={len(pen)} hex={len(hex_)}")

# all_faces: 0..11=pen, 12..31=hex
all_faces = pen + hex_
fset_to_idx = {frozenset(f):i for i,f in enumerate(all_faces)}

# vert->faces
vf=[[] for _ in range(len(verts))]
for fi,f in enumerate(all_faces):
    for v in f: vf[v].append(fi)

trigaps=[]
for v in range(len(verts)):
    if len(vf[v])==3:
        trigaps.append({"gap_id":len(trigaps),
            "face_a":vf[v][0],"face_b":vf[v][1],"face_c":vf[v][2]})
print(f"trigaps={len(trigaps)}")

# pentagon pairs
def cen(f):
    pts=[verts[v] for v in f]
    c=tuple(sum(p[i] for p in pts)/len(pts) for i in range(3))
    return norm(c)

pc=[cen(f) for f in pen]
pairs=[]; used=set()
for i in range(12):
    if i in used: continue
    for j in range(i+1,12):
        if j in used: continue
        if dot(pc[i],pc[j])<-0.95:
            pairs.append([i,j]); used.add(i); used.add(j); break

print(f"pentagon pairs={len(pairs)}")

out={"n_verts":len(verts),"n_pen":len(pen),"n_hex":len(hex_),
     "n_faces":len(all_faces),"n_trigap":len(trigaps),
     "pentagon_pairs":pairs,"trigaps":trigaps}
with open("goldberg_gp11.json","w") as f:
    json.dump(out,f,indent=2)
print("Done")
