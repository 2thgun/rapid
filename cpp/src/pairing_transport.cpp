#include "rapid/pairing.hpp"

namespace rapid::native {
namespace {
Response reply(int code, const Json &body) { return {code, body.dump()}; }

bool valid_transaction(const std::string &value) {
  return value.size() == 32 &&
      value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
} // namespace

PairingTransport::PairingTransport(PairingCoordinator &pairing,
                                   std::function<double()> clock)
    : pairing_(pairing), clock_(std::move(clock)) {}

bool PairingTransport::handles(const Request &request) {
  const auto path = request.target.substr(0, request.target.find('?'));
  return (path == "/api/v1/pairing/request" && request.method == "POST") ||
      (path == "/api/v1/pairing/result" && request.method == "GET");
}

Response PairingTransport::handle(const Request &request) {
  if (!handles(request)) return reply(404, {{"detail", "not found"}});
  if (request.body.size() > 1024) return reply(413, {{"detail", "request too large"}});
  const auto path = request.target.substr(0, request.target.find('?'));
  const auto time = clock_();
  if (path == "/api/v1/pairing/request") {
    if (request.headers.find("content-type") == request.headers.end() ||
        request.headers.at("content-type") != "application/json")
      return reply(415, {{"detail", "application/json required"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 2 || !body["label"].is_string() ||
        !body["companion_public_key"].is_string())
      return reply(400, {{"detail", "label and companion public key required"}});
    try {
      const auto pending = pairing_.request(body["label"].get<std::string>(),
                                            body["companion_public_key"].get<std::string>(), time);
      return reply(201, {{"transaction_id", pending.transaction_id},
                         {"nonce", pending.nonce}, {"expires_at", pending.expires_at}});
    } catch (const std::invalid_argument &error) {
      return reply(400, {{"detail", error.what()}});
    } catch (const std::exception &error) {
      return reply(409, {{"detail", error.what()}});
    }
  }
  constexpr std::string_view prefix = "/api/v1/pairing/result?transaction_id=";
  if (!request.target.starts_with(prefix))
    return reply(400, {{"detail", "transaction_id required"}});
  const auto transaction = request.target.substr(prefix.size());
  if (!valid_transaction(transaction))
    return reply(400, {{"detail", "invalid transaction_id"}});
  const auto completed = pairing_.consume(time, transaction);
  if (!completed) return reply(202, {{"approved", false}});
  return reply(200, {{"approved", true}, {"peer_id", completed->peer_id},
                     {"label", completed->label},
                     {"ephemeral_public_key", completed->envelope.ephemeral_public_key},
                     {"nonce", completed->envelope.nonce},
                     {"ciphertext", completed->envelope.ciphertext},
                     {"tag", completed->envelope.tag}});
}
} // namespace rapid::native
