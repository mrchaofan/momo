# ext_hello: 纯 C 的 Node 插件(只用 Node-API)。产物: ../build/ext_hello.node。
# 独立构建(扩展作者):
#   cmake -S . -B out -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build out
if(NOT COMMAND momo_add_extension)
  cmake_minimum_required(VERSION 3.24)
  project(ext_hello LANGUAGES C)
  find_package(MomoExtSdk 1 REQUIRED)
endif()
momo_add_extension(ext_hello src/ext_hello.c)
