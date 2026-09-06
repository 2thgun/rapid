#include "rapid/native.hpp"
#include <array>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <condition_variable>
#include <sys/socket.h>
#include <thread>

namespace rapid::native {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
void serve(const std::string &host, int port, Handler handler,
           Runtime *runtime) {
  asio::io_context context;
  tcp::acceptor acceptor(context, {asio::ip::make_address(host),
                                   static_cast<unsigned short>(port)});
  acceptor.non_blocking(true);
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<tcp::socket> queue;
  std::vector<std::thread> workers;
  struct Client {
    std::mutex mutex;
    int fd = -1;
    double deadline = 0;
  };
  std::array<Client, 8> clients;
  std::atomic_int viewers = 0;
  for (int i = 0; i < 8; ++i)
    workers.emplace_back([&, i] {
      while (!stopping) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(100),
                      [&] { return !queue.empty() || stopping.load(); });
        if (queue.empty())
          continue;
        tcp::socket socket = std::move(queue.front());
        queue.pop_front();
        lock.unlock();
        {
          std::lock_guard client_lock(clients[i].mutex);
          clients[i].fd = socket.native_handle();
          clients[i].deadline = monotonic() + 5;
        }
        struct Reset {
          Client &client;
          ~Reset() {
            std::lock_guard lock(client.mutex);
            client.fd = -1;
          }
        } reset{clients[i]};
        try {
          timeval timeout{2, 0};
          setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof timeout);
          setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof timeout);
          beast::flat_buffer buffer;
          http::request_parser<http::string_body> parser;
          parser.body_limit(2 * 1024 * 1024);
          parser.header_limit(16384);
          http::read(socket, buffer, parser);
          auto req = parser.release();
          auto target = std::string(req.target());
          if (runtime && beast::websocket::is_upgrade(req) &&
              target.substr(0, target.find('?')) == "/api/v1/live") {
            if (viewers.fetch_add(1) >= 4) {
              --viewers;
              continue;
            }
            struct Viewer {
              std::atomic_int &count;
              ~Viewer() { --count; }
            } viewer{viewers};
            beast::websocket::stream<tcp::socket> ws(std::move(socket));
            Reset websocket_reset{clients[i]};
            ws.read_message_max(4096);
            ws.accept(req);
            ws.text(true);
            std::uint64_t cursor = 0;
            int history = 30;
            auto pos = target.find("history=");
            if (pos != std::string::npos)
              try {
                history = std::stoi(target.substr(pos + 8));
              } catch (...) {
              }
            while (!stopping) {
              {
                std::lock_guard client_lock(clients[i].mutex);
                clients[i].deadline = monotonic() + 2;
              }
              auto events = runtime->events(cursor, history);
              ws.write(asio::buffer(events.dump()));
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
          }
          Request request{
              std::string(req.method_string()), target, req.body(), {}};
          for (const auto &field : req) {
            auto name = std::string(field.name_string());
            for (auto &c : name)
              c = std::tolower(static_cast<unsigned char>(c));
            request.headers[name] = std::string(field.value());
          }
          Response value;
          try {
            value = handler(request);
          } catch (const std::invalid_argument &) {
            value = {400, "{\"detail\":\"invalid request\"}"};
          } catch (const Json::exception &) {
            value = {400, "{\"detail\":\"invalid JSON request\"}"};
          } catch (const std::exception &e) {
            log(std::string("ERROR HTTP handler: ") + e.what());
            value = {500, "{\"detail\":\"operation failed\"}"};
          }
          http::response<http::string_body> response{http::status(value.status),
                                                     req.version()};
          response.set(http::field::server, "raPId-native");
          response.set(http::field::content_type, value.type);
          response.set(http::field::cache_control, "no-store, max-age=0");
          for (const auto &[name, v] : value.headers)
            response.set(name, v);
          response.keep_alive(false);
          response.body() = value.body;
          response.prepare_payload();
          http::write(socket, response);
        } catch (const std::exception &) { /* Disconnected or malformed clients
                                              do not interrupt recording. */
        }
      }
    });
  log("Native HTTP listening on port " + std::to_string(port));
  while (!stopping) {
    for (auto &client : clients) {
      std::lock_guard client_lock(client.mutex);
      if (client.fd >= 0 && monotonic() > client.deadline)
        shutdown(client.fd, SHUT_RDWR);
    }
    boost::system::error_code ec;
    tcp::socket socket(context);
    acceptor.accept(socket, ec);
    if (ec == asio::error::would_block || ec == asio::error::try_again) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (ec) {
      log("ERROR accept failed");
      stopping = true;
      break;
    }
    std::lock_guard lock(mutex);
    if (queue.size() < 16) {
      queue.push_back(std::move(socket));
      wake.notify_one();
    }
  }
  for (auto &client : clients) {
    std::lock_guard client_lock(client.mutex);
    if (client.fd >= 0)
      shutdown(client.fd, SHUT_RDWR);
  }
  wake.notify_all();
  for (auto &worker : workers)
    worker.join();
}
} // namespace rapid::native
