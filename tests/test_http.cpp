#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>

#include "http.hpp"
#include "tcp.hpp"

using namespace etg;
using namespace std::chrono_literals;

namespace {

// A raw client, so the test exercises the server's own parsing rather than
// whatever a convenience wrapper would paper over. `chunk` sends the request in
// pieces of that size.
std::string request(std::uint16_t port, const std::string& raw, std::size_t chunk = 0) {
  std::string error;
  const int fd = tcp::connect_to("127.0.0.1", port, error);
  if (fd < 0) {
    return {};
  }

  if (chunk == 0 || chunk >= raw.size()) {
    const ssize_t w = ::write(fd, raw.data(), raw.size());
    static_cast<void>(w);
  } else {
    for (std::size_t off = 0; off < raw.size(); off += chunk) {
      const std::size_t n = std::min(chunk, raw.size() - off);
      const ssize_t w = ::write(fd, raw.data() + off, n);
      static_cast<void>(w);
      std::this_thread::sleep_for(2ms);
    }
  }

  std::string out;
  std::array<char, 4096> buf{};
  for (;;) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    out.append(buf.data(), static_cast<std::size_t>(n));
  }
  ::close(fd);
  return out;
}

}  // namespace

TEST_CASE("the server routes on the path and closes the connection", "[http]") {
  std::string error;
  auto server = http::Server::create(0, error);  // port 0: kernel picks
  REQUIRE(server != nullptr);
  REQUIRE(server->port() != 0);

  server->start([](const std::string& path) -> http::Response {
    if (path == "/stats.json") {
      return {200, "application/json", R"({"ok":true})"};
    }
    return {404, "text/plain", "not found"};
  });

  SECTION("a known route") {
    const std::string res = request(server->port(), "GET /stats.json HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE_FALSE(res.empty());
    CHECK(res.find("HTTP/1.1 200 OK") == 0);
    CHECK(res.find("Content-Type: application/json") != std::string::npos);
    CHECK(res.find("Content-Length: 11") != std::string::npos);
    CHECK(res.find(R"({"ok":true})") != std::string::npos);
  }

  SECTION("a query string does not change the route") {
    const std::string res = request(server->port(), "GET /stats.json?t=99 HTTP/1.1\r\n\r\n");
    CHECK(res.find("HTTP/1.1 200 OK") == 0);
  }

  SECTION("an unknown route") {
    const std::string res = request(server->port(), "GET /nope HTTP/1.1\r\n\r\n");
    CHECK(res.find("HTTP/1.1 404 Not Found") == 0);
  }

  SECTION("a malformed request line") {
    const std::string res = request(server->port(), "garbage\r\n\r\n");
    CHECK(res.find("HTTP/1.1 400 Bad Request") == 0);
  }

  SECTION("a request split across many reads") {
    // Same hazard as the frame parser: a request arriving in fragments must not
    // be treated as complete at the first read. Three bytes at a time puts the
    // header terminator across a boundary.
    const std::string res =
        request(server->port(), "GET /stats.json HTTP/1.1\r\nHost: x\r\n\r\n", 3);
    CHECK(res.find("HTTP/1.1 200 OK") == 0);
    CHECK(res.find(R"({"ok":true})") != std::string::npos);
  }

  server->stop();
}

TEST_CASE("the server serves repeated requests and counts them", "[http]") {
  std::string error;
  auto server = http::Server::create(0, error);
  REQUIRE(server != nullptr);
  server->start([](const std::string&) -> http::Response { return {200, "text/plain", "hi"}; });

  for (int i = 0; i < 5; ++i) {
    const std::string res = request(server->port(), "GET / HTTP/1.1\r\n\r\n");
    CHECK(res.find("HTTP/1.1 200 OK") == 0);
  }

  server->stop();
  CHECK(server->requests() == 5);
}

TEST_CASE("stop is prompt and idempotent, and the destructor stops a running server",
          "[http]") {
  std::string error;
  {
    auto server = http::Server::create(0, error);
    REQUIRE(server != nullptr);
    server->start([](const std::string&) -> http::Response { return {200, "text/plain", ""}; });
    std::this_thread::sleep_for(30ms);

    const auto begin = std::chrono::steady_clock::now();
    server->stop();
    CHECK(std::chrono::steady_clock::now() - begin < 200ms);
    server->stop();
  }  // no explicit stop the second time: the destructor must join

  auto server = http::Server::create(0, error);
  REQUIRE(server != nullptr);
  server->start([](const std::string&) -> http::Response { return {200, "text/plain", ""}; });
  std::this_thread::sleep_for(30ms);
}
