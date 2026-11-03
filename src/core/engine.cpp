// SPDX-License-Identifier: GPL-3.0-only
#include "core/engine.hpp"

#include <atomic>
#include <cassert>

#include <fpdfview.h>

#ifndef PDFZ_PDFIUM_VERSION
#define PDFZ_PDFIUM_VERSION "unknown"
#endif

namespace pdfz {

namespace {
std::atomic<bool> g_running{false};
}

std::mutex& engineMutex() {
  static std::mutex m;
  return m;
}

Engine::Engine() {
  assert(!g_running && "only one pdfz::Engine at a time");
  std::lock_guard lock(engineMutex());
  FPDF_LIBRARY_CONFIG config{};
  config.version = 2;  // fields up to m_v8EmbedderSlot; no V8/XFA in this build
  config.m_pUserFontPaths = nullptr;  // PDFium's own system font lookup
  config.m_pIsolate = nullptr;
  config.m_v8EmbedderSlot = 0;
  FPDF_InitLibraryWithConfig(&config);
  g_running = true;
}

Engine::~Engine() {
  std::lock_guard lock(engineMutex());
  FPDF_DestroyLibrary();
  g_running = false;
}

bool Engine::running() { return g_running; }

std::string Engine::version() { return PDFZ_PDFIUM_VERSION; }

}  // namespace pdfz
