/* stickman.c — procedural stickman implementation
 * Self-contained: generates capsule + sphere + box as triangle soups,
 * reusing the exact winding/normal logic from demos/toon/scene.c and demos/common/mesh.c.
 */
#include "stickman.h"
#include "mu_math.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

StickmanParams stickman_default_params(void){
    StickmanParams p={0};
    p.head_radius=0.22f; p.head_y=1.58f;
    p.torso_radius=0.13f; p.torso_half_h=0.30f;
    p.limb_radius=0.052f;
    p.arm_upper_len=0.20f; p.arm_lower_len=0.20f;
    p.leg_upper_len=0.26f; p.leg_lower_len=0.26f;
    p.col_body[0]=0.11f; p.col_body[1]=0.11f; p.col_body[2]=0.11f; p.col_body[3]=1.0f;
    p.col_head[0]=0.95f; p.col_head[1]=0.95f; p.col_head[2]=0.95f; p.col_head[3]=1.0f;
    p.col_pick[0]=0.75f; p.col_pick[1]=0.55f; p.col_pick[2]=0.25f; p.col_pick[3]=1.0f;
    return p;
}

/* ---- low-level mesh helpers (DemoVertex = pos[4]+nrm[4], 32B) ---- */
static DemoVertex* push_n(DemoMesh *m, uint32_t n){
    if(m->count + n > m->capacity){
        m->capacity = m->capacity ? m->capacity*2u : 4096u;
        while(m->capacity < m->count+n) m->capacity*=2u;
        m->vertices = (DemoVertex*)realloc(m->vertices, (size_t)m->capacity*sizeof(DemoVertex));
    }
    DemoVertex *out = &m->vertices[m->count];
    m->count += n;
    return out;
}
static void set_v(DemoVertex *v,float px,float py,float pz,float nx,float ny,float nz){
    v->pos[0]=px; v->pos[1]=py; v->pos[2]=pz; v->pos[3]=1.0f;
    v->nrm[0]=nx; v->nrm[1]=ny; v->nrm[2]=nz; v->nrm[3]=0.0f;
}

static DemoRange add_sphere_range(DemoMesh *m, float r, uint32_t seg, uint32_t rings){
    DemoRange rng={(uint32_t)m->count, seg*rings*6u};
    DemoVertex *out = push_n(m, rng.count);
    for(uint32_t i=0;i<rings;i++){
        for(uint32_t j=0;j<seg;j++){
            static const uint32_t corner[6][2]={{0,0},{0,1},{1,1},{0,0},{1,1},{1,0}};
            for(int k=0;k<6;k++){
                float t = (float)M_PI*(float)(i+corner[k][0])/(float)rings;
                float p = 2.0f*(float)M_PI*(float)(j+corner[k][1])/(float)seg;
                float nx= sinf(t)*cosf(p), ny=cosf(t), nz=sinf(t)*sinf(p);
                set_v(out++, r*nx, r*ny, r*nz, nx, ny, nz);
            }
        }
    }
    return rng;
}

