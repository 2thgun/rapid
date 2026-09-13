#include "rapid/pairing.hpp"
#include <iostream>

using namespace rapid::native;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    const std::string device(32, 'a'), certificate(64, 'b'), public_key(64, 'c');
    const auto code = PairingWindow::verification_code(device, certificate,
        std::string(32, 'd'), std::string(32, 'e'), public_key);
    require(code.size() == 8 && code.find_first_not_of("0123456789") == std::string::npos &&
                code == PairingWindow::verification_code(device, certificate,
                    std::string(32, 'd'), std::string(32, 'e'), public_key),
            "verification code is deterministic and eight numeric digits");
    PairingWindow window(device, certificate);
    require(!window.active(0), "pairing starts physically closed");
    window.open(10);
    auto pending = window.request("Driver PC", public_key, 11);
    require(window.pending(11)->code == pending.code && !window.approve(pending.transaction_id,
                "00000000", 11) && window.pending(11),
            "only the displayed transaction code approves");
    for (int i = 0; i < 4; ++i)
      require(!window.approve(pending.transaction_id, "00000000", 12 + i),
              "failed approval is bounded");
    require(!window.active(20), "five failures close physical pairing window");
    require(!window.pending(20), "five failures erase pending material");
    window.open(30);
    pending = window.request("Driver PC", public_key, 31);
    require(!window.approve(pending.transaction_id, pending.code, 152) && !window.active(152),
            "expired transaction closes pairing window");
    window.open(200);
    pending = window.request("Driver PC", public_key, 201);
    require(window.approve(pending.transaction_id, pending.code, 201),
            "matching physical approval is accepted");
    require(window.consume_approved(201)->transaction_id == pending.transaction_id &&
                !window.consume_approved(201),
            "approved transaction is consumed exactly once");
    window.cancel();
    require(!window.pending(201) && !window.active(201), "cancel clears pairing state");
    std::cout << "Pairing window: code, expiry, cancellation and failure limits passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
