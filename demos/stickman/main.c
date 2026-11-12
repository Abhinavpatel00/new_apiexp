
/* demos/stickman/main.c — minimal demo using stickman.h/c
 * Copy this folder to demos/stickman/ and add mu_add_demo(stickman demos/stickman/main.c demos/stickman/stickman.c) to CMakeLists.txt
 */
#include "demo.h"
#include "mesh.h"
#include "stickman.h"
#include "mu_math.h"
#include <string.h>

typedef struct App {
    Demo demo;
    Stickman man;
    BufSlice vtxSlice;
    BufUse   vtxUse;
    Target   colorTarget, depthTarget;
    Pipe     meshPipe;
    DemoOrbit orbit;
    bool mining;
    float timeScale;
} App;

// ---- shaders ----
// Reuses demos/common/shaders/mesh.slang (compiled to mesh.vert.spv / mesh.frag.spv)
// and fullscreen.slang for present. No new shaders needed.

static void app_init(App *a){
    Demo *d=&a->demo;

    StickmanParams p = stickman_default_params();
    p.col_body[0]=0.10f; p.col_body[1]=0.10f; p.col_body[2]=0.10f; // true stickman black
    stickman_build(&a->man, p, true);

    // upload vertices once (ARENA_DEVICE)
    a->vtxSlice = demo_alloc(d, a->man.mesh.vertices, (uint64_t)a->man.mesh.count * sizeof(DemoVertex));
    a->vtxUse   = use_read(a->vtxSlice.buffer, a->vtxSlice.offset, a->vtxSlice.size);

    // render targets
    a->colorTarget = demo_target(d, "color", FMT_RGBA16F, d->width, d->height, false, NULL);
    a->depthTarget = demo_target(d, "depth", FMT_D32F, d->width, d->height, false, NULL);

    // pipe: mesh shader
    ShaderId vs = demo_shader(d, "demos/common/shaders/mesh.vert.spv", SHADER_STAGE_VERT);
    ShaderId fs = demo_shader(d, "demos/common/shaders/mesh.frag.spv", SHADER_STAGE_FRAG);
    PipeKey key={0};
    key.vs=vs; key.fs=fs;
    key.color[0]=FMT_RGBA16F; key.color_count=1; key.depth=FMT_D32F;
    a->meshPipe = pipe_intern(d->gpu, &key);

    a->orbit.target[0]=0; a->orbit.target[1]=1.0f; a->orbit.target[2]=0;
    a->orbit.distance=4.0f; a->orbit.yaw=0.4f; a->orbit.pitch=0.15f;
    a->orbit.fov_deg=45; a->orbit.znear=0.1f; a->orbit.zfar=40;
    a->orbit.min_distance=1.5f; a->orbit.max_distance=12;

    // frame program: Shadow? omitted for brevity — single pass
    static PassDesc passes[2];
    passes[0]=(PassDesc){ .name="Scene", .pipe=a->meshPipe, .color[0]={a->colorTarget, ATT_CLEAR, {0.82f,0.80f,0.78f,1}}, .depth={a->depthTarget, ATT_DEPTH_CLEAR, {1,0}}, .reads={a->vtxUse} };
    passes[1]=demo_present_pass(d, a->colorTarget);
    d->program = demo_program(d, passes, 2);
}

// per-frame push: must match mu_mesh.slang MeshPush (see demos/common/shaders/mu_mesh.slang)
typedef struct MeshPush {
    mu_mat4 viewProj;
    mu_mat4 model;
    mu_vec4 color;
    mu_vec4 params;
    uint32_t vtxOffset;
    uint32_t pad[3];
} MeshPush;

static void app_frame(App *a){
    Demo *d=&a->demo;
    if(!demo_frame_begin(d)) return;
    demo_orbit_input(d, &a->orbit);
    float view[16], proj[16], vp[16], eye[3];
    demo_orbit_camera(&a->orbit, (float)d->width/(float)d->height, view, proj, vp, eye);

    // choose animation
    if(a->mining) stickman_pose_mine(&a->man, d->time * a->timeScale);
    else stickman_pose_walk(&a->man, d->time * a->timeScale, 1.0f);

    // record Scene pass
    PassCtx *p = demo_pass(d, "Scene");
    for(int i=0;i<STICK_PART_COUNT;i++){
        if(!a->man.has_pickaxe && i>=STICK_PICK_HANDLE) continue;
        MeshPush push={0};
        memcpy(push.viewProj, vp, sizeof vp);
        memcpy(push.model, a->man.pose[i].model, sizeof push.model);
        memcpy(push.color, a->man.pose[i].color, sizeof push.color);
        // params.x = roughness etc — leave 0
        push.vtxOffset = (uint32_t)(a->vtxSlice.offset + (uint64_t)a->man.range[i].first * sizeof(DemoVertex));
        pass_push(p, &push, sizeof push);
        pass_draw(p, a->man.range[i].count, 1, 0, 0);
    }

    demo_present(d);
    demo_frame_end(d);
}

int main(int argc, char **argv){
    static App app={0};
    app.timeScale=1.0f;
    static const DemoOption opts[]={
        {"--mine", .flag=&app.mining, .help="mining swing instead of walk"},
        {"--speed", .f32=&app.timeScale, .lo=0.1, .hi=3.0, .help="time scale"},
        {0}
    };
    demo_defaults(&app.demo, "stickman", "Stickman — procedural miner");
    demo_open(&app.demo, opts, argc, argv);
    app_init(&app);
    while(demo_running(&app.demo)){
        // UI
        if(app.demo.ui){
            struct nk_context *ctx=app.demo.ui;
            if(nk_begin(ctx,"Stickman", nk_rect(10,10,280,160), NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE)){
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_checkbox_label(ctx, "Mining", &app.mining);
                nk_slider_float(ctx, 0.1f, &app.timeScale, 3.0f, 0.1f);
                nk_label(ctx, app.mining?"Mine: overhead slam":"Walk: opposite arm/leg", NK_TEXT_LEFT);
            } nk_end(ctx);
        }
        app_frame(&app);
    }
    bool check = app.demo.check;
    int rc = demo_close(&app.demo);
    if(check){
        // acceptance: head should be visible etc.
        demo_check(&app.demo, "vertices", (double)app.man.mesh.count, "900 < v < 2000", app.man.mesh.count>800 && app.man.mesh.count<2000);
    }
    stickman_free(&app.man);
    return rc;
}
