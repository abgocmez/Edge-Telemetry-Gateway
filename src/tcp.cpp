#include "tcp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace etg::tcp {

int listen_on(std::uint16_t port, std::string& error) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    error = std::string{"socket: "} + std::strerror(errno);
    return -1;
  }

  // Without this, restarting the gateway fails with EADDRINUSE for the duration
  // of TIME_WAIT, which turns every consumer-recovery experiment into a wait.
  const int on = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    error = std::string{"setsockopt(SO_REUSEADDR): "} + std::strerror(errno);
    ::close(fd);
    return -1;
  }

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    error = "bind port " + std::to_string(port) + ": " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 16) < 0) {
    error = std::string{"listen: "} + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  if (!set_nonblocking(fd, error)) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::uint16_t local_port(int fd) {
  struct sockaddr_in addr {};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
    return 0;
  }
  return ntohs(addr.sin_port);
}

int accept_nonblocking(int listen_fd, bool& would_block, std::string& error) {
  would_block = false;
  const int fd = ::accept(listen_fd, nullptr, nullptr);
  if (fd >= 0) {
    if (!set_nonblocking(fd, error) || !set_nodelay(fd, error)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    would_block = true;
    return -1;
  }
  error = std::string{"accept: "} + std::strerror(errno);
  return -1;
}

int connect_to(const std::string& host, std::uint16_t port, std::string& error) {
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
  if (rc != 0) {
    error = "getaddrinfo " + host + ": " + ::gai_strerror(rc);
    return -1;
  }

  int fd = -1;
  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);

  if (fd < 0) {
    error = "connect " + host + ":" + port_str + ": " + std::strerror(errno);
    return -1;
  }
  if (!set_nodelay(fd, error)) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool set_nodelay(int fd, std::string& error) {
  const int on = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    error = std::string{"setsockopt(TCP_NODELAY): "} + std::strerror(errno);
    return false;
  }
  return true;
}

bool set_nonblocking(int fd, std::string& error) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    error = std::string{"fcntl(O_NONBLOCK): "} + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace etg::tcp
