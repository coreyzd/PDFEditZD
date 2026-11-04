// SPDX-License-Identifier: GPL-3.0-only
// pdfz-info: what the core library sees in a PDF, without the app.
//
//   pdfz-info file.pdf                          pages, sizes, metadata
//   pdfz-info file.pdf --render 1 out.bmp       render page 1 (1-based) at 96 DPI
//   pdfz-info file.pdf --render 1 out.bmp --dpi 300
//   pdfz-info file.pdf --password secret ...
#include "core/annotations.hpp"
#include "core/document.hpp"
#include "core/engine.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

void put16(std::ofstream& o, unsigned v) { o.put(char(v & 0xFF)); o.put(char((v >> 8) & 0xFF)); }
void put32(std::ofstream& o, unsigned v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }

// 32-bit top-down BMP: opens anywhere, needs no image library.
bool writeBmp(const char* path, const pdfz::Bitmap& b) {
  std::ofstream o(path, std::ios::binary);
  if (!o) return false;
  const unsigned dataSize = unsigned(b.stride) * unsigned(b.height);
  o.write("BM", 2);
  put32(o, 54 + dataSize);
  put32(o, 0);
  put32(o, 54);
  put32(o, 40);
  put32(o, unsigned(b.width));
  put32(o, unsigned(-b.height));  // negative height: rows top to bottom
  put16(o, 1);
  put16(o, 32);
  put32(o, 0);
  put32(o, dataSize);
  put32(o, 3780);
  put32(o, 3780);
  put32(o, 0);
  put32(o, 0);
  o.write(reinterpret_cast<const char*>(b.pixels.data()), std::streamsize(dataSize));
  return bool(o);
}

int usage() {
  std::fprintf(stderr, "usage: pdfz-info FILE.pdf [--password P] [--render PAGE OUT.bmp [--dpi N]]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const char* file = argv[1];
  std::string password;
  int renderPage = 0;
  const char* renderOut = nullptr;
  double dpi = 96;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--password") && i + 1 < argc) {
      password = argv[++i];
    } else if (!std::strcmp(argv[i], "--render") && i + 2 < argc) {
      renderPage = std::atoi(argv[++i]);
      renderOut = argv[++i];
    } else if (!std::strcmp(argv[i], "--dpi") && i + 1 < argc) {
      dpi = std::atof(argv[++i]);
    } else {
      return usage();
    }
  }

  pdfz::Engine engine;
  auto [doc, err] = pdfz::Document::open(file, password);
  if (!doc) {
    std::fprintf(stderr, "%s: can't open: %s\n", file, pdfz::describe(err));
    return 1;
  }

  if (renderOut) {
    if (renderPage < 1 || renderPage > doc->pageCount() || dpi <= 0) {
      std::fprintf(stderr, "page %d out of range (1-%d)\n", renderPage, doc->pageCount());
      return 1;
    }
    const double scale = dpi / 72.0;
    const pdfz::SizeF s = doc->pageSize(renderPage - 1);
    const pdfz::Bitmap bmp = doc->render(renderPage - 1, scale,
        {0, 0, int(std::ceil(s.width * scale)), int(std::ceil(s.height * scale))});
    if (bmp.empty() || !writeBmp(renderOut, bmp)) {
      std::fprintf(stderr, "render or write failed\n");
      return 1;
    }
    std::printf("%s: page %d, %dx%d px\n", renderOut, renderPage, bmp.width, bmp.height);
    return 0;
  }

  const int v = doc->fileVersion();
  std::printf("File:     %s\n", file);
  std::printf("PDF:      %d.%d\n", v / 10, v % 10);
  std::printf("PDFium:   %s\n", pdfz::Engine::version().c_str());
  for (const char* key : {"Title", "Author", "Subject", "Creator", "Producer"}) {
    const std::string val = doc->metadata(key);
    if (!val.empty()) std::printf("%-9s %s\n", (std::string(key) + ":").c_str(), val.c_str());
  }
  std::printf("Pages:    %d\n", doc->pageCount());
  for (int i = 0; i < doc->pageCount(); ++i) {
    const pdfz::SizeF s = doc->pageSize(i);
    std::printf("  %4d  %7.1f x %7.1f pt  (%.2f x %.2f in)\n", i + 1, s.width, s.height, s.width / 72,
                s.height / 72);
    for (const pdfz::Annotation& a : doc->annotations(i)) {
      std::string text = a.contents.substr(0, 60);
      for (char& c : text)
        if (c == '\n' || c == '\r') c = ' ';
      std::printf("        %-10s at %.0f,%.0f %.0fx%.0f  %s%s%s\n", pdfz::kindName(a.kind), a.rect.left, a.rect.top,
                  a.rect.width(), a.rect.height(), a.author.empty() ? "" : a.author.c_str(), a.author.empty() ? "" : ": ",
                  text.c_str());
    }
  }
  return 0;
}
