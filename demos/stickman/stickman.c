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

static void set_model_trs(DemoObject *o, const float t[3], const float q[4], float scale, const float col[4]){
    float m[16];
    mu_mat4_trs(m, t, q, (float[3]){scale,scale,scale});
    // capsule is unit radius & height ±1, so stretch Y to desired half-length
    memcpy(o->model, m, sizeof m);
    o->color[0]=col[0]; o->color[1]=col[1]; o->color[2]=col[2]; o->color[3]=col[3];
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

    s->range[STICK_HEAD]=headR;
    s->range[STICK_TORSO]=torsoR;
    s->range[STICK_ARM_L_UP]=limbR; s->range[STICK_ARM_L_LO]=limbR;
    s->range[STICK_ARM_R_UP]=limbR; s->range[STICK_ARM_R_LO]=limbR;
    s->range[STICK_LEG_L_UP]=limbR; s->range[STICK_LEG_L_LO]=limbR;
    s->range[STICK_LEG_R_UP]=limbR; s->range[STICK_LEG_R_LO]=limbR;
    s->range[STICK_PICK_HANDLE]=pickHandleR;
    s->range[STICK_PICK_HEAD]=pickHeadR;

    s->height = p.head_y + p.head_radius;
    // initial pose
    stickman_pose_idle(s, 0.0f);
    return true;
}

