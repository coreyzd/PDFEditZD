# PDFEditZD

A lightweight PDF viewer and editor for everyday documents, made to replace Adobe Acrobat for
basic work. It's one program and a few DLLs: no background services, no updater, no tray
icon, nothing installed or registered.

Built with C++20, Qt 6 Widgets and [PDFium](https://pdfium.googlesource.com/pdfium/) (the PDF
engine in Chrome). Windows comes first, through MSYS2. It builds on Linux too.

Status: **phase 2 (Fill & Sign, stamps, extract and split)** done; phase 3 (export to Word and Excel) next.

## What works

| Area | Status |
| --- | --- |
| Viewing | Continuous scrolling, rendered in tiles on a worker thread, with a quick preview while tiles arrive. Zooming shows the page at its old sharpness until the new tiles are in, and an edit keeps showing the page as it was until it's redrawn, so nothing flashes blank. Fit width, fit page, actual size, zoom from 8% to 6400% (Ctrl+wheel zooms at the cursor). |
| Tabs | Each document opens in its own tab, with its own thumbnails, comments, undo and find results. Ctrl+Tab / Ctrl+Shift+Tab switch tabs and Ctrl+W closes one. Opening a file that's already open goes to its tab. While nothing is open, a welcome page lists recent files. |
| Tools | Select tool (V): click a comment to select it (and drag to move it), drag over text to select it, or drag anywhere else to move the page. Hand tool (H): dragging always moves the page. The middle button always moves the page. Esc steps back: out of a drag, then the selection, then to the Select tool. |
| Comments | A Comment Tools toolbar has Highlight, Underline, Strikethrough (drag over text, or select text first and use Ctrl+Shift+H/U/X), Sticky Note, Text Box, Pen, Rectangle, Ellipse, Line and Arrow. Selected comments can be moved, and rectangles, ellipses and text boxes resized by their corners. The toolbar's style controls (color, fill color or no fill, opacity, line width, and for text boxes font, size, text color and alignment) change the selected comment, or what the tool makes next; a fill can be added or taken away at any time. Arrowheads grow with the line width, as in Acrobat. Text boxes are typed in right on the page: click (or drag out a box) and type, and the box grows as the text does. Double-click one to type in it again; Esc or a click elsewhere finishes. Double-click any other comment to edit its text, press Delete to remove it, or right-click for a menu. Hovering shows a comment's text. Every change can be undone. |
| Stamps | The Stamp button on the Comment Tools toolbar has Acrobat's stamps (Approved, Reviewed, Received and Revised say who stamped and when; Draft, Final, Confidential, Void and the rest), and pictures of your own saved as stamps. Click to place one; it can be moved and resized (keeping its shape) like any comment. |
| Form fields | Fillable PDF forms: fields are tinted (Fill & Sign > Highlight Form Fields). With the Select tool, click a text field to type in it on the page, click check boxes and radio buttons to set them, and pick from drop-down and list boxes. Tab and Shift+Tab go from field to field (Space sets a check box or opens a list). Each change is one undo step, read-only fields stay as they are, and other readers show the values as filled in. |
| Fill & Sign | A Fill & Sign toolbar for forms without fields: Add Text (type on the form, no box), Checkmark, Cross, Dot and today's Date, placed with a click. Sign places your signature or initials: draw one with the mouse or a pen, type it in a handwriting font, or use a photo or scan (its paper is made clear). Saved signatures are kept for next time (the Sign button's arrow lists them, and removes them). Clicking a signature field signs there. Signatures can be moved and resized. File > Save as Flattened saves a copy in which comments, signatures and form fields are part of the page and can't be changed. These are visible signatures, like ink on paper, not certificate-based digital signatures. |
| Comments panel | F6 lists every comment by page, with author, date and text. Click one to go to it, double-click to edit it, Delete to remove it. The author name is your login name unless set in Comment > Comment Author Name. |
| Text | Select by dragging, including across pages (the view scrolls when you drag past an edge). Double-click selects a word and triple-click a line; Shift+click extends the selection. Ctrl+A selects all, Ctrl+C copies, Esc clears. |
| Find | Ctrl+F opens a find bar that searches as you type, on a thread of its own, so the window stays responsive on documents of thousands of pages. Every match is highlighted. Enter/F3 goes to the next match and Shift+Enter/Shift+F3 to the previous one. Match case and Whole words options. A phrase is found even where it wraps to the next line. |
| Navigation | Page thumbnails (F4), page number box, Home/End, Left/Right, Space/Shift+Space |
| Files | Open (and drag and drop), password-protected files, Open Recent, Save, Save As. Saves go to a temporary file first, then replace the original. |
| Pages | Drag thumbnails to reorder (several at once with Ctrl/Shift-click). Insert a blank page, insert the pages of another PDF, rotate, delete (Delete key in the thumbnails). Extract Pages saves pages (like "1-3, 5, 8-") as a new PDF, or each as a file of its own, and can delete them here afterwards. Split Document saves the document as several PDFs of so many pages each. Extracted pages keep their form fields. |
| Undo | Ctrl+Z / Ctrl+Y for every page edit. The title bar's modified marker follows undo and redo back to the saved state. |
| Printing | System print dialog, with page ranges and the current page. Shrinks to fit and auto-rotates landscape pages. Rendered at the printer's resolution. |

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
                  (Engine; Document: open, render, page edits, save; EditHistory: undo
                  snapshots; PageText: characters and their boxes, hit-testing, selection;
                  annotations: comments, markups and stamps; forms: form fields;
                  geometry: the page frames)
