// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: yang_frontend_fuzz CORPUS_DIRECTORY\n";
    return 2;
  }
  std::size_t runs = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(argv[1])) {
    if (!entry.is_regular_file()) continue;
    std::ifstream input(entry.path(), std::ios::binary);
    std::vector<std::uint8_t> seed(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(seed.data(), seed.size());
    ++runs;
    for (std::size_t index = 0; index < seed.size(); ++index) {
      std::vector<std::uint8_t> mutation = seed;
      mutation[index] ^= static_cast<std::uint8_t>(0xa5U + index);
      LLVMFuzzerTestOneInput(mutation.data(), mutation.size());
      ++runs;
    }
  }
  std::cout << "completed " << runs << " deterministic fuzz smoke runs\n";
  return 0;
}
