cmake_minimum_required(VERSION 3.15)
project(sa_view CXX)
set(CMAKE_CXX_STANDARD 17)

# librw with the D3D9 platform + its window skeleton (aap/librw, cloned under reVC/vendor).
set(SA_LIBRW_DIR "E:/openSA/reVC/vendor/librw" CACHE PATH "librw source tree")
set(LIBRW_PLATFORM D3D9 CACHE STRING "" FORCE)
set(LIBRW_TOOLS OFF CACHE BOOL "" FORCE)
set(LIBRW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${SA_LIBRW_DIR} librw_build)
add_subdirectory(${SA_LIBRW_DIR}/skeleton librw_skeleton_build)

add_executable(sa_view WIN32 main.cpp ../sa_port/src/ImgV2.cpp ${SA_LIBRW_DIR}/skeleton/camera.cpp)
target_include_directories(sa_view PRIVATE ../sa_port/src)
target_link_libraries(sa_view PRIVATE librw::skeleton librw::librw)

add_executable(sa_world WIN32 main_world.cpp
    ../sa_port/src/ImgV2.cpp ../sa_port/src/TextData.cpp ../sa_port/src/BinaryIpl.cpp
    ${SA_LIBRW_DIR}/skeleton/camera.cpp)
target_include_directories(sa_world PRIVATE ../sa_port/src)
target_link_libraries(sa_world PRIVATE librw::skeleton librw::librw)

add_executable(sa_stream WIN32 main_stream.cpp
    ../sa_port/src/ImgV2.cpp ../sa_port/src/TextData.cpp ../sa_port/src/BinaryIpl.cpp
    ../sa_port/src/StreamingWorld.cpp ../sa_port/src/Streamer.cpp ../sa_port/src/ColBounds.cpp ../sa_port/src/Timecyc.cpp ../sa_port/src/IfpAnim.cpp ../sa_port/src/WaterDat.cpp ../sa_port/src/ScriptVM.cpp ../sa_port/src/PopCycle.cpp ../sa_port/src/Handling.cpp ../sa_port/src/PathNodes.cpp ../sa_port/src/AudioBank.cpp
    ${SA_LIBRW_DIR}/skeleton/camera.cpp)
target_include_directories(sa_stream PRIVATE ../sa_port/src)
target_link_libraries(sa_stream PRIVATE librw::skeleton librw::librw winmm)
