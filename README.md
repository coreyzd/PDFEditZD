# PDFEditZD

A lightweight PDF viewer and editor for everyday documents, made to replace Adobe Acrobat for
basic work. It's one program and a few DLLs: no background services, no updater, no tray
icon, nothing installed or registered.

Built with C++20, Qt 6 Widgets and [PDFium](https://pdfium.googlesource.com/pdfium/) (the PDF
engine in Chrome). Windows comes first, through MSYS2. It builds on Linux too.

Status: **phase 0 (foundations)**.

## What works

| Area | Status |
| --- | --- |
| Viewing | Continuous scrolling, rendered in tiles on a worker thread, with a quick preview while tiles arrive. Fit width, fit page, actual size, zoom from 8% to 6400% (Ctrl+wheel zooms at the cursor). Drag to pan. |
| Navigation | Page thumbnails (F4), page number box, Home/End, Left/Right, Space/Shift+Space |
| Files | Open (and drag and drop), password-protected files, Open Recent, Save, Save As. Saves go to a temporary file first, then replace the original. |
| Pages | Insert a blank page, insert the pages of another PDF, rotate, delete (selected thumbnails, or the current page) |
| Printing | System print dialog, with page ranges and the current page. Shrinks to fit and auto-rotates landscape pages. Rendered at the printer's resolution. |

There's no undo yet, so deleting pages asks first.

## Building on Windows (MSYS2 UCRT64)

In the **MSYS2 UCRT64** shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base
cd /c/Users/corey.holt/Projects/PDFEditZD
cmake -S . -B build-win -G Ninja
cmake --build build-win
ctest --test-dir build-win --output-on-failure
./build-win/bin/pdfeditzd.exe
```

The first configure downloads a pinned PDFium build (about 4 MB, SHA-256 checked) into the
build folder. If you're offline, download `pdfium-win-x64.tgz` from
[pdfium-binaries](https://github.com/bblanchon/pdfium-binaries/releases), unpack it, and pass
`-DPDFZ_PDFIUM_DIR=C:/path/to/it`.

That build runs from the UCRT64 shell, which puts Qt's DLLs on the PATH. To get a folder that
runs from Explorer or on a PC without MSYS2:

```sh
cmake --build build-win --target deploy
```

It makes `build-win/deploy/PDFEditZD/`, holding `pdfeditzd.exe`, `pdfium.dll`, the Qt and
MinGW DLLs, Qt's plugins and the licenses. Copy or zip that folder as it is. To uninstall,
delete it. Settings are kept in `%APPDATA%\PDFEditZD\PDFEditZD.ini`, and Qt's warnings go to
`pdfeditzd.log` beside it (started fresh each run), which is the place to look when something
goes wrong.

## Building on Linux

```sh
# Fedora / Nobara
sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel
# Ubuntu / Debian
sudo apt install cmake ninja-build g++ qt6-base-dev

cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/pdfeditzd some.pdf
```

The core library, `pdfz-info` and the tests need only a C++20 compiler, so they build without
Qt (`-DPDFZ_BUILD_APP=OFF`).

## pdfz-info

A command-line view of what the core library sees, handy for checking a problem file:

```sh
pdfz-info file.pdf                          # version, metadata, page sizes
pdfz-info file.pdf --render 3 page3.bmp --dpi 150
pdfz-info file.pdf --password secret
```

## Layout

```
src/core          pdfz: PDFium behind a small thread-safe C++ API, with no Qt
                  (Engine, Document: open, render, page edits, save)
src/app           the Qt 6 app: MainWindow, DocumentView (tiled continuous view),
                  PageRenderer (worker thread + image caches), ThumbnailPanel, Printing
src/tools         pdfz-info
tests             doctest unit tests for src/core
cmake             PDFium download/import, Windows deploy script
third_party       doctest (MIT)
```

PDFium isn't thread-safe. Every call into it goes through `pdfz::engineMutex()`, so the UI
thread can lay out pages while the worker thread renders.

## Roadmap

1. **Phase 1, basic editing:** undo/redo for page edits. Drag to reorder thumbnails. Text
   selection, copy and search. Markups and comments (highlight, underline, strikeout, sticky
   note, free text, ink, shapes) with a comments panel. Tabs for several open documents.
2. **Phase 2:** Fill & Sign (AcroForm filling, plus a visual signature from a drawn or typed
   image). Stamps. Extract and split pages.
3. **Phase 3:** export to Word and Excel. This is a layout-analysis problem: rebuilding
   paragraphs and tables from positioned text. It will be a separate module, and good results
   are expected on simple documents.

Out of scope: requesting e-signatures, Protect, Scan & OCR.

## License

GPL-3.0-only (see `LICENSE`). PDFium is BSD-3-Clause and Apache-2.0 (the pdfium-binaries
packaging is MIT). Its license and those of its dependencies are shipped in
`licenses/pdfium/` of a deployed folder. doctest is MIT.
