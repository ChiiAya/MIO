#pragma once

#include "runtime/Runtime.h"

#include <iosfwd>

namespace mio {

void runConsole(Runtime& runtime, std::istream& input, std::ostream& output);

} // namespace mio
