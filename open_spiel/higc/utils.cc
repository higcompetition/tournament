// Copyright 2019 DeepMind Technologies Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "open_spiel/higc/utils.h"
#include <thread>

namespace open_spiel {
namespace higc {

void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int time_elapsed(const std::chrono::time_point<std::chrono::system_clock>& start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now() - start).count();
}

bool getline_async(std::istream& is, std::string& line_out, std::string& buf) {
  int chars_read = 0;
  bool line_read = false;
  line_out.clear();

  do {
    // Read a single character (non-blocking).
    char c;
    chars_read = is.readsome(&c, 1);

    if (chars_read == 1) {
      if (c == '\n') {
        line_out = buf;
        buf = "";
        line_read = true;
      } else {
        buf.append(1, c);
      }
    }
  } while (chars_read != 0 && !line_read);

  return line_read;
}

}  // namespace higc
}  // namespace open_spiel
