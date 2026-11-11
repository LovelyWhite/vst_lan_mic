#pragma once

// Single source of truth for the product version. Consumed by the firmware
// (info reply), the VST3 editor credit, vst3/CMakeLists.txt (project VERSION)
// and the packaging scripts (make_pkg.sh, make_installer.ps1), which parse
// the quoted string below with sed/Select-String -- keep the exact shape.
#define LANMIC_VERSION "v1.0.0"
