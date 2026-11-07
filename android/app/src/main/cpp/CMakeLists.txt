cmake_minimum_required(VERSION 3.22)
project(aotgame C CXX)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(RT ${ROOT}/src/runtime)
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -fno-strict-aliasing -fwrapv")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED AOT_MODE)
  set(AOT_MODE transpiler)
endif()
if(AOT_MODE STREQUAL "triangle")
  # Vulkan bring-up self-test: shaders compiled with the NDK's glslc at build time
  file(GLOB GLSLC_CAND ${ANDROID_NDK}/shader-tools/*/glslc)
  list(GET GLSLC_CAND 0 GLSLC)
  set(TRI ${ROOT}/src/triangle)
  foreach(S vert frag)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tri.${S}.inc
      COMMAND ${GLSLC} -mfmt=c -o ${CMAKE_CURRENT_BINARY_DIR}/tri.${S}.inc ${TRI}/tri.${S}
      DEPENDS ${TRI}/tri.${S})
  endforeach()
  add_library(aotgame SHARED ${TRI}/tri_main.c ${TRI}/vk_triangle.c ${GLUE}/android_native_app_glue.c
    ${CMAKE_CURRENT_BINARY_DIR}/tri.vert.inc ${CMAKE_CURRENT_BINARY_DIR}/tri.frag.inc)
  target_include_directories(aotgame PRIVATE ${TRI} ${GLUE} ${CMAKE_CURRENT_BINARY_DIR})
  target_link_options(aotgame PRIVATE -Wl,-u,ANativeActivity_onCreate)
  target_link_libraries(aotgame android log vulkan)
elseif(AOT_MODE STREQUAL "transpiler")
  include(FetchContent)
  set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
  set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_X86_SUPPORT ON CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
  set(CAPSTONE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(capstone
    URL https://codeload.github.com/capstone-engine/capstone/zip/refs/tags/5.0.1
    URL_HASH SHA256=06B5DDFEB9900FB3FADAE1983FD61289D19E125C600E6CD0C54315A9168AB393
  )
  FetchContent_MakeAvailable(capstone)
  set_target_properties(capstone PROPERTIES VERSION "" SOVERSION "")

  add_library(aotgame SHARED android_main.c ${GLUE}/android_native_app_glue.c)
  target_include_directories(aotgame PRIVATE ${GLUE})
  target_link_options(aotgame PRIVATE -Wl,-u,ANativeActivity_onCreate)
  target_link_libraries(aotgame PRIVATE android log capstone)
else()
  file(GLOB GEN ${CMAKE_CURRENT_SOURCE_DIR}/gen/*.c)
  if(AOT_PREBUILT_GEN)   # game parts were compiled in parallel CI shards (static archives); only the small table is built here
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/gen/aot_tab.c)
  endif()
  set_source_files_properties(${GEN} PROPERTIES COMPILE_FLAGS "-O1")
  add_library(aotgame SHARED android_main.c ${GLUE}/android_native_app_glue.c ${RT}/runtime.c ${RT}/win_resolver.c ${RT}/shims.c ${RT}/win_compat.c ${RT}/win_file.c ${RT}/win_file_async.c ${RT}/win_audio.c ${RT}/win_d3d10.c ${RT}/win_sys.c ${RT}/win_x86.c ${RT}/win_x86_dll.c ${RT}/win_input.c ${RT}/win_gfx.c ${RT}/dxvk_proxy.c ${RT}/gfx_backend.c ${RT}/gfx_draw_batch.c ${GEN})
  add_library(aotreserve SHARED ${RT}/aot_reserve.c)   # LD_PRELOADed by wrap.sh to reserve the PE32 image range before ART starts
  target_include_directories(aotgame PRIVATE ${RT} ${GLUE})
  target_compile_definitions(aotgame PRIVATE AOT_ANDROID_APP=1)
  target_link_options(aotgame PRIVATE -Wl,-u,ANativeActivity_onCreate)
  target_link_libraries(aotgame PRIVATE android log vulkan OpenSLES m)
  if(AOT_PREBUILT_GEN)
    file(GLOB AOT_SHARDS ${AOT_PREBUILT_GEN}/*.a)
    target_link_libraries(aotgame PRIVATE -Wl,--whole-archive ${AOT_SHARDS} -Wl,--no-whole-archive)
  endif()

  option(AOT_ENABLE_DXBC_SPIRV "Build dxbc-spirv for Direct3D shader translation" ON)
  if(AOT_ENABLE_DXBC_SPIRV)
    include(FetchContent)
    FetchContent_Declare(dxbc_spirv
      URL https://codeload.github.com/doitsujin/dxbc-spirv/tar.gz/76236b827e95381aa1b37bccfb31642896c427f8
      URL_HASH SHA256=D6E6B876F6FCEDF7ADB82D924C4561EE06DA1628A7C6128A9B8F403FC9E70AD9
    )
    FetchContent_GetProperties(dxbc_spirv)
    if(NOT dxbc_spirv_POPULATED)
      FetchContent_Populate(dxbc_spirv)
    endif()

    FetchContent_Declare(spirv_headers
      URL https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/c8ad050fcb29e42a2f57d9f59e97488f465c436d
      URL_HASH SHA256=5B6F38B430F1C68C7395C570053403DA14EB4CECC7FCB4040E7A5248C2F63874
    )
    FetchContent_GetProperties(spirv_headers)
    if(NOT spirv_headers_POPULATED)
      FetchContent_Populate(spirv_headers)
    endif()
    set(DXBC_SPIRV_HEADERS_SUBMODULE "${dxbc_spirv_SOURCE_DIR}/submodules/spirv_headers")
    file(MAKE_DIRECTORY "${DXBC_SPIRV_HEADERS_SUBMODULE}")
    file(COPY "${spirv_headers_SOURCE_DIR}/include/"
      DESTINATION "${DXBC_SPIRV_HEADERS_SUBMODULE}/include")

    file(GLOB DXBC_SPIRV_SOURCES CONFIGURE_DEPENDS
      "${dxbc_spirv_SOURCE_DIR}/dxbc/*.cpp"
      "${dxbc_spirv_SOURCE_DIR}/ir/*.cpp"
      "${dxbc_spirv_SOURCE_DIR}/ir/passes/*.cpp"
      "${dxbc_spirv_SOURCE_DIR}/sm3/*.cpp"
      "${dxbc_spirv_SOURCE_DIR}/spirv/*.cpp"
      "${dxbc_spirv_SOURCE_DIR}/util/*.cpp"
    )
    add_library(aot_dxbc_spirv STATIC ${DXBC_SPIRV_SOURCES})
    set_target_properties(aot_dxbc_spirv PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(aot_dxbc_spirv PRIVATE
      DXBC_SPV_ENABLE_SM3
      DXBC_SPV_ENABLE_SM5
      DXBC_SPV_ENABLE_SPIRV
    )
    target_compile_options(aot_dxbc_spirv PRIVATE -fno-exceptions)
    target_include_directories(aot_dxbc_spirv PUBLIC "${dxbc_spirv_SOURCE_DIR}")
    target_sources(aotgame PRIVATE "${RT}/dxbc_translate.cpp")
    target_compile_definitions(aotgame PRIVATE AOT_HAS_DXBC_SPIRV=1)
    target_link_libraries(aotgame PRIVATE aot_dxbc_spirv)
  else()
    target_sources(aotgame PRIVATE "${RT}/dxbc_translate_stub.c")
  endif()
endif()
