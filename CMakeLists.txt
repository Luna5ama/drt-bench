cmake_minimum_required(VERSION 3.25)

project(drt-bench LANGUAGES CXX)

find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)

add_executable(drt-bench src/main.cpp)
target_compile_features(drt-bench PRIVATE cxx_std_20)
target_compile_definitions(drt-bench PRIVATE
  VK_USE_PLATFORM_WIN32_KHR
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE)
target_link_libraries(drt-bench PRIVATE Vulkan::Vulkan PNG::PNG WebP::webp unofficial::tinyexr::tinyexr)

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
          $<TARGET_FILE_DIR:drt-bench>/passthrough/main.glsl)