// Unit capsule: radius 1, cylinder y in [-1,1] + hemispherical caps
static DemoRange add_capsule_range(DemoMesh *m, uint32_t slices, uint32_t capStacks){
    uint32_t tri_side = slices*2u;
    uint32_t tri_cap  = slices*capStacks*2u; // 2 tris per quad
    DemoRange rng={(uint32_t)m->count, tri_side*3u + 2u*tri_cap*3u};
    DemoVertex *base = push_n(m, rng.count);
    DemoVertex *out = base;

    // side
    for(uint32_t s=0;s<slices;s++){
        float lon0 = 2.0f*(float)M_PI*(float)s/(float)slices;
        float lon1 = 2.0f*(float)M_PI*(float)(s+1)/(float)slices;
        float s0=sinf(lon0), c0=cosf(lon0), s1=sinf(lon1), c1=cosf(lon1);
        // quad a(top)-c(top)-d / a-d-e
        // CCW from outside
        set_v(out++, s0,-1.0f,c0, s0,0,c0);
        set_v(out++, s1, 1.0f,c1, s1,0,c1);
        set_v(out++, s0, 1.0f,c0, s0,0,c0);

        set_v(out++, s0,-1.0f,c0, s0,0,c0);
        set_v(out++, s1,-1.0f,c1, s1,0,c1);
        set_v(out++, s1, 1.0f,c1, s1,0,c1);
    }
    // caps: use lat/lon sphere_point
    for(uint32_t s=0;s<slices;s++){
        float lon0 = 2.0f*(float)M_PI*(float)s/(float)slices;
        float lon1 = 2.0f*(float)M_PI*(float)(s+1)/(float)slices;
        for(uint32_t k=0;k<capStacks;k++){
            float lat0 = 0.5f*(float)M_PI*(float)k/(float)capStacks;
            float lat1 = 0.5f*(float)M_PI*(float)(k+1)/(float)capStacks;
            float cl0=cosf(lat0), sl0=sinf(lat0), cl1=cosf(lat1), sl1=sinf(lat1);
            // top cap y_offset = +1
            float p00[3]={cl0*sinf(lon0), sl0+1.0f, cl0*cosf(lon0)};
            float n00[3]={cl0*sinf(lon0), sl0,      cl0*cosf(lon0)};
            float p10[3]={cl1*sinf(lon0), sl1+1.0f, cl1*cosf(lon0)};
            float n10[3]={cl1*sinf(lon0), sl1,      cl1*cosf(lon0)};
            float p01[3]={cl0*sinf(lon1), sl0+1.0f, cl0*cosf(lon1)};
            float n01[3]={cl0*sinf(lon1), sl0,      cl0*cosf(lon1)};
            float p11[3]={cl1*sinf(lon1), sl1+1.0f, cl1*cosf(lon1)};
            float n11[3]={cl1*sinf(lon1), sl1,      cl1*cosf(lon1)};
            set_v(out++, p00[0],p00[1],p00[2], n00[0],n00[1],n00[2]);
            set_v(out++, p11[0],p11[1],p11[2], n11[0],n11[1],n11[2]);
            set_v(out++, p10[0],p10[1],p10[2], n10[0],n10[1],n10[2]);
            set_v(out++, p00[0],p00[1],p00[2], n00[0],n00[1],n00[2]);
            set_v(out++, p01[0],p01[1],p01[2], n01[0],n01[1],n01[2]);
            set_v(out++, p11[0],p11[1],p11[2], n11[0],n11[1],n11[2]);
        }
        for(uint32_t k=0;k<capStacks;k++){
            float lat0 = -0.5f*(float)M_PI*(float)k/(float)capStacks;
            float lat1 = -0.5f*(float)M_PI*(float)(k+1)/(float)capStacks;
            float cl0=cosf(lat0), sl0=sinf(lat0), cl1=cosf(lat1), sl1=sinf(lat1);
            // bottom cap y_offset = -1
            float p00[3]={cl1*sinf(lon0), sl1-1.0f, cl1*cosf(lon0)};
            float n00[3]={cl1*sinf(lon0), sl1,      cl1*cosf(lon0)};
            float p10[3]={cl0*sinf(lon0), sl0-1.0f, cl0*cosf(lon0)};
            float n10[3]={cl0*sinf(lon0), sl0,      cl0*cosf(lon0)};
            float p01[3]={cl1*sinf(lon1), sl1-1.0f, cl1*cosf(lon1)};
            float n01[3]={cl1*sinf(lon1), sl1,      cl1*cosf(lon1)};
            float p11[3]={cl0*sinf(lon1), sl0-1.0f, cl0*cosf(lon1)};
            float n11[3]={cl0*sinf(lon1), sl0,      cl0*cosf(lon1)};
            set_v(out++, p00[0],p00[1],p00[2], n00[0],n00[1],n00[2]);
            set_v(out++, p11[0],p11[1],p11[2], n11[0],n11[1],n11[2]);
            set_v(out++, p10[0],p10[1],p10[2], n10[0],n10[1],n10[2]);
            set_v(out++, p00[0],p00[1],p00[2], n00[0],n00[1],n00[2]);
            set_v(out++, p01[0],p01[1],p01[2], n01[0],n01[1],n01[2]);
            set_v(out++, p11[0],p11[1],p11[2], n11[0],n11[1],n11[2]);
        }
    }
    return rng;
}

