#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace etg::http {

// A deliberately small HTTP/1.1 server: fixed routes, no keep-alive, no request
// body, no chunking. It exists to expose what a consumer already knows over a
// browser, not to be a web framework, and anything beyond that belongs to a
// real server rather than to this project.
//
// Every response closes the connection. That is wasteful per request and
// completely irrelevant at a poll every 500ms, and it removes an entire class
// of connection-lifetime bugs from a component that is not the point.
struct Response {
  int status = 200;
  std::string content_type = "text/plain";
  std::string body;
};

using Handler = std::function<Response(const std::string& path)>;

class Server {
 public:
  static std::unique_ptr<Server> create(std::uint16_t port, std::string& error);

  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void start(Handler handler);
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::uint64_t requests() const noexcept {
    return requests_.load(std::memory_order_relaxed);
  }

 private:
  Server(std::uint16_t port, int listen_fd, int stop_fd);

  void run();
  void serve_one(int fd);

  std::uint16_t port_;
  int listen_fd_;
  int stop_fd_;
  Handler handler_;
  std::thread thread_;
  bool started_ = false;
  std::atomic<std::uint64_t> requests_{0};
};

}  // namespace etg::http
