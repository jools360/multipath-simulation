cmake_minimum_required(VERSION 3.19)
project(DistortionPreview)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(OPENCV_DIR "${CMAKE_SOURCE_DIR}/../deps/opencv/build" CACHE PATH "Path to OpenCV")

add_executable(distortion_preview distortion_preview.cpp)

target_include_directories(distortion_preview PRIVATE
    ${OPENCV_DIR}/include
    ${CMAKE_SOURCE_DIR}/../deps
)

target_link_directories(distortion_preview PRIVATE
    ${OPENCV_DIR}/x64/vc16/lib
)

target_link_libraries(distortion_preview PRIVATE
    opencv_world4120
)

# Copy OpenCV DLL
add_custom_command(TARGET distortion_preview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENCV_DIR}/x64/vc16/bin/opencv_world4120.dll"
    "$<TARGET_FILE_DIR:distortion_preview>/opencv_world4120.dll"
)