static DemoRange add_box_range(DemoMesh *m, float hx, float hy, float hz){
    DemoRange rng={(uint32_t)m->count, 36u};
    DemoVertex *out = push_n(m, 36);
    float h[3]={hx,hy,hz};
    static const int axes[3][3]={{0,1,2},{1,2,0},{2,0,1}};
    int idx=0;
    for(int a=0;a<3;a++){
        for(int s=0;s<2;s++){
            int ni=axes[a][0], ui=axes[a][ s?2:1 ], vi=axes[a][ s?1:2 ];
            float n[3]={0,0,0}, u[3]={0,0,0}, v[3]={0,0,0}, c[3]={0,0,0};
            n[ni]= s? -1.0f:1.0f;
            c[ni]= n[ni]*h[ni];
            u[ui]= h[ui]; v[vi]= h[vi];
            static const float k[6][2]={{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
            for(int kidx=0;kidx<6;kidx++){
                float px=c[0]+k[kidx][0]*u[0]+k[kidx][1]*v[0];
                float py=c[1]+k[kidx][0]*u[1]+k[kidx][1]*v[1];
                float pz=c[2]+k[kidx][0]*u[2]+k[kidx][1]*v[2];
                set_v(&out[idx++], px,py,pz, n[0],n[1],n[2]);
            }
        }
    }
    return rng;
}

static void set_model(DemoObject *o, float x,float y,float z, float yaw, float scale, const float col[4]){
    float c=cosf(yaw)*scale, s=sinf(yaw)*scale;
    memset(o,0,sizeof *o);
    o->model[0]=c; o->model[2]=-s;
    o->model[5]=scale;
    o->model[8]=s; o->model[10]=c;
    o->model[12]=x; o->model[13]=y; o->model[14]=z; o->model[15]=1.0f;
    o->color[0]=col[0]; o->color[1]=col[1]; o->color[2]=col[2]; o->color[3]=col[3];
}

static void place_limb(DemoObject *o, float jx,float jy,float jz, const float q[4], float len, float rad, const float col[4]){
    float dir[3]={0,-1,0}; // down
    float d2[3]; mu_quat_rotate_vec3(d2, q, dir);
    float mid[3]={jx + d2[0]*len*0.5f, jy + d2[1]*len*0.5f, jz + d2[2]*len*0.5f};
    float sc[3]={rad, len*0.5f, rad};
    float m[16]; mu_mat4_trs(m, mid, q, sc);
    memcpy(o->model,m,sizeof m);
    o->color[0]=col[0]; o->color[1]=col[1]; o->color[2]=col[2]; o->color[3]=1;
}

bool stickman_build(Stickman *s, StickmanParams p, bool with_pickaxe){
    memset(s,0,sizeof *s);
    s->params=p; s->has_pickaxe=with_pickaxe;

    // 1 head sphere
    DemoRange headR = add_sphere_range(&s->mesh, 1.0f, 20, 12);
    // 1 torso capsule
    DemoRange torsoR = add_capsule_range(&s->mesh, 16, 4);
    // limbs share one capsule mesh (reused 8x via ranges)
    DemoRange limbR = add_capsule_range(&s->mesh, 12, 3);
    DemoRange pickHandleR = add_box_range(&s->mesh, 0.02f, 0.35f, 0.02f);
    DemoRange pickHeadR   = add_box_range(&s->mesh, 0.14f, 0.03f, 0.04f);

    s->ranges[STICK_HEAD]=headR;
    s->ranges[STICK_TORSO]=torsoR;
    s->ranges[STICK_ARM_L_UP]=limbR; s->ranges[STICK_ARM_L_LO]=limbR;
    s->ranges[STICK_ARM_R_UP]=limbR; s->ranges[STICK_ARM_R_LO]=limbR;
    s->ranges[STICK_LEG_L_UP]=limbR; s->ranges[STICK_LEG_L_LO]=limbR;
    s->ranges[STICK_LEG_R_UP]=limbR; s->ranges[STICK_LEG_R_LO]=limbR;
    s->ranges[STICK_PICK_HANDLE]=pickHandleR;
    s->ranges[STICK_PICK_HEAD]=pickHeadR;

    s->height = p.head_y + p.head_radius;
    // initial pose
    stickman_pose_idle(s, 0.0f);
    return true;
}

/* The pickaxe: handle sticking out of the fist along the arm's own axis, head at its tip. */
static void place_pick(Stickman *s, const float hand[3], float pitch){
    float q[4], down[3]={0,-1,0}, dir[3], m[16];
    mu_quat_axis_angle(q, (float[3]){1,0,0}, pitch);
    mu_quat_rotate_vec3(dir, q, down);
    mu_mat4_trs(m, (float[3]){hand[0]+dir[0]*0.10f, hand[1]+dir[1]*0.10f, hand[2]+dir[2]*0.10f}, q, (float[3]){1,1,1});
    memcpy(s->pose[STICK_PICK_HANDLE].model, m, sizeof m);
    s->pose[STICK_PICK_HANDLE].color[0]=0.55f; s->pose[STICK_PICK_HANDLE].color[1]=0.35f;
    s->pose[STICK_PICK_HANDLE].color[2]=0.15f; s->pose[STICK_PICK_HANDLE].color[3]=1.0f;
    mu_mat4_trs(m, (float[3]){hand[0]+dir[0]*0.45f, hand[1]+dir[1]*0.45f, hand[2]+dir[2]*0.45f}, q, (float[3]){1,1,1});
    memcpy(s->pose[STICK_PICK_HEAD].model, m, sizeof m);
    s->pose[STICK_PICK_HEAD].color[0]=0.60f; s->pose[STICK_PICK_HEAD].color[1]=0.60f;
    s->pose[STICK_PICK_HEAD].color[2]=0.65f; s->pose[STICK_PICK_HEAD].color[3]=1.0f;
}

void stickman_pose_idle(Stickman *s, float t){
    StickmanParams *p = &s->params;
    float breathe = sinf(t*1.2f)*0.008f;
    float yShoulder, yHip, hand[3];
    float qDown[4], qDownR[4], d2[3];

    /* head and torso: the torso is one capsule scaled to an ellipse */
    set_model(&s->pose[STICK_HEAD], 0, p->head_y + breathe, 0, 0, p->head_radius, p->col_head);
    {
        DemoObject *o = &s->pose[STICK_TORSO];
        memset(o, 0, sizeof *o);
        o->model[0] = p->torso_radius; o->model[5] = p->torso_half_h; o->model[10] = p->torso_radius;
        o->model[13] = p->head_y - p->head_radius - p->torso_half_h + 0.08f; o->model[15] = 1.0f;
        o->color[0] = p->col_body[0]; o->color[1] = p->col_body[1];
        o->color[2] = p->col_body[2]; o->color[3] = 1.0f;
    }
    yShoulder = s->pose[STICK_TORSO].model[13] + p->torso_half_h*0.85f;
    yHip      = s->pose[STICK_TORSO].model[13] - p->torso_half_h*0.90f;

    /* arms hang straight down, slightly out */
    mu_quat_axis_angle(qDown,  (float[3]){0,0,1},  0.15f);
    mu_quat_axis_angle(qDownR, (float[3]){0,0,1}, -0.15f);
    place_limb(&s->pose[STICK_ARM_L_UP], -0.18f, yShoulder, 0, qDown, p->arm_upper_len, p->limb_radius, p->col_body);
    place_limb(&s->pose[STICK_ARM_R_UP],  0.18f, yShoulder, 0, qDownR, p->arm_upper_len, p->limb_radius, p->col_body);
    mu_quat_rotate_vec3(d2, qDown, (float[3]){0,-1,0});
    place_limb(&s->pose[STICK_ARM_L_LO], -0.18f + d2[0]*p->arm_upper_len, yShoulder + d2[1]*p->arm_upper_len, 0,
               qDown, p->arm_lower_len, p->limb_radius*0.9f, p->col_body);
    mu_quat_rotate_vec3(d2, qDownR, (float[3]){0,-1,0});
    place_limb(&s->pose[STICK_ARM_R_LO], 0.18f + d2[0]*p->arm_upper_len, yShoulder + d2[1]*p->arm_upper_len, 0,
               qDownR, p->arm_lower_len, p->limb_radius*0.9f, p->col_body);

    /* legs: straight down */
    {
        float qLeg[4];
        mu_quat_identity(qLeg);
        place_limb(&s->pose[STICK_LEG_L_UP], -0.09f, yHip, 0, qLeg, p->leg_upper_len, p->limb_radius*1.1f, p->col_body);
        place_limb(&s->pose[STICK_LEG_R_UP],  0.09f, yHip, 0, qLeg, p->leg_upper_len, p->limb_radius*1.1f, p->col_body);
        place_limb(&s->pose[STICK_LEG_L_LO], -0.09f, yHip - p->leg_upper_len, 0, qLeg, p->leg_lower_len, p->limb_radius, p->col_body);
        place_limb(&s->pose[STICK_LEG_R_LO],  0.09f, yHip - p->leg_upper_len, 0, qLeg, p->leg_lower_len, p->limb_radius, p->col_body);
    }

    if (s->has_pickaxe) {
        mu_quat_rotate_vec3(d2, qDownR, (float[3]){0,-1,0});
        hand[0] = 0.18f + d2[0]*(p->arm_upper_len + p->arm_lower_len);
        hand[1] = yShoulder + d2[1]*(p->arm_upper_len + p->arm_lower_len);
        hand[2] = 0.0f;
        place_pick(s, hand, 0.0f);
    }
}

/* One bone: upper from the joint at `pitch` about X, lower continuing from its end. */
static void pose_chain(Stickman *s, StickmanPart up, StickmanPart lo, float x, float y,
                       float pitch, float up_len, float lo_len, float rad, float bend){
    StickmanParams *p = &s->params;
    float q[4], dir[3], elbow[3];
    mu_quat_axis_angle(q, (float[3]){1,0,0}, pitch);
    mu_quat_rotate_vec3(dir, q, (float[3]){0,-1,0});
    place_limb(&s->pose[up], x, y, 0, q, up_len, rad, p->col_body);
    elbow[0] = x + dir[0]*up_len;
    elbow[1] = y + dir[1]*up_len;
    elbow[2] = dir[2]*up_len;
    mu_quat_axis_angle(q, (float[3]){1,0,0}, pitch + bend);
    mu_quat_rotate_vec3(dir, q, (float[3]){0,-1,0});
    place_limb(&s->pose[lo], elbow[0], elbow[1], elbow[2], q, lo_len, rad*0.9f, p->col_body);
}

void stickman_pose_walk(Stickman *s, float t, float speed){
    StickmanParams *p = &s->params;
    float phase  = t*speed*5.0f;
    float swing  = sinf(phase)*0.55f;
    float yShoulder, yHip, hand[3], q[4], dir[3];

    stickman_pose_idle(s, t);
    yShoulder = s->pose[STICK_TORSO].model[13] + p->torso_half_h*0.85f;
    yHip      = s->pose[STICK_TORSO].model[13] - p->torso_half_h*0.90f;

    /* arms counter the legs, knees and elbows bend slightly */
    pose_chain(s, STICK_ARM_L_UP, STICK_ARM_L_LO, -0.18f, yShoulder,  swing, p->arm_upper_len, p->arm_lower_len, p->limb_radius, -0.30f);
    pose_chain(s, STICK_ARM_R_UP, STICK_ARM_R_LO,  0.18f, yShoulder, -swing, p->arm_upper_len, p->arm_lower_len, p->limb_radius, -0.30f);
    pose_chain(s, STICK_LEG_L_UP, STICK_LEG_L_LO, -0.09f, yHip, -swing, p->leg_upper_len, p->leg_lower_len, p->limb_radius*1.1f, 0.35f);
    pose_chain(s, STICK_LEG_R_UP, STICK_LEG_R_LO,  0.09f, yHip,  swing, p->leg_upper_len, p->leg_lower_len, p->limb_radius*1.1f, 0.35f);

    if (s->has_pickaxe) {
        mu_quat_axis_angle(q, (float[3]){1,0,0}, -swing - 0.30f);
        mu_quat_rotate_vec3(dir, q, (float[3]){0,-1,0});
        hand[0] = 0.18f + dir[0]*(p->arm_upper_len + p->arm_lower_len);
        hand[1] = yShoulder + dir[1]*(p->arm_upper_len + p->arm_lower_len);
        hand[2] = dir[2]*(p->arm_upper_len + p->arm_lower_len);
        place_pick(s, hand, -swing - 0.30f);
    }
}

void stickman_pose_mine(Stickman *s, float t){
    StickmanParams *p = &s->params;
    float swing = powf(sinf(t*7.0f)*0.5f + 0.5f, 1.8f) * 1.6f;
    float pitch = -1.9f - swing;                        /* overhead, slamming down */
    float yShoulder, hand[3], q[4], dir[3];

    stickman_pose_idle(s, t);
    yShoulder = s->pose[STICK_TORSO].model[13] + p->torso_half_h*0.85f;

    pose_chain(s, STICK_ARM_L_UP, STICK_ARM_L_LO, -0.18f, yShoulder, pitch, p->arm_upper_len, p->arm_lower_len, p->limb_radius, -0.15f);
    pose_chain(s, STICK_ARM_R_UP, STICK_ARM_R_LO,  0.18f, yShoulder, pitch, p->arm_upper_len, p->arm_lower_len, p->limb_radius, -0.15f);

    if (s->has_pickaxe) {
        mu_quat_axis_angle(q, (float[3]){1,0,0}, pitch - 0.15f);
        mu_quat_rotate_vec3(dir, q, (float[3]){0,-1,0});
        hand[0] = 0.18f + dir[0]*(p->arm_upper_len + p->arm_lower_len);
        hand[1] = yShoulder + dir[1]*(p->arm_upper_len + p->arm_lower_len);
        hand[2] = dir[2]*(p->arm_upper_len + p->arm_lower_len);
        place_pick(s, hand, pitch - 0.15f);
    }
}

void stickman_free(Stickman *s){ demo_mesh_free(&s->mesh); memset(s,0,sizeof *s); }
