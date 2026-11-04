cmake_minimum_required(VERSION 3.21)
project(helix-engine VERSION 0.1.0 LANGUAGES C CXX)
include(CTest)

# 기존 Windows 프리셋의 진입점. C#은 csharp/Helix.sln에서 독립 빌드한다.
add_subdirectory(cpp)
