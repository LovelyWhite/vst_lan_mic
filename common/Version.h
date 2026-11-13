#pragma once

// Single source of truth for the product version. Consumed by the firmware
// (info reply), the VST3 editor credit, vst3/CMakeLists.txt (project VERSION)
// and the packaging scripts (make_pkg.sh, make_installer.ps1), which parse
// the quoted string below with sed/Select-String. Keep it numeric with no "v"
// prefix: those parsers require a digit right after the quote, and the display
// sites (editor credit, manifest.json) add their own "v".
#define LANMIC_VERSION "1.0.0"
