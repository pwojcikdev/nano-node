add_library(
  test_common
  chains.hpp
  chains.cpp
  common.hpp
  common.cpp
  ledger_context.hpp
  ledger_context.cpp
  make_store.hpp
  make_store.cpp
  network.hpp
  network.cpp
  random.hpp
  random.cpp
  rate_observer.cpp
  rate_observer.hpp
  system.hpp
  system.cpp
  telemetry.hpp
  telemetry.cpp
  testutil.hpp
  testutil.cpp
  transport/fwd.hpp
  transport/fake.hpp
  transport/fake.cpp
  transport/inproc.hpp
  transport/inproc.cpp
  transport/test_channel.hpp
  transport/test_channel.cpp)

target_link_libraries(test_common node gtest)

include_directories(${CMAKE_SOURCE_DIR}/submodules)
include_directories(${CMAKE_SOURCE_DIR}/submodules/gtest/googletest/include)
include_directories(${CMAKE_SOURCE_DIR}/submodules/lmdb)
