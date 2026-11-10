# ext_hello: 纯 C 编写的示例扩展, 只依赖 C ABI 头 momo_extension.h。
# 独立构建(扩展作者):
#   cmake -S . -B build -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build build
if(NOT COMMAND momo_add_extension)
  cmake_minimum_required(VERSION 3.24)
  project(ext_hello LANGUAGES C)
  find_package(MomoExtSdk 1 REQUIRED)
endif()
momo_add_extension(ext_hello src/ext_hello.c)
