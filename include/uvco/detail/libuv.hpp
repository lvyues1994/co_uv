#pragma once
// Single internal libuv boundary. Set Windows guards before libuv brings in
// platform headers; numeric_limits calls also use macro-resistant spelling
// for consumers that already included windows.h without NOMINMAX.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <uv.h>
