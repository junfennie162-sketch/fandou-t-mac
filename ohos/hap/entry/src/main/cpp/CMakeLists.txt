# LUT-SA HAP native library.
# Builds the T-MAC LUT kernel into the HAP for both targets:
#   x86_64  -> deploy/tuned              (AVX2 kernels)   <- DevEco emulator on Windows
#   arm64   -> deploy/tuned/aarch64-hf-bitnet-3b (NEON)  <- real HarmonyOS device
cmake_minimum_required(VERSION 3.16)
project(tmac_hap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root: ohos/hap/entry/src/main/cpp -> up 6 levels to fandou-t-mac-main
get_filename_component(T_MAC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../" ABSOLUTE)
if(NOT EXISTS "${T_MAC_ROOT}/deploy/tuned/kernels.cc")
  message(FATAL_ERROR "T-MAC repo root not found (looked at ${T_MAC_ROOT}). Adjust T_MAC_ROOT above.")
endif()

if(OHOS_ARCH STREQUAL "x86_64")
  set(TMAC_KERNEL_DIR "${T_MAC_ROOT}/deploy/tuned")
  set(TMAC_ARCH_FLAGS -mavx2 -mf16c -mfma)
  message(STATUS "T-MAC HAP: x86_64 (AVX2) kernels <- deploy/tuned")
else()
  set(TMAC_KERNEL_DIR "${T_MAC_ROOT}/deploy/tuned/aarch64-hf-bitnet-3b")
  set(TMAC_ARCH_FLAGS -march=armv8.2a+dotprod+fp16)
  message(STATUS "T-MAC HAP: ${OHOS_ARCH} (NEON) kernels <- aarch64-hf-bitnet-3b")
endif()

# kernels.cc includes "t-mac/kernels.h" — stage the arch-specific header the same way
# the top-level ohos/CMakeLists.txt does.
set(TMAC_GEN_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated_include")
file(MAKE_DIRECTORY "${TMAC_GEN_INCLUDE}/t-mac")
configure_file(
  "${TMAC_KERNEL_DIR}/kernels.h"
  "${TMAC_GEN_INCLUDE}/t-mac/kernels.h"
  COPYONLY
)

add_library(tmac_hap SHARED
  napi_init.cpp
  "${TMAC_KERNEL_DIR}/kernels.cc"
)

target_include_directories(tmac_hap PRIVATE
  "${T_MAC_ROOT}/ohos/shim"   # dmlc/logging.h shim wins over any TVM header
  "${TMAC_GEN_INCLUDE}"       # t-mac/kernels.h (arch-specific)
  "${T_MAC_ROOT}/include"
)

target_compile_options(tmac_hap PRIVATE ${TMAC_ARCH_FLAGS} -O2)
target_compile_definitions(tmac_hap PRIVATE NDEBUG)

target_link_libraries(tmac_hap PRIVATE libace_napi.z.so)
