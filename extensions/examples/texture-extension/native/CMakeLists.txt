# ext_texture: 纹理注入示例(C++, 使用 momo_extension.hpp;不链接引擎的任何符号, 只用 C ABI)。
# 独立构建(扩展作者):
#   cmake -S . -B build -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build build
if(NOT COMMAND momo_add_extension)
  cmake_minimum_required(VERSION 3.24)
  project(ext_texture LANGUAGES CXX)
  find_package(MomoExtSdk 1 REQUIRED)
endif()
momo_add_extension(ext_texture src/ext_texture.cc)
