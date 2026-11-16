# ext_lifecycle: 只供测试(extensions/examples/examples_unittests.cc)——把宿主对插件的每次调用记进文件,
# 用来验证 momo_host.h 写下的关闭顺序。产物: ../build/ext_lifecycle.node。
momo_add_extension(ext_lifecycle src/ext_lifecycle.c)
