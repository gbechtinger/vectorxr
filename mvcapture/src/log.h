#pragma once

namespace mvcapture {

// Appends a line to %LOCALAPPDATA%\VectorXR\mvcapture.log (the file is
// restarted when the process starts, and capped so a long session cannot fill
// the disk). Safe to call from any thread; never throws.
void Log(const char* format, ...);

} // namespace mvcapture
