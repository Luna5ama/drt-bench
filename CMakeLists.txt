cmake_minimum_required(VERSION 3.25)

project(drt-bench LANGUAGES CXX)

find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)

add_executable(drt-bench src/main.cpp src/gpu_cube.cpp)
target_compile_features(drt-bench PRIVATE cxx_std_20)
target_compile_definitions(drt-bench PRIVATE
  VK_USE_PLATFORM_WIN32_KHR
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE)
target_link_libraries(drt-bench PRIVATE Vulkan::Vulkan PNG::PNG WebP::webp unofficial::tinyexr::tinyexr)

find_program(GLSLC_EXECUTABLE NAMES glslc
  HINTS "$ENV{VULKAN_SDK}/Bin"
  REQUIRED)
set(CUBE_SHADER_OUTPUTS)
foreach(CUBE_SHADER cube_peak.comp cube_render.comp)
  set(CUBE_SHADER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cube/${CUBE_SHADER}.spv")
  add_custom_command(
    OUTPUT "${CUBE_SHADER_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/cube"
    COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2
            -fshader-stage=compute -o "${CUBE_SHADER_OUTPUT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube/${CUBE_SHADER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube/${CUBE_SHADER}"
    VERBATIM)
  list(APPEND CUBE_SHADER_OUTPUTS "${CUBE_SHADER_OUTPUT}")
endforeach()
add_custom_target(drt-bench-cube-shaders DEPENDS ${CUBE_SHADER_OUTPUTS})
add_dependencies(drt-bench drt-bench-cube-shaders)

add_executable(opendrt-convert src/opendrt_convert.cpp)
target_compile_features(opendrt-convert PRIVATE cxx_std_20)
target_compile_definitions(opendrt-convert PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(opendrt-convert PRIVATE glm::glm WebP::webp unofficial::tinyexr::tinyexr)

if(MSVC)
  target_compile_options(drt-bench PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(opendrt-convert PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_custom_command(TARGET drt-bench POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:drt-bench>/passthrough
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/shaders/passthrough/main.glsl
          $<TARGET_FILE_DIR:drt-bench>/passthrough/main.glsl
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_BINARY_DIR}/cube
          $<TARGET_FILE_DIR:drt-bench>/cube)
