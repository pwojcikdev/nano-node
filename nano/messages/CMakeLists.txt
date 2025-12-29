add_library(nano_messages messages.hpp messages.cpp)

target_link_libraries(nano_messages nano_lib Boost::endian)

target_compile_definitions(nano_messages PUBLIC -DQT_NO_KEYWORDS
                                                -DBOOST_ASIO_HAS_STD_ARRAY=1)
