add_library(
	nano_transport
	transport_context.hpp
	transport_context.cpp)

target_link_libraries(
	nano_transport
	nano_lib
	nano_secure
	Boost::asio)