void stickman_pose_idle(Stickman *s, float t){
    StickmanParams *p=&s->params;
    float breathe = sinf(t*1.2f)*0.008f;

    // head: slight bob
    set_model(&s->pose[STICK_HEAD], 0, p->head_y + breathe, 0, 0, p->head_radius, p->col_head);

    // torso: single capsule scaled non-uniformly: radius XZ, half_h Y
    // We encode non-uniform scale via model matrix columns
    {
        DemoObject *o=&s->pose[STICK_TORSO];
        memset(o,0,sizeof *o);
        o->model[0]=p->torso_radius; o->model[5]=p->torso_half_h; o->model[10]=p->torso_radius;
        o->model[12]=0; o->model[13]=p->head_y - p->head_radius - p->torso_half_h + 0.08f; o->model[14]=0; o->model[15]=1;
        o->color[0]=p->col_body[0]; o->color[1]=p->col_body[1]; o->color[2]=p->col_body[2]; o->color[3]=1;
    }

    float yShoulder = s->pose[STICK_TORSO].model[13] + p->torso_half_h*0.85f;
    float yHip = s->pose[STICK_TORSO].model[13] - p->torso_half_h*0.9f;

    // arms hang straight down, slightly out
    float qDown[4]; mu_quat_axis_angle(qDown, (float[3]){0,0,1}, 0.15f);
    float qDownR[4]; mu_quat_axis_angle(qDownR, (float[3]){0,0,1}, -0.15f);

    float tL[3]={-0.18f, yShoulder, 0}, tR[3]={0.18f, yShoulder, 0};
    // upper arm: translate to shoulder, rotate, scale Y = length
    // we store capsule radius via model scale, length via Y scale
    // limbR is unit capsule (r=1, h=±1 => total height 2). So scale = desired.
    // For limbs we need to offset pivot: capsule center should be at mid-point of bone
    // Easiest: model = T_shoulder * R * T(0,-arm_upper_len,0) * S(r, len, r) but since mesh centered, just T_shoulder + offset along rotated -Y
    // Simplified: build TRS then translate along local Y before scale: we'll just compose manually
    float sArm = p->limb_radius;
    float lenUp = p->arm_upper_len, lenLo = p->arm_lower_len;
    // helper to place limb: position = joint + dir*len/2, orientation = dir rotation
    // For idle, dir = (0,-1,0) with slight outward tilt
    auto void place_limb(DemoObject *o, float jx,float jy,float jz, float q[4], float len, float rad, const float col[4]){
        float dir[3]={0,-1,0}; // down
        // rotate dir by quat
        float d2[3]; mu_quat_rotate(d2, q, dir);
        float mid[3]={jx + d2[0]*len*0.5f, jy + d2[1]*len*0.5f, jz + d2[2]*len*0.5f};
        float qs[4]; memcpy(qs,q,sizeof qs);
        // build matrix: T(mid) * R(q) * S(rad, len/2 ?, wait capsule height is 2 units -> scale Y should be len/2 ???)
        // Unit capsule: total height = 2 (cylinder) + 2*radius caps? Actually our capsule is r=1, h=±1 => total 4? Wait: cylinder 2 + caps 2 =4. But we scaled uniformly before. For limb we want length = len. So scaleY = len / 2.0f (since base height 2 in Y? Let's check: cylinder from -1 to +1 => height 2, plus caps radius 1 each => total 4. Hmm to keep limb length exact, use len/2.0f).
        // Let's approximate: scaleY = len/2.0f, scaleXZ = rad
        float sc[3]={rad, len*0.5f, rad};
        float m[16]; mu_mat4_trs(m, mid, qs, sc);
        memcpy(o->model,m,sizeof m);
        o->color[0]=col[0]; o->color[1]=col[1]; o->color[2]=col[2]; o->color[3]=1;
    }

    place_limb(&s->pose[STICK_ARM_L_UP], tL[0],tL[1],tL[2], qDown, lenUp, sArm, p->col_body);
    // elbow
    {
        float dir[3]={0,-1,0}; float d2[3]; mu_quat_rotate(d2,qDown,dir);
        float elbow[3]={tL[0]+d2[0]*lenUp, tL[1]+d2[1]*lenUp, tL[2]+d2[2]*lenUp};
        place_limb(&s->pose[STICK_ARM_L_LO], elbow[0],elbow[1],elbow[2], qDown, lenLo, sArm*0.9f, p->col_body);
    }
    place_limb(&s->pose[STICK_ARM_R_UP], tR[0],tR[1],tR[2], qDownR, lenUp, sArm, p->col_body);
    {
        float dir[3]={0,-1,0}; float d2[3]; mu_quat_rotate(d2,qDownR,dir);
        float elbow[3]={tR[0]+d2[0]*lenUp, tR[1]+d2[1]*lenUp, tR[2]+d2[2]*lenUp};
        place_limb(&s->pose[STICK_ARM_R_LO], elbow[0],elbow[1],elbow[2], qDownR, lenLo, sArm*0.9f, p->col_body);
    }

    // legs: straight
    float qLeg[4]; mu_quat_axis_angle(qLeg, (float[3]){0,0,1}, 0.0f);
    place_limb(&s->pose[STICK_LEG_L_UP], -0.09f, yHip, 0, qLeg, p->leg_upper_len, p->limb_radius*1.1f, p->col_body);
    {
        float knee[3]={-0.09f, yHip - p->leg_upper_len, 0};
        place_limb(&s->pose[STICK_LEG_L_LO], knee[0],knee[1],knee[2], qLeg, p->leg_lower_len, p->limb_radius, p->col_body);
    }
    place_limb(&s->pose[STICK_LEG_R_UP], 0.09f, yHip, 0, qLeg, p->leg_upper_len, p->limb_radius*1.1f, p->col_body);
    {
        float knee[3]={0.09f, yHip - p->leg_upper_len, 0};
        place_limb(&s->pose[STICK_LEG_R_LO], knee[0],knee[1],knee[2], qLeg, p->leg_lower_len, p->limb_radius, p->col_body);
    }

    if(s->has_pickaxe){
        // pick parented to right hand
        float hand[3]; { float dir[3]={0,-1,0}; float d2[3]; mu_quat_rotate(d2,qDownR,dir); hand[0]=tR[0]+d2[0]*lenUp + d2[0]*lenLo; hand[1]=tR[1]+d2[1]*lenUp + d2[1]*lenLo; hand[2]=0; }
        // handle
        DemoObject *h=&s->pose[STICK_PICK_HANDLE];
        float qPick[4]; mu_quat_axis_angle(qPick, (float[3]){0,0,1}, 0.6f);
        float m2[16], qComb[4]; mu_quat_mul(qComb, qDownR, qPick);
        mu_mat4_trs(m2, (float[3]){hand[0]+0.05f, hand[1]+0.1f, 0}, qComb, (float[3]){1,1,1});
        memcpy(h->model,m2,sizeof m2); h->color[0]=0.55f;h->color[1]=0.35f;h->color[2]=0.15f;h->color[3]=1;
        DemoObject *ph=&s->pose[STICK_PICK_HEAD];
        mu_mat4_trs(m2, (float[3]){hand[0]+0.05f, hand[1]+0.45f, 0}, qComb, (float[3]){1,1,1});
        memcpy(ph->model,m2,sizeof m2); ph->color[0]=0.6f;ph->color[1]=0.6f;ph->color[2]=0.65f;ph->color[3]=1;
    }
}

