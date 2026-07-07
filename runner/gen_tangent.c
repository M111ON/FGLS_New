#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <math.h>

typedef struct { double x, y, z; } vec3;

static const vec3 VERTS[24] = {
    {+0.00000000,+0.52573111,+0.85065081},{+0.00000000,-0.52573111,+0.85065081},
    {+0.00000000,+0.52573111,-0.85065081},{+0.00000000,-0.52573111,-0.85065081},
    {+0.52573111,+0.85065081,+0.00000000},{-0.52573111,+0.85065081,+0.00000000},
    {+0.52573111,-0.85065081,+0.00000000},{-0.52573111,-0.85065081,+0.00000000},
    {+0.85065081,+0.00000000,+0.52573111},{-0.85065081,+0.00000000,+0.52573111},
    {+0.85065081,+0.00000000,-0.52573111},{-0.85065081,+0.00000000,-0.52573111},
    {+0.58614413,-0.07467460,+0.80675818},{+0.22287287,-0.92532540,+0.30675818},
    {-0.22287287,+0.92532540,-0.30675818},{-0.58614413,+0.07467460,-0.80675818},
    {+0.71921803,+0.68819096,+0.09549150},{-0.13143278,+0.68819096,+0.71352549},
    {+0.13143278,-0.68819096,-0.71352549},{-0.71921803,-0.68819096,-0.09549150},
    {+0.93819096,-0.30901699,-0.15590452},{-0.43819096,-0.30901699,+0.84409548},
    {+0.43819096,+0.30901699,-0.84409548},{-0.93819096,+0.30901699,+0.15590452},
};

static void tangent_frame(const vec3 *v, vec3 *u, vec3 *vt) {
    vec3 ref = {0,0,1};
    if (fabs(v->z) > 0.9) { ref.x = 1; ref.y = 0; ref.z = 0; }
    double d = ref.x*v->x + ref.y*v->y + ref.z*v->z;
    u->x = ref.x - d*v->x;
    u->y = ref.y - d*v->y;
    u->z = ref.z - d*v->z;
    double len = sqrt(u->x*u->x + u->y*u->y + u->z*u->z);
    if (len > 1e-12) { u->x/=len; u->y/=len; u->z/=len; }
    vt->x = v->y*u->z - v->z*u->y;
    vt->y = v->z*u->x - v->x*u->z;
    vt->z = v->x*u->y - v->y*u->x;
    len = sqrt(vt->x*vt->x + vt->y*vt->y + vt->z*vt->z);
    if (len > 1e-12) { vt->x/=len; vt->y/=len; vt->z/=len; }
}

int main() {
    int i;

    printf("/* Pre-computed tangent U for RC_VERTS[0..23] */\n");
    printf("static const rc_vec3 RC_TANGENT_U[RC_N_VERTICES] = {\n");
    for (i = 0; i < 24; i++) {
        vec3 u, v;
        tangent_frame(&VERTS[i], &u, &v);
        printf("    { %+.10f, %+.10f, %+.10f },\n", u.x, u.y, u.z);
    }
    printf("};\n\n");

    printf("/* Pre-computed tangent V for RC_VERTS[0..23] */\n");
    printf("static const rc_vec3 RC_TANGENT_V[RC_N_VERTICES] = {\n");
    for (i = 0; i < 24; i++) {
        vec3 u, v;
        tangent_frame(&VERTS[i], &u, &v);
        printf("    { %+.10f, %+.10f, %+.10f },\n", v.x, v.y, v.z);
    }
    printf("};\n");
    return 0;
}
