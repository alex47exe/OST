#include "include/Dialog.h"
#include "include/Encoding.h"

#include <windows.h>

namespace OSTPlatform::Dialog {

void ShowWarning(std::string title, std::string message) {
    // The text is UTF-8 and MessageBoxA decodes it with the system ANSI code page (GBK on
    // Chinese systems), so Chinese text would inevitably be garbled. Convert to UTF-16 and go
    // through MessageBoxW to display correctly in every locale.
    // The temporary c_str stays alive for the full expression and MessageBoxW consumes it
    // within that same expression, so it is safe.
    MessageBoxW(nullptr,
                Encoding::Utf8ToWide(message).c_str(),
                Encoding::Utf8ToWide(title).c_str(),
                MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

} // namespace OSTPlatform::Dialog