void stickman_pose_walk(Stickman *s, float t, float speed){
    stickman_pose_idle(s,t); // base
    float phase = t*speed*5.0f;
    float swing = sinf(phase)*0.55f;
    float swing2= sinf(phase + 0.6f)*0.45f;
    // overwrite arm rotations with swing
    // left arm forward when right leg forward (opposite)
    // We'll just perturb the idle quats
    // For brevity we recompute shoulders with pitch X rotation
    StickmanParams *p=&s->params;
    float yShoulder = s->pose[STICK_TORSO].model[13] + p->torso_half_h*0.85f;
    float yHip = s->pose[STICK_TORSO].model[13] - p->torso_half_h*0.9f;

    auto void place_limb_pitched(DemoObject *o,float jx,float jy,float jz,float pitch, float len, float rad){
        float q[4]; mu_quat_axis_angle(q, (float[3]){1,0,0}, pitch);
        float qOut[4]; mu_quat_axis_angle(qOut, (float[3]){0,0,1}, 0.1f);
        float q2[4]; mu_quat_mul(q2, q, qOut);
        float dir[3]={0,-1,0}; float d2[3]; mu_quat_rotate(d2,q2,dir);
        float mid[3]={jx+d2[0]*len*0.5f, jy+d2[1]*len*0.5f, jz+d2[2]*len*0.5f};
        float m[16]; mu_mat4_trs(m, mid, q2, (float[3]){rad, len*0.5f, rad});
        memcpy(o->model,m,sizeof m);
    }
    place_limb_pitched(&s->pose[STICK_ARM_L_UP], -0.18f,yShoulder,0, swing, p->arm_upper_len, p->limb_radius);
    place_limb_pitched(&s->pose[STICK_ARM_L_LO], -0.18f,yShoulder - p->arm_upper_len*0.9f, 0.08f*sinf(phase)*0.3f, swing+swing2*0.5f, p->arm_lower_len, p->limb_radius*0.9f);
    place_limb_pitched(&s->pose[STICK_ARM_R_UP], 0.18f,yShoulder,0, -swing, p->arm_upper_len, p->limb_radius);
    place_limb_pitched(&s->pose[STICK_ARM_R_LO], 0.18f,yShoulder + p->arm_upper_len*0.9f*sinf(phase+3.14f)*0.2f, 0, -swing - swing2*0.5f, p->arm_lower_len, p->limb_radius*0.9f);

    place_limb_pitched(&s->pose[STICK_LEG_L_UP], -0.09f,yHip,0, -swing, p->leg_upper_len, p->limb_radius*1.1f);
    place_limb_pitched(&s->pose[STICK_LEG_R_UP], 0.09f,yHip,0, swing, p->leg_upper_len, p->limb_radius*1.1f);
    place_limb_pitched(&s->pose[STICK_LEG_L_LO], -0.09f,yHip - p->leg_upper_len, -0.06f, -swing*0.4f, p->leg_lower_len, p->limb_radius);
    place_limb_pitched(&s->pose[STICK_LEG_R_LO], 0.09f,yHip - p->leg_upper_len, 0.06f, swing*0.4f, p->leg_lower_len, p->limb_radius);
}

