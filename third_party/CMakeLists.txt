# third_party/: 预编译依赖的接入冒烟测试(依赖本体由 cmake/momo_prebuilt.cmake 导出)。
if(MOMO_BUILD_TESTS AND MOMO_HAVE_SKIA AND MOMO_HAVE_V8)
  add_executable(momo_thirdparty_smoketest smoke_test/thirdparty_smoketest.cc)
  target_link_libraries(momo_thirdparty_smoketest PRIVATE momo_base momo::skia momo::v8
                        GTest::gtest_main)
  momo_target_use_v8_runtime(momo_thirdparty_smoketest)
  gtest_discover_tests(momo_thirdparty_smoketest)
endif()
