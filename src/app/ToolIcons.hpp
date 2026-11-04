// SPDX-License-Identifier: GPL-3.0-only
// Icons for the markup tools, drawn here rather than shipped as files: they follow the theme's
// text color, so they suit light and dark modes.
#pragma once

#include "Markup.hpp"

#include <QIcon>

class QPalette;

QIcon toolIcon(MarkupTool tool, const QPalette& palette);
QIcon selectToolIcon(const QPalette& palette);  // an arrow pointer
QIcon handToolIcon(const QPalette& palette);    // an open hand
QIcon signIcon(const QPalette& palette);        // a pen over a signature
QIcon stampIcon(const QPalette& palette);       // a rubber stamp
// A square of `color` with a frame, for color buttons and menus; crossed out if `color` is
// invalid (no fill).
QIcon colorIcon(const QColor& color);
// The style buttons: a stroke (an outlined square), a fill (a filled one), text color (an "A"
// over a bar of it). Grayed out without a color.
QIcon strokeColorIcon(const QColor& color, const QPalette& palette);
QIcon fillColorIcon(const QColor& color, const QPalette& palette);
QIcon textColorIcon(const QColor& color, const QPalette& palette);
QIcon alignIcon(int align, const QPalette& palette);  // 0 left, 1 center, 2 right
