include_guard(GLOBAL)
include(FetchContent)
# Taskflow 4.1.0. Only the private header implementation is used; no public
# Helix header or installed consumer depends on Taskflow headers/targets.
FetchContent_Declare(helix_taskflow
	GIT_REPOSITORY https://github.com/taskflow/taskflow.git
	GIT_TAG 45366fe5bc4f2f8ec9aa590b40c504e296886865
	GIT_SHALLOW FALSE
	SOURCE_SUBDIR helix-no-upstream-build)
FetchContent_MakeAvailable(helix_taskflow)
