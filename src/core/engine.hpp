// SPDX-License-Identifier: GPL-3.0-only
// The PDF engine (PDFium) as a whole: starting it up, shutting it down, and the one lock that
// every call into it goes through.
#pragma once

#include <mutex>
#include <string>

namespace pdfz {

// Starts PDFium for the lifetime of this object. Make exactly one, before any Document, and
// keep it alive until every Document is gone (main() owns it in the app, the test runner in
// the tests).
class Engine {
public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  static bool running();
  // "158.0.8125.0": the PDFium build this was compiled against.
  static std::string version();
};

// PDFium isn't thread-safe: no two calls into it may run at the same time, even on different
// documents. Everything in pdfz takes this lock around its PDFium calls, so pdfz objects can be
// used from any thread (the app renders on a worker thread while the UI asks for page sizes).
std::mutex& engineMutex();

}  // namespace pdfz
