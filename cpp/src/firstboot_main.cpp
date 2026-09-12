#include "rapid/setup.hpp"
#include <iostream>

using namespace rapid::native;

int main(int argc, char **argv) {
  try {
    fs::path directory, status_file;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-firstboot --state-directory PATH [--status-file PATH]\n"
                     "Initializes private device state and reports provisioning requirements.\n";
        return 0;
      }
      if (option == "--state-directory" && i + 1 < argc) directory = argv[++i];
      else if (option == "--status-file" && i + 1 < argc) status_file = argv[++i];
      else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    SetupStore store(directory);
    const auto status = provisioning_status(store.snapshot(), !store.owner_hash().empty());
    const auto text = status.dump() + "\n";
    if (!status_file.empty()) atomic_file(status_file, text);
    std::cout << text;
    log("INFO firstboot: provisioning state is " + status.at("state").get<std::string>() +
        "; secure AP bootstrap is not implemented");
    return 0;
  } catch (const std::exception &error) {
    log(std::string("ERROR firstboot: ") + error.what());
    return 1;
  }
}