src/app           the Qt 6 app: MainWindow (menus, toolbars, tabs), DocumentTab (one open
                  document), DocumentView (tiled continuous view, selection, the markup
                  tools), PageRenderer (worker thread + image caches), ThumbnailPanel (drag
                  to reorder), CommentsPanel, CommentDialog, Markup (annotation cache,
                  hit-testing, tool styles, text box appearances), InlineTextEdit (typing
                  in a text box or form field on the page), Forms (field cache), Signatures
                  + SignatureDialog (draw, type or picture; saved ones), Stamps, ToolIcons,
                  TextSearch (search thread) + FindBar, Printing
src/tools         pdfz-info
tests             doctest unit tests for src/core
cmake             PDFium download/import, Windows deploy script
third_party       doctest (MIT)
```

PDFium isn't thread-safe. Every call into it goes through `pdfz::engineMutex()`, so the UI
thread can lay out pages while the worker threads render and search. A document keeps its
few most recently used pages parsed, because parsing a page is the slow part (over 100 ms
for a heavy drawing) and every render, text extraction and comment edit needs it. Page text is extracted once into
plain C++ data (`pdfz::PageText`), so hit-testing on every mouse move never waits for PDFium.

Comments are standard PDF annotations, so other readers show and edit them. Their
appearances are saved in the file. PDFium draws them for highlights, underlines,
strikethroughs, notes, ink, rectangles and ellipses. Text boxes get one built here: the text,
wrapped to the box, as glyph outlines, so they look the same in every reader without
embedding a font. Lines and arrows are saved as ink (PDFium can't create line annotations),
with "Line" or "Arrow" as their subject. While a text box is typed in, it's hidden in the
document (the editor shows its text), and the change is one undo step when the typing ends. As in Acrobat, a highlight's comment text starts out as
the text it marks.

Form fields are filled through PDFium's form layer (as if typed or clicked), so each field
keeps its own formatting and appearance. That layer draws fields over everything else, so
where a comment or signature overlaps a field, the page is drawn a second time without
comments and the two are combined, keeping the comment on top as it is in the file. There is
no JavaScript: forms that calculate totals by script don't calculate.

Signatures, initials, check marks and stamps are Stamp annotations whose appearance pdfz builds
from filled outlines (pen strokes given their width, typed names as glyph outlines) or a
picture with transparency, so they scale cleanly when resized and look the same everywhere.
Saved signatures are kept in `signatures.json` in the app's data folder
(`%APPDATA%\PDFEditZD\PDFEditZD` on Windows), and picture stamps in `stamps` beside it.

Undo keeps whole-document snapshots (the bytes of a PDF) rather than inverse operations.
PDFium can't put a deleted page back, only import a copy of it, and bookmarks, links and form
fields pointing at the original page would quietly break. While a document is unedited, its
snapshot is the file it was opened from, at no cost. Snapshots are capped at 768 MB in total,
oldest first.

## Roadmap

1. ~~**Phase 1, basic editing:** undo/redo for page edits, drag to reorder thumbnails, text
   selection, copy and search, markups and comments with a comments panel, tabs.~~ Done.
2. ~~**Phase 2:** Fill & Sign (form filling, and visible signatures drawn, typed or from a
   picture), stamps, extract and split pages.~~ Done.
3. **Phase 3:** export to Word and Excel. This is a layout-analysis problem: rebuilding
   paragraphs and tables from positioned text. It will be a separate module, and good results
   are expected on simple documents.

Out of scope: requesting e-signatures, Protect, Scan & OCR.

## License

GPL-3.0-only (see `LICENSE`). PDFium is BSD-3-Clause and Apache-2.0 (the pdfium-binaries
packaging is MIT). Its license and those of its dependencies are shipped in
`licenses/pdfium/` of a deployed folder. doctest is MIT.
