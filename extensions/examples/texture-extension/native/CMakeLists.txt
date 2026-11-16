# ext_texture: 外部纹理示例(C++ 的 Node 插件, 用到宿主能力表 momo_host.h)。产物: ../build/ext_texture.node。
# 独立构建(扩展作者):
#   cmake -S . -B out -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build out
if(NOT COMMAND momo_add_extension)
  cmake_minimum_required(VERSION 3.24)
  project(ext_texture LANGUAGES CXX)
  find_package(MomoExtSdk 1 REQUIRED)
endif()
momo_add_extension(ext_texture src/ext_texture.cc)
