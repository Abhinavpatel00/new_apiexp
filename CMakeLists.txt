cmake_minimum_required(VERSION 3.16)
project(mu_gfx_galaxy C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(external/glfw EXCLUDE_FROM_ALL)

set(MU_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/vulkan/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/volk
    ${CMAKE_CURRENT_SOURCE_DIR}/external/mu_lib/mu
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nuklear)

add_library(mu_gfx STATIC
    src/vk.c
    external/volk/volk.c
    external/mu_lib/mu/mu_id_pool.c)
target_include_directories(mu_gfx PUBLIC ${MU_INCLUDE_DIRS})
target_compile_options(mu_gfx PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable -fno-strict-aliasing)
target_link_libraries(mu_gfx PUBLIC ${CMAKE_DL_LIBS} m)

# Both demos share demos/common/ (window/WSI, the Nuklear backend and the
# shared Slang modules in demos/common/shaders/) and differ only in their scene
# code under demos/<name>/.
set(DEMO_COMMON_SOURCES
    demos/common/platform.c
    demos/common/nk_vk.c)

add_executable(galaxy
    demos/galaxy/main.c
    ${DEMO_COMMON_SOURCES})
target_include_directories(galaxy PRIVATE ${MU_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/demos/common)
target_link_libraries(galaxy PRIVATE mu_gfx glfw)
target_compile_options(galaxy PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable -fno-strict-aliasing)

add_executable(toon
    demos/toon/main.c
    demos/toon/scene.c
    ${DEMO_COMMON_SOURCES})
target_include_directories(toon PRIVATE ${MU_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/demos/common
    ${CMAKE_CURRENT_SOURCE_DIR}/demos/toon)
target_link_libraries(toon PRIVATE mu_gfx glfw)
target_compile_options(toon PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable -fno-strict-aliasing)

# ---- demos built on demos/common/demo.c --------------------------------------
# demo.c owns the window, device, UI overlay, frame loop, command line, headless
# capture and the --check plumbing; a demo adds its pass graph and shaders.
add_library(demo_common STATIC
    demos/common/demo.c
    demos/common/mesh.c
    demos/common/canvas.c
    demos/common/softmesh.c
    ${DEMO_COMMON_SOURCES})
target_include_directories(demo_common PUBLIC ${MU_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/demos/common)
target_link_libraries(demo_common PUBLIC mu_gfx glfw)
target_compile_options(demo_common PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable -fno-strict-aliasing)

function(mu_add_demo name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE demo_common)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing)
endfunction()

mu_add_demo(sdf demos/sdf/main.c)
mu_add_demo(post demos/post/main.c)
mu_add_demo(shadows demos/shadows/main.c)
mu_add_demo(particles demos/particles/main.c)
mu_add_demo(pbr demos/pbr/main.c)
mu_add_demo(deferred demos/deferred/main.c)
mu_add_demo(clustered demos/clustered/main.c)
mu_add_demo(sdfworld demos/sdfworld/main.c)
mu_add_demo(fluid demos/fluid/main.c)
mu_add_demo(jfa demos/jfa/main.c)
mu_add_demo(gpu_driven demos/gpu_driven/main.c)
mu_add_demo(oit demos/oit/main.c)
mu_add_demo(taa demos/taa/main.c)
mu_add_demo(clay demos/clay/main.c)
mu_add_demo(flightsim demos/flightsim/main.c demos/flightsim/flight.c demos/flightsim/world.c)
target_include_directories(flightsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/demos/flightsim
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mu_lib/mu)
mu_add_demo(bricks demos/bricks/main.c)
mu_add_demo(screenspace demos/screenspace/main.c)
mu_add_demo(ocean demos/ocean/main.c)
mu_add_demo(lines demos/lines/main.c)
mu_add_demo(blackhole demos/blackhole/main.c)
mu_add_demo(turing demos/turing/main.c)
mu_add_demo(schrodinger demos/schrodinger/main.c)
mu_add_demo(mpm demos/mpm/main.c)
mu_add_demo(pat demos/pat/main.c demos/pat/method.c)
mu_add_demo(snow demos/snow/main.c demos/snow/method.c)
mu_add_demo(xpbd demos/xpbd/main.c demos/xpbd/method.c)
mu_add_demo(projdyn demos/projdyn/main.c demos/projdyn/method.c)
mu_add_demo(seiler demos/seiler/main.c demos/seiler/method.c)
mu_add_demo(avbd demos/avbd/main.c demos/avbd/method.c)
mu_add_demo(absnewton demos/absnewton/main.c demos/absnewton/method.c)
mu_add_demo(waveparticles demos/waveparticles/main.c demos/waveparticles/method.c)
mu_add_demo(mergesplit demos/mergesplit/main.c demos/mergesplit/method.c)
mu_add_demo(ipbf demos/ipbf/main.c demos/ipbf/method.c)
mu_add_demo(sampleelim demos/sampleelim/main.c demos/sampleelim/method.c)
mu_add_demo(gogreen demos/gogreen/main.c demos/gogreen/method.c)
mu_add_demo(c2spline demos/c2spline/main.c demos/c2spline/method.c)
mu_add_demo(catmullrom demos/catmullrom/main.c demos/catmullrom/method.c)
mu_add_demo(polyroots demos/polyroots/main.c demos/polyroots/method.c)
mu_add_demo(pointlight demos/pointlight/main.c)
mu_add_demo(wildmesh demos/wildmesh/main.c demos/wildmesh/method.c)
mu_add_demo(softloco demos/softloco/main.c demos/softloco/method.c)
mu_add_demo(contact demos/contact/main.c demos/contact/method.c)
mu_add_demo(koopman demos/koopman/main.c demos/koopman/method.c)
# The network of demos/pat is 100 million multiply-adds a point.
set_source_files_properties(demos/pat/method.c demos/snow/method.c demos/xpbd/method.c demos/projdyn/method.c demos/seiler/method.c demos/avbd/method.c demos/absnewton/method.c demos/waveparticles/method.c demos/mergesplit/method.c demos/ipbf/method.c demos/sampleelim/method.c demos/gogreen/method.c demos/c2spline/method.c demos/catmullrom/method.c demos/polyroots/method.c demos/wildmesh/method.c demos/softloco/method.c demos/contact/method.c demos/koopman/method.c PROPERTIES COMPILE_OPTIONS "-O3")

# ---- shaders ---------------------------------------------------------------
# Every shader is Slang. scripts/build_shaders.sh owns the manifest (source,
# entry point, stage, output); the targets below only select a group. The
# SPIR-V is checked in, so a machine without slangc still builds and runs.
find_program(SLANGC slangc)
function(mu_add_shader_target target)
    if(SLANGC)
        add_custom_target(${target}
            COMMAND ${CMAKE_COMMAND} -E env "SLANGC=${SLANGC}"
                    bash scripts/build_shaders.sh ${ARGN}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Slang -> SPIR-V: ${ARGN}"
            VERBATIM)
    else()
        add_custom_target(${target}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "slangc not found; using checked-in SPIR-V (${ARGN})")
    endif()
endfunction()

mu_add_shader_target(galaxy_shaders common galaxy)
mu_add_shader_target(toon_shaders common toon)
mu_add_shader_target(shaders common toon galaxy slang-toon sdf post shadows particles pbr deferred clustered sdfworld fluid jfa gpu_driven oit taa clay flightsim bricks screenspace ocean lines blackhole turing schrodinger mpm pat snow pointlight)

# The two Slang toon packages under demos/ keep their own build.sh so they can
# be vendored on their own; these targets run it.
function(mu_add_slang_package_target target dir)
    if(SLANGC)
        add_custom_target(${target}
            COMMAND ${CMAKE_COMMAND} -E env "SLANGC=${SLANGC}" bash build.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/demos/${dir}
            COMMENT "Rebuilding ${dir} SPIR-V with ${SLANGC}"
            VERBATIM)
    else()
        add_custom_target(${target}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "slangc not found; using checked-in SPIR-V in demos/${dir}/spirv")
    endif()
endfunction()
mu_add_slang_package_target(slang_toon_shaders slang-toon)
mu_add_slang_package_target(slang_toon_inspired_shaders slang-toon-inspired)

# Reference host for the bindless Slang toon stages (ABI v2): the demos/toon
# character scene through shadow-map, forward and outline passes. Windowed, or
# --headless to a PPM. It reuses demos/toon/scene.c for the geometry.
add_executable(slang_toon_host
    demos/slang-toon/host.c
    demos/toon/scene.c
    demos/common/platform.c)
target_include_directories(slang_toon_host PRIVATE ${MU_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/demos/common
    ${CMAKE_CURRENT_SOURCE_DIR}/demos/toon)
target_link_libraries(slang_toon_host PRIVATE mu_gfx glfw)
target_compile_options(slang_toon_host PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable -fno-strict-aliasing)

add_executable(smoke tests/smoke.c)
target_include_directories(smoke PRIVATE ${MU_INCLUDE_DIRS})
target_link_libraries(smoke PRIVATE mu_gfx)
target_compile_options(smoke PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable -fno-strict-aliasing)

add_executable(input_test tests/input.c)
target_include_directories(input_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/mu_lib/mu)
target_link_libraries(input_test PRIVATE m)
target_compile_options(input_test PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable)

add_executable(id_pool_test tests/id_pool.c external/mu_lib/mu/mu_id_pool.c)
target_include_directories(id_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/mu_lib/mu)
target_compile_options(id_pool_test PRIVATE -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-error=unused-but-set-variable)
