#pragma once
#include "../common/mesh.h" // demos/common/mesh.h
#include "mu_math.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum StickmanPart {
  STICK_HEAD=0, STICK_TORSO,
  STICK_ARM_L_UP, STICK_ARM_L_LO,
  STICK_ARM_R_UP, STICK_ARM_R_LO,
  STICK_LEG_L_UP, STICK_LEG_L_LO,
  STICK_LEG_R_UP, STICK_LEG_R_LO,
  STICK_PICK_HANDLE, STICK_PICK_HEAD,
  STICK_PART_COUNT
} StickmanPart;

typedef struct StickmanParams {
    float head_radius, head_y;
    float torso_radius, torso_half_h;
    float limb_radius;
    float arm_upper_len, arm_lower_len;
    float leg_upper_len, leg_lower_len;
    float col_head[4], col_body[4], col_pick[4];
} StickmanParams;

StickmanParams stickman_default_params(void);

typedef struct Stickman {
  DemoMesh   mesh;                 // one vertex buffer for all parts
  DemoRange  ranges[STICK_PART_COUNT]; // first/count per part
  DemoObject pose[STICK_PART_COUNT];  // per-frame model + color
  float height; // total, for camera framing
  StickmanParams params;
  bool has_pickaxe;
} Stickman;

bool stickman_build(Stickman *s, StickmanParams p, bool with_pickaxe);
void stickman_pose_idle(Stickman *s, float t);
void stickman_pose_walk(Stickman *s, float t, float speed);
void stickman_pose_mine(Stickman *s, float t);
void stickman_free(Stickman *s);
