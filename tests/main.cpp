// SPDX-License-Identifier: GPL-3.0-only
// The test runner: PDFium is started once for the whole run.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "core/engine.hpp"

int main(int argc, char** argv) {
  pdfz::Engine engine;
  doctest::Context ctx(argc, argv);
  return ctx.run();
}
