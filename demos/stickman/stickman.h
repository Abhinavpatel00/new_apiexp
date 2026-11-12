#pragma once
#include "../common/mesh.h" // demos/common/mesh.h
#include "../../third_party/mu_lib/mu_math.h"

typedef enum StickmanPart {
  STICK_HEAD=0, STICK_TORSO,
  STICK_ARM_L_UP, STICK_ARM_L_LO,
  STICK_ARM_R_UP, STICK_ARM_R_LO,
  STICK_LEG_L_UP, STICK_LEG_L_LO,
  STICK_LEG_R_UP, STICK_LEG_R_LO,
  STICK_PART_COUNT
} StickmanPart;

typedef struct Stickman {
  DemoMesh   mesh;                 // one vertex buffer for all parts
  DemoRange  ranges[STICK_PART_COUNT]; // first/count per part
  DemoObject pose[STICK_PART_COUNT];  // per-frame model + color
  float height; // total, for camera framing
} Stickman;

void stickman_build(Stickman *s);
void stickman_pose(Stickman *s, float time, bool mining); // updates s->pose[].model
void stickman_free(Stickman *s);
