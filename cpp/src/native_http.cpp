#include "rapid/native.hpp"
#include <array>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ssl.hpp>
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
          boost::system::error_code blocking_error;
          socket.non_blocking(false, blocking_error);
          if (blocking_error)
            throw boost::system::system_error(blocking_error);
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
            // Two viewer shapes share this path. The engineering telemetry
            // view (cpp/assets/telemetry.html) asks for a rolling batch of
            // raw per-sample events (?history=<seconds>) so it can chart
            // arbitrary channels. A display (Qt panel, browser dashboard,
            // ?mode=state) instead wants only the newest runtime snapshot --
            // the same JSON shape as GET /api/live -- redrawn at display
            // rate. Neither branch queues anything for a slow client: each
            // tick re-reads the current state and sends exactly one message,
            // so a client that falls behind simply skips the ticks it missed
            // instead of ever being handed a backlog of stale frames. Per-
            // client memory is therefore O(1) regardless of connection speed,
            // and a stalled write is bounded by the socket's SO_SNDTIMEO (set
            // above) rather than growing a queue.
            const bool state_mode = target.find("mode=state") != std::string::npos;
            std::uint64_t cursor = 0;
            int history = 30;
            auto pos = target.find("history=");
            if (pos != std::string::npos)
              try {
                history = std::stoi(target.substr(pos + 8));
              } catch (...) {
              }
            // ~30 Hz: fast enough that the push, not the transport, sets the
            // display's update rate, without redoing work the runtime can't
            // usefully produce faster than telemetry arrives.
            constexpr auto tick = std::chrono::milliseconds(33);
            while (!stopping) {
              {
                std::lock_guard client_lock(clients[i].mutex);
                clients[i].deadline = monotonic() + 2;
              }
              const std::string payload = state_mode
                  ? runtime->snapshot().dump()
                  : runtime->events(cursor, history).dump();
              ws.write(asio::buffer(payload));
              std::this_thread::sleep_for(tick);
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

void serve_tls(const std::string &host, int port, Handler handler,
               const fs::path &certificate, const fs::path &private_key) {
  asio::io_context context;
  asio::ssl::context tls(asio::ssl::context::tls_server);
  tls.set_options(asio::ssl::context::default_workarounds |
                  asio::ssl::context::no_sslv2 |
                  asio::ssl::context::no_sslv3 |
                  asio::ssl::context::no_tlsv1 |
                  asio::ssl::context::no_tlsv1_1);
  tls.use_certificate_chain_file(certificate.string());
  tls.use_private_key_file(private_key.string(), asio::ssl::context::pem);
  if (SSL_CTX_check_private_key(tls.native_handle()) != 1)
    throw std::runtime_error("TLS certificate does not match private key");

  tcp::acceptor acceptor(context);
  const tcp::endpoint endpoint{asio::ip::make_address(host),
                                static_cast<unsigned short>(port)};
  acceptor.open(endpoint.protocol());
  acceptor.bind(endpoint);
  acceptor.listen(tcp::acceptor::max_listen_connections);
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
          boost::system::error_code blocking_error;
          socket.non_blocking(false, blocking_error);
          if (blocking_error)
            throw boost::system::system_error(blocking_error);
          asio::ssl::stream<tcp::socket> stream(std::move(socket), tls);
          stream.handshake(asio::ssl::stream_base::server);
          beast::flat_buffer buffer;
          http::request_parser<http::string_body> parser;
          parser.body_limit(2 * 1024 * 1024);
          parser.header_limit(16384);
          http::read(stream, buffer, parser);
          auto req = parser.release();
          Request request{std::string(req.method_string()),
                          std::string(req.target()), req.body(), {}};
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
            log(std::string("ERROR HTTPS handler: ") + e.what());
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
          http::write(stream, response);
        } catch (const std::exception &) {
          // Failed handshakes and disconnected clients do not stop setup.
        }
      }
    });
  log("Native HTTPS listening on port " + std::to_string(port));
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
      log("ERROR HTTPS accept failed");
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
