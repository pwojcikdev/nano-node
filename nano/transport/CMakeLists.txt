add_library(
	transport
	transport_context.hpp
	transport_context.cpp)

target_link_libraries(
	transport
	nano_lib
	nano_secure
	Boost::asio)
