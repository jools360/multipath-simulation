cmake_minimum_required(VERSION 3.19)
project(FilamentCone)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static runtime to match Filament's mt libraries
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Paths to dependencies (relative to project root)
set(FILAMENT_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Path to Filament installation")
set(SDL2_DIR "${CMAKE_SOURCE_DIR}/deps/SDL2-2.30.11" CACHE PATH "Path to SDL2 installation")
set(OPENCV_DIR "${CMAKE_SOURCE_DIR}/deps/opencv/build" CACHE PATH "Path to OpenCV installation")

# ImGui sources
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/deps/imgui")

# Add executable
add_executable(FilamentCone
    src/main.cpp
    src/ConeGenerator.cpp
    src/SceneManager.cpp
    src/WebcamCapture.cpp
    src/TrackingSystem.cpp
    src/Recording.cpp
    src/LensDistortion.cpp
    src/ImGuiPanel.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Include directories
target_include_directories(FilamentCone PRIVATE
    ${FILAMENT_DIR}/include
    ${SDL2_DIR}/include
    ${OPENCV_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/deps
    src
)

# Link directories - Filament uses x86_64 subfolder on Windows
target_link_directories(FilamentCone PRIVATE
    ${FILAMENT_DIR}/lib/x86_64/mt
    ${SDL2_DIR}/lib/x64
    ${OPENCV_DIR}/x64/vc16/lib
)

# Filament libraries - order matters for static linking
# Core Filament libraries
target_link_libraries(FilamentCone PRIVATE
    filament
    gltfio
    gltfio_core
    uberarchive
    filameshio
    backend
    bluegl
    bluevk
    shaders
    filabridge
    filaflat
    utils
    geometry
    smol-v
    ibl
    image
    ktxreader
    stb
    uberzlib
    dracodec
    meshoptimizer
    zstd
)

# SDL2
target_link_libraries(FilamentCone PRIVATE
    SDL2
    SDL2main
)

# OpenCV
target_link_libraries(FilamentCone PRIVATE
    opencv_world4120
)

# Windows system libraries
target_link_libraries(FilamentCone PRIVATE
    opengl32
    gdi32
    user32
    shlwapi
    dwmapi
    ws2_32
)

# Copy material files to output directory
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/red_material.filamat"
    "$<TARGET_FILE_DIR:FilamentCone>/red_material.filamat"
)
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/logo_material.filamat"
    "$<TARGET_FILE_DIR:FilamentCone>/logo_material.filamat"
)
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/scoreboard_material.filamat"
    "$<TARGET_FILE_DIR:FilamentCone>/scoreboard_material.filamat"
)
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/background.filamat"
    "$<TARGET_FILE_DIR:FilamentCone>/background.filamat"
)
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/shadow_plane.filamat"
    "$<TARGET_FILE_DIR:FilamentCone>/shadow_plane.filamat"
)
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/video_quad.filamat"
    "$<TARGET_FILE_DIR:FilamentCone>/video_quad.filamat"
)
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/lens_distortion.filamat"
    "$<TARGET_FILE_DIR:FilamentCone>/lens_distortion.filamat"
)
# Copy video file to output directory (skip if not present)
if(EXISTS "${CMAKE_SOURCE_DIR}/NFL_JT.mp4")
    add_custom_command(TARGET FilamentCone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/NFL_JT.mp4"
        "$<TARGET_FILE_DIR:FilamentCone>/NFL_JT.mp4"
    )
endif()

# Copy SDL2.dll to output directory
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SDL2_DIR}/lib/x64/SDL2.dll"
    "$<TARGET_FILE_DIR:FilamentCone>/SDL2.dll"
)

# Copy OpenCV DLL to output directory
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENCV_DIR}/x64/vc16/bin/opencv_world4120.dll"
    "$<TARGET_FILE_DIR:FilamentCone>/opencv_world4120.dll"
)

# Copy logo asset to output directory
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/AirPixel_logo.glb"
    "$<TARGET_FILE_DIR:FilamentCone>/AirPixel_logo.glb"
)

# Copy car model to output directory
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/Ford_Mustang_Mach-E_2021.glb"
    "$<TARGET_FILE_DIR:FilamentCone>/Ford_Mustang_Mach-E_2021.glb"
)



# Copy logo texture to output directory
add_custom_command(TARGET FilamentCone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "C:/Users/julia/Dropbox/AirPixel-White-01.png"
    "$<TARGET_FILE_DIR:FilamentCone>/AirPixel-White-01.png"
)

# ============================================================
# GhostCar executable
# ============================================================
add_executable(GhostCar
    src/ghostcar_main.cpp
    src/GhostCarApp.cpp
    src/VboParser.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(GhostCar PRIVATE
    ${FILAMENT_DIR}/include
    ${SDL2_DIR}/include
    ${OPENCV_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/deps
    src
)

target_link_directories(GhostCar PRIVATE
    ${FILAMENT_DIR}/lib/x86_64/mt
    ${SDL2_DIR}/lib/x64
    ${OPENCV_DIR}/x64/vc16/lib
)

target_link_libraries(GhostCar PRIVATE
    filament
    gltfio
    gltfio_core
    uberarchive
    filameshio
    backend
    bluegl
    bluevk
    shaders
    filabridge
    filaflat
    utils
    geometry
    smol-v
    ibl
    image
    ktxreader
    stb
    uberzlib
    dracodec
    meshoptimizer
    zstd
)

target_link_libraries(GhostCar PRIVATE
    SDL2
    SDL2main
)

target_link_libraries(GhostCar PRIVATE
    opencv_world4120
)

target_link_libraries(GhostCar PRIVATE
    opengl32
    gdi32
    user32
    shlwapi
    dwmapi
    ws2_32
    comdlg32
)

# Copy materials to GhostCar output directory
add_custom_command(TARGET GhostCar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/ghost_car.filamat"
    "$<TARGET_FILE_DIR:GhostCar>/ghost_car.filamat"
)
add_custom_command(TARGET GhostCar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/materials/background.filamat"
    "$<TARGET_FILE_DIR:GhostCar>/background.filamat"
)

# Copy funcup.glb to output directory
add_custom_command(TARGET GhostCar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/assets/funcup.glb"
    "$<TARGET_FILE_DIR:GhostCar>/funcup.glb"
)

# Copy SDL2.dll to output directory
add_custom_command(TARGET GhostCar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SDL2_DIR}/lib/x64/SDL2.dll"
    "$<TARGET_FILE_DIR:GhostCar>/SDL2.dll"
)

# Copy OpenCV DLL to output directory
add_custom_command(TARGET GhostCar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OPENCV_DIR}/x64/vc16/bin/opencv_world4120.dll"
    "$<TARGET_FILE_DIR:GhostCar>/opencv_world4120.dll"
)

# Copy StartFinishDataBase.xml to output directory
add_custom_command(TARGET GhostCar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/StartFinishDataBase.xml"
    "$<TARGET_FILE_DIR:GhostCar>/StartFinishDataBase.xml"
)
