add_library(
	nano_transport
	common.hpp
	fwd.hpp
	transport_service.hpp
	transport_service.cpp
	tcp/fwd.hpp
	tcp/tcp_service.hpp
	tcp/tcp_service.cpp)

target_link_libraries(
	nano_transport
	nano_lib
	nano_secure
	Boost::asio)
