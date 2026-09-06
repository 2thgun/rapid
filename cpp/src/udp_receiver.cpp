#include "rapid/protocol.hpp"
#include "rapid/state.hpp"

#include <array>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rapid {
void receive_udp(unsigned short port, LiveState& state) {
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0) return;
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(port);
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { close(socket_fd); return; }
  for (;;) {
    std::array<char, 8192> bytes{}; sockaddr_in source{}; socklen_t length = sizeof(source);
    const auto count = recvfrom(socket_fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&source), &length);
    if (count <= 0) continue;
    const auto packet = parse_companion_packet(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
    if (!packet) { state.mark_invalid(); continue; }
    char host[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, &source.sin_addr, host, sizeof(host));
    state.update(packet->simulator, packet->state, host, packet->frame.value_or(TelemetryFrame{}));
  }
}
}  // namespace rapid
