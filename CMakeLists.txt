cmake_minimum_required(VERSION 3.16)
project(kinect_audio_recorder VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(RECORDER_BUILD_GUI "Build Dear ImGui/GLFW desktop recorder (downloads pinned sources)" OFF)
include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_library(recorder_core
    src/platform.cpp src/process.cpp src/audio.cpp src/depth.cpp src/encoding_queue.cpp src/wav_writer.cpp src/recorder.cpp)
target_include_directories(recorder_core PUBLIC include)
target_link_libraries(recorder_core PUBLIC Threads::Threads)
if(WIN32)
    target_sources(recorder_core PRIVATE src/wasapi_source.cpp)
    target_compile_definitions(recorder_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(recorder_core PRIVATE ole32 uuid)
else()
    target_sources(recorder_core PRIVATE src/audio_devices_stub.cpp)
    target_compile_definitions(recorder_core PRIVATE _FILE_OFFSET_BITS=64)
endif()
if(MSVC)
    target_compile_options(recorder_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(recorder_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(recording_tool src/cli.cpp)
target_link_libraries(recording_tool PRIVATE recorder_core)
if(WIN32)
    target_link_libraries(recording_tool PRIVATE shell32)
endif()

if(RECORDER_BUILD_GUI)
    include(FetchContent)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 70a9bb3881fe80fd483236e2b203cb451c6ecf40)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG f1cc2ae15e53a861a874c3034aae6798fde194ab)
    FetchContent_MakeAvailable(glfw imgui)
    find_package(OpenGL REQUIRED)
    add_library(recorder_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(recorder_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(recorder_imgui PUBLIC glfw OpenGL::GL ${CMAKE_DL_LIBS})
    add_executable(audio_recorder src/gui.cpp)
    target_link_libraries(audio_recorder PRIVATE recorder_core recorder_imgui)
endif()

if(BUILD_TESTING)
    add_executable(recorder_tests tests/core_tests.cpp)
    target_link_libraries(recorder_tests PRIVATE recorder_core)
    add_test(NAME recorder_core COMMAND recorder_tests)
    add_executable(recorder_encoding_tests tests/encoding_queue_tests.cpp)
    target_link_libraries(recorder_encoding_tests PRIVATE recorder_core)
    add_test(NAME recorder_encoding_queue COMMAND recorder_encoding_tests)
    set_tests_properties(recorder_encoding_queue PROPERTIES TIMEOUT 30)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME recorder_files COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:recording_tool>)
        add_test(NAME recorder_depth COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/depth_integration.py $<TARGET_FILE:recording_tool>)
        add_test(NAME recorder_ffmpeg_bundle COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bundled_ffmpeg.py $<TARGET_FILE:recording_tool>)
        set_tests_properties(recorder_ffmpeg_bundle PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME recorder_encoding_files COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/encoding_integration.py $<TARGET_FILE:recording_tool>)
        set_tests_properties(recorder_encoding_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
    endif()
endif()

install(TARGETS recording_tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE DESTINATION .)
if(RECORDER_BUILD_GUI)
    install(TARGETS audio_recorder RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${glfw_SOURCE_DIR}/LICENSE.md DESTINATION licenses RENAME GLFW.txt)
    install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME DearImGui.txt)
endif()
