#pragma once

#include "Commands.h"

namespace ost {

// Interactive console menu, used when the tool is started without arguments
// from Explorer (it owns its console window). Every menu action runs the same
// code path as the CLI command, and the action log is echoed as it happens.
int RunInteractiveMenu(const Options& baseOptions);

} // namespace ost
