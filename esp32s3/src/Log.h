#pragma once

// ---- Output control macros -------------------------------------------------
// Define LANMIC_ENABLE_SERIAL to enable all Serial output.
// Undefine or comment out for silent builds (e.g. field deployments).
// Every translation unit includes this header, so the toggle stays global.
// #define LANMIC_ENABLE_SERIAL

#ifdef LANMIC_ENABLE_SERIAL
  #define S_INIT()               Serial.begin(115200)
  #define S_PRINTF(...)          Serial.printf(__VA_ARGS__)
#else
  #define S_INIT()               ((void)0)
  #define S_PRINTF(...)          ((void)0)
#endif
