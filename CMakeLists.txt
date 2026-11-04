cmake_minimum_required(VERSION 3.21)
project(helix-engine VERSION 0.1.0 LANGUAGES C CXX)
include(cmake/HelixIdeFolders.cmake)
include(CTest)

# 기존 Windows 프리셋의 진입점. C#은 csharp/Helix.sln에서 독립 빌드한다.
add_subdirectory(cpp)
helix_group_ide_targets("${CMAKE_CURRENT_SOURCE_DIR}")
