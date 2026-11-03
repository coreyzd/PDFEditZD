// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "core/document.hpp"

#include <QImage>

#include <vector>

// A pdfz bitmap as a QImage that owns its pixels. pdfz bitmaps are B,G,R,A bytes with alpha
// 255: QImage::Format_RGB32 on little-endian machines (every Windows and Linux PC this targets).
//
// The QImage takes over the buffer rather than pointing into `bmp`, so copies of it stay valid
// after `bmp` is gone. That matters: Qt's Windows print engine can hold on to the images drawn
// on a page until the page is finished.
inline QImage toQImage(pdfz::Bitmap&& bmp) {
  if (bmp.empty()) return {};
  auto* owned = new std::vector<std::uint8_t>(std::move(bmp.pixels));
  return QImage(owned->data(), bmp.width, bmp.height, bmp.stride, QImage::Format_RGB32,
                [](void* p) { delete static_cast<std::vector<std::uint8_t>*>(p); }, owned);
}