void stickman_pose_mine(Stickman *s, float t){
    stickman_pose_idle(s,t);
    StickmanParams *p=&s->params;
    float swing = powf(sinf(t*7.0f)*0.5f+0.5f, 1.8f) * 1.7f; // 0..1.7 rad slam
    float yShoulder = s->pose[STICK_TORSO].model[13] + p->torso_half_h*0.85f;
    float pitch = -1.0f - swing; // overhead
    auto void place_both_arms(float pitch){
        float q[4]; mu_quat_axis_angle(q, (float[3]){1,0,0}, pitch);
        float qL[4], qR[4]; mu_quat_axis_angle(qL, (float[3]){0,0,1}, 0.25f); mu_quat_mul(qL,q,qL);
        mu_quat_axis_angle(qR, (float[3]){0,0,1}, -0.25f); mu_quat_mul(qR,q,qR);
        float dirL[3]={0,-1,0}, dirR[3]={0,-1,0}; float dL[3], dR[3];
        mu_quat_rotate(dL,qL,dirL); mu_quat_rotate(dR,qR,dirR);
        float midL[3]={-0.16f+dL[0]*p->arm_upper_len*0.5f, yShoulder+dL[1]*p->arm_upper_len*0.5f, dL[2]*p->arm_upper_len*0.5f};
        float midR[3]={ 0.16f+dR[0]*p->arm_upper_len*0.5f, yShoulder+dR[1]*p->arm_upper_len*0.5f, dR[2]*p->arm_upper_len*0.5f};
        float m[16];
        mu_mat4_trs(m, midL, qL, (float[3]){p->limb_radius, p->arm_upper_len*0.5f, p->limb_radius}); memcpy(s->pose[STICK_ARM_L_UP].model,m,sizeof m);
        mu_mat4_trs(m, midR, qR, (float[3]){p->limb_radius, p->arm_upper_len*0.5f, p->limb_radius}); memcpy(s->pose[STICK_ARM_R_UP].model,m,sizeof m);
        float elbowL[3]={-0.16f+dL[0]*p->arm_upper_len, yShoulder+dL[1]*p->arm_upper_len, dL[2]*p->arm_upper_len};
        float elbowR[3]={ 0.16f+dR[0]*p->arm_upper_len, yShoulder+dR[1]*p->arm_upper_len, dR[2]*p->arm_upper_len};
        float midLL[3]={elbowL[0]+dL[0]*p->arm_lower_len*0.5f, elbowL[1]+dL[1]*p->arm_lower_len*0.5f, elbowL[2]+dL[2]*p->arm_lower_len*0.5f};
        float midRL[3]={elbowR[0]+dR[0]*p->arm_lower_len*0.5f, elbowR[1]+dR[1]*p->arm_lower_len*0.5f, elbowR[2]+dR[2]*p->arm_lower_len*0.5f};
        mu_mat4_trs(m, midLL, qL, (float[3]){p->limb_radius*0.9f, p->arm_lower_len*0.5f, p->limb_radius*0.9f}); memcpy(s->pose[STICK_ARM_L_LO].model,m,sizeof m);
        mu_mat4_trs(m, midRL, qR, (float[3]){p->limb_radius*0.9f, p->arm_lower_len*0.5f, p->limb_radius*0.9f}); memcpy(s->pose[STICK_ARM_R_LO].model,m,sizeof m);
        if(s->has_pickaxe){
            float hand[3]={elbowR[0]+dR[0]*p->arm_lower_len, elbowR[1]+dR[1]*p->arm_lower_len, elbowR[2]+dR[2]*p->arm_lower_len};
            mu_mat4_trs(s->pose[STICK_PICK_HANDLE].model, (float[3]){hand[0],hand[1],hand[2]}, qR, (float[3]){1,1,1});
            mu_mat4_trs(s->pose[STICK_PICK_HEAD].model, (float[3]){hand[0]+dR[0]*0.35f, hand[1]+dR[1]*0.35f, hand[2]+dR[2]*0.35f}, qR, (float[3]){1,1,1});
        }
    }
    place_both_arms(pitch);
}

void stickman_free(Stickman *s){ demo_mesh_free(&s->mesh); memset(s,0,sizeof *s); }

