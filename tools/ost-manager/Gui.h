#pragma once

#include "Commands.h"

namespace ost {

// Win32 control-panel UI. Every button runs the same command as the CLI, and
// the log pane tails ost.log, so the window, the console and the file always
// show identical lines (including work done by the elevated instance).
int RunGui(const Options& options);

} // namespace ost
