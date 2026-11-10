# SDK 产物自检(CTest ExtensionSdk.StandaloneCxxBuild): 在全新目录中, 只凭 <build>/sdk 的头文件与包配置,
# 用 find_package(MomoExtSdk) 独立构建一个 C++ 扩展(texture 示例)。参数: SDK_DIR SOURCE_DIR WORK_DIR
# GENERATOR MAKE_PROGRAM CXX_COMPILER。
file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -S "${SOURCE_DIR}" -B "${WORK_DIR}" -G "${GENERATOR}"
          "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
          "-DMomoExtSdk_DIR=${SDK_DIR}/lib/cmake/MomoExtSdk"
  RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "Standalone configure against the SDK failed")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} --build "${WORK_DIR}" RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "Standalone build against the SDK failed")
endif()
