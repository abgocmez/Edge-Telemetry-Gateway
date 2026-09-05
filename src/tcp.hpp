#pragma once

#include <cstdint>
#include <string>

namespace etg::tcp {

// The gateway is the server and consumers connect in. That direction is chosen
// deliberately: consumers come and go, the gateway is the stable endpoint, and
// reconnect then needs no discovery mechanism on the gateway side at all.

// Returns a listening socket, or -1 with `error` set. Port 0 asks the kernel to
// choose a free port, which is what tests use so a busy port cannot make them
// flaky; read it back with local_port().
int listen_on(std::uint16_t port, std::string& error);

// The port a socket is actually bound to. Returns 0 on failure.
std::uint16_t local_port(int fd);

// Non-blocking. Returns the accepted socket, -1 if nothing is pending
// (`would_block` set true), or -1 with `error` set on a real failure.
int accept_nonblocking(int listen_fd, bool& would_block, std::string& error);

// Returns a connected socket, or -1 with `error` set.
int connect_to(const std::string& host, std::uint16_t port, std::string& error);

// TCP_NODELAY is not optional here. Nagle would coalesce small batches and add
// up to 40ms of invisible latency, which would be measured and attributed to
// the gateway. Batching is a decision this code makes explicitly; leaving it to
// the kernel would make the latency/throughput trade-off unmeasurable.
bool set_nodelay(int fd, std::string& error);

bool set_nonblocking(int fd, std::string& error);

}  // namespace etg::tcp
