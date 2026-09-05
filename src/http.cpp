#include "http.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

#include "tcp.hpp"

namespace etg::http {
namespace {

constexpr int kPollTimeoutMs = 100;
constexpr int kClientTimeoutMs = 2000;
constexpr std::size_t kMaxRequest = 8192;

const char* reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    default:  return "Error";
  }
}

// Only the path is needed. Method, headers and body are read and discarded:
// this server has no route that varies on any of them, and pretending
// otherwise would be parsing for its own sake.
bool request_path(const std::string& request, std::string& path) {
  const std::size_t sp1 = request.find(' ');
  if (sp1 == std::string::npos) {
    return false;
  }
  const std::size_t sp2 = request.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) {
    return false;
  }
  path = request.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::size_t query = path.find('?');
  if (query != std::string::npos) {
    path.resize(query);
  }
  return !path.empty();
}

}  // namespace

std::unique_ptr<Server> Server::create(std::uint16_t port, std::string& error) {
  const int listen_fd = tcp::listen_on(port, error);
  if (listen_fd < 0) {
    return nullptr;
  }
  const int stop_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stop_fd < 0) {
    error = std::string{"eventfd: "} + std::strerror(errno);
    ::close(listen_fd);
    return nullptr;
  }
  const std::uint16_t bound = tcp::local_port(listen_fd);
  return std::unique_ptr<Server>{new Server(bound != 0 ? bound : port, listen_fd, stop_fd)};
}

Server::Server(std::uint16_t port, int listen_fd, int stop_fd)
    : port_(port), listen_fd_(listen_fd), stop_fd_(stop_fd) {}

Server::~Server() {
  stop();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
  }
  if (stop_fd_ >= 0) {
    ::close(stop_fd_);
  }
}

void Server::start(Handler handler) {
  if (started_) {
    return;
  }
  handler_ = std::move(handler);
  started_ = true;
  thread_ = std::thread([this] { run(); });
}

void Server::stop() {
  if (!started_) {
    return;
  }
  const std::uint64_t one = 1;
  const ssize_t written = ::write(stop_fd_, &one, sizeof(one));
  static_cast<void>(written);
  if (thread_.joinable()) {
    thread_.join();
  }
  started_ = false;
}

void Server::run() {
  for (;;) {
    std::array<struct pollfd, 2> pfds{};
    pfds[0].fd = listen_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = stop_fd_;
    pfds[1].events = POLLIN;

    const int rc = ::poll(pfds.data(), pfds.size(), kPollTimeoutMs);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (rc > 0 && (pfds[1].revents & POLLIN) != 0) {
      return;
    }
    if (rc > 0 && (pfds[0].revents & POLLIN) != 0) {
      bool would_block = false;
      std::string error;
      const int fd = tcp::accept_nonblocking(listen_fd_, would_block, error);
      if (fd >= 0) {
        serve_one(fd);
        ::close(fd);
      }
    }
  }
}

void Server::serve_one(int fd) {
  std::string request;
  request.reserve(1024);

  // Read until the end of the headers. A browser sends this in one segment
  // almost always, but "almost always" is the same assumption that breaks the
  // frame parser under load, so it is looped here too.
  for (;;) {
    if (request.find("\r\n\r\n") != std::string::npos) {
      break;
    }
    if (request.size() > kMaxRequest) {
      const std::string body = "request too large";
      const std::string out = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      static_cast<void>(::send(fd, out.data(), out.size(), MSG_NOSIGNAL));
      return;
    }

    std::array<char, 2048> buf{};
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
    if (n > 0) {
      request.append(buf.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      return;  // client went away mid-request
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return;
    }

    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, kClientTimeoutMs) <= 0) {
      return;
    }
  }

  std::string path;
  Response res;
  if (!request_path(request, path)) {
    res.status = 400;
    res.body = "malformed request";
  } else {
    res = handler_(path);
  }

  requests_.fetch_add(1, std::memory_order_relaxed);

  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) +
                    "\r\nContent-Type: " + res.content_type +
                    "\r\nContent-Length: " + std::to_string(res.body.size()) +
                    "\r\nCache-Control: no-store"
                    "\r\nConnection: close\r\n\r\n" +
                    res.body;

  std::size_t sent = 0;
  while (sent < out.size()) {
    const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd {};
      pfd.fd = fd;
      pfd.events = POLLOUT;
      if (::poll(&pfd, 1, kClientTimeoutMs) <= 0) {
        return;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

}  // namespace etg::http
