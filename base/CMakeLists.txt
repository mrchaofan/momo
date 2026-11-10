# base/: 全项目的基础库(docs/12)。不依赖任何上层(platform/engine/embedder 都依赖它)。
# 内容: 词汇类型(Status/logging/time/geometry/Closure)、对象模型(Object/WeakPtr/ObjectPtr)、
#       线程与任务(TaskRunner/EventLoop/MessagePump/Thread/WorkerPool/task_util/Pipeline)、
#       原生循环适配(NativeLoop/NativeMessagePump)。

add_library(momo_base STATIC
  event_loop.cc
  message_pump.cc
  native_message_pump.cc
  object.cc
  task_runner.cc
  task_util.cc
  thread.cc
  worker_pool.cc)
target_link_libraries(momo_base PUBLIC momo_compile_options)
find_package(Threads REQUIRED)
target_link_libraries(momo_base PUBLIC Threads::Threads)
add_library(momo::base ALIAS momo_base)

if(MOMO_BUILD_TESTS)
  add_executable(momo_base_unittests
    blocking_queue_unittests.cc
    event_loop_unittests.cc
    matrix_unittests.cc
    native_message_pump_unittests.cc
    object_unittests.cc
    pipeline_unittests.cc
    task_util_unittests.cc
    worker_pool_unittests.cc)
  target_link_libraries(momo_base_unittests PRIVATE momo_base GTest::gtest_main)
  gtest_discover_tests(momo_base_unittests)
endif()
