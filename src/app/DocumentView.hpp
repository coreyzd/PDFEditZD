// SPDX-License-Identifier: GPL-3.0-only
// The pages, one under another, scrolled continuously. Draws each page from the tiles
// PageRenderer makes at the current zoom, with the page's preview underneath until they arrive.
// On top: the text selection, the find bar's matches, and the selected comment. The markup
// tools draw their shapes here too; the finished annotation is handed on (annotationCreated),
// and the document tab adds it as an undoable edit.
#pragma once

#include "Markup.hpp"

#include "core/annotations.hpp"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QPoint>
#include <QRectF>
#include <QTimer>

#include <climits>
#include <vector>

class AnnotationCache;
class InlineTextEdit;
class PageRenderer;
class TextCache;
class TextSearch;
struct TileKey;
namespace pdfz { struct RectF; struct Annotation; }

class DocumentView : public QAbstractScrollArea {
  Q_OBJECT
public:
  enum class FitMode { None, Width, Page };
  // Select: click a comment to select it (and drag to move it), drag over text to select it,
  // drag anywhere else to move the pages. Hand: dragging always moves the pages. Markup: the
  // markup tool set with setMarkupTool().
  enum class Tool { Select, Hand, Markup };

  DocumentView(PageRenderer* renderer, TextCache* text, TextSearch* search, AnnotationCache* annots,
               QWidget* parent = nullptr);

  // The document (or its pages) changed: lay it out again, keeping the current page in view
  // when `keepPosition` is set, otherwise starting at the top.
  void documentChanged(bool keepPosition);

  int currentPage() const { return currentPage_; }
  void goToPage(int page);

  // 1.0 is actual size: a 1-inch line is an inch long at the screen's logical DPI.
  double zoom() const { return zoom_; }
  FitMode fitMode() const { return fitMode_; }
  void setZoom(double zoom);  // leaves fit mode
  void setFitMode(FitMode mode);
  void zoomIn();
  void zoomOut();

  static constexpr double kMinZoom = 0.08;
  static constexpr double kMaxZoom = 64.0;

  Tool tool() const { return tool_; }
  void setTool(Tool tool);
  MarkupTool markupTool() const { return markupTool_; }
  void setMarkupTool(MarkupTool tool, const MarkupStyle& style);
  void setMarkupStyle(const MarkupStyle& style) { markupStyle_ = style; }

  // Comments. Annotations are identified by page and their index in the page's list.
  bool selectedAnnotation(int& page, int& index) const;
  void selectAnnotation(int page, int index, bool scrollIntoView = true);
  void clearAnnotationSelection();
  // The annotations on `page` changed (the cache has been told): repaint.
  void annotationsChanged(int page);
  // Highlights (underlines, strikes out) the selected text: one annotation per page.
  void markupSelection(MarkupTool tool, const MarkupStyle& style);

  // Typing in a text box, on the page. `index` is the box's index, or -1 for a new box that
  // isn't in the document yet (it's added when the typing ends, if it has any text).
  bool isEditingText() const { return editor_ != nullptr; }
  bool isEditingNewText() const { return editor_ != nullptr && editIndex_ < 0; }
  void startTextEdit(int page, int index, const pdfz::Annotation& a);
  // Ends the typing: emits textEditFinished. Nothing happens if there's none.
  void commitTextEdit();
  // Restyles the box being typed in.
  void restyleTextEdit(const StyleChange& change);
  // The box being typed in, as it stands (nullptr if none).
  const pdfz::Annotation* editedText() const { return editor_ ? &editAnnot_ : nullptr; }

  // Text selection. It may run across pages.
  bool hasSelection() const;
  QString selectedText() const;  // pages joined by line breaks
  void clearSelection();
  void selectAll();
  // Selects characters [start, end) of `page` and scrolls them into view.
  void selectRange(int page, int start, int end);
  // Scrolls so `rect` (display points on `page`) is in view, centering it if it wasn't.
  void ensureVisible(int page, const pdfz::RectF& rect);

  // The search matches changed: repaint them.
  void searchChanged();

signals:
  void currentPageChanged(int page);
  void zoomChanged(double zoom, DocumentView::FitMode mode);
  void selectionChanged(bool hasSelection);
  void annotationCreated(int page, const pdfz::Annotation& annotation);
  void annotationMoved(int page, int index, double dx, double dy);  // display points
  void annotationResized(int page, int index, const pdfz::RectF& rect);
  void annotationEditRequested(int page, int index);
  void annotationDeleteRequested(int page, int index);
  void annotationSelectionChanged(int page, int index);  // -1, -1 for none
  // Right click: `annotIndex` is the comment clicked on (now selected), or -1.
  void contextMenuRequested(const QPoint& globalPos, int page, int annotIndex);
  void textEditStarted();
  // The typing in a text box ended. `a` is the box with its text; `changed` if the text, size
  // or style differ from what it started with.
  void textEditFinished(int page, int index, const pdfz::Annotation& a, bool changed);
  // Esc with nothing left to cancel: back to the Select tool.
  void toolResetRequested();

protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void scrollContentsBy(int dx, int dy) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;
  void changeEvent(QEvent*) override;

private:
  double pixelsPerPoint() const;  // logical pixels per PDF point at the current zoom
  double fitZoom(FitMode mode) const;
  void relayout();
  void updateScrollBars();
  // Zooms so the content point under viewport position `anchor` stays under it.
  void applyZoom(double zoom, FitMode mode, QPoint anchor);
  QRectF visibleRect() const;  // in content coordinates
  int pageAt(double contentY) const;
  void updateCurrentPage();
  void scheduleRequests();
  void sendRequests();
  void onTileReady(const TileKey& key);
  void onPreviewReady(int page);

  // A place in the text: before character `caret` of `page` (INT_MAX: the page's end).
  struct TextPos {
    int page = 0;
    int caret = 0;
    friend bool operator==(const TextPos&, const TextPos&) = default;
    friend bool operator<(const TextPos& a, const TextPos& b) {
      return a.page != b.page ? a.page < b.page : a.caret < b.caret;
    }
  };
  // Viewport position to page and display points on it; false off the pages.
  bool pagePointAt(QPointF viewportPos, int& page, QPointF& point, bool clampToNearestPage) const;
  QRectF toViewport(int page, const pdfz::RectF& r) const;
  bool overText(QPointF viewportPos) const;
  void extendSelectionTo(QPointF viewportPos);
  void autoScrollStep();
  void updateHoverCursor(QPointF viewportPos);
  // [start, end) of the selection on `page`; start == end when the page has none.
  std::pair<int, int> selectionOn(int page) const;
  void setSelection(TextPos anchor, TextPos focus);

  double pagePixelsPerPoint(int page) const;
  QPointF toViewport(int page, double x, double y) const;
  // A viewport position in the display points of `page`, wherever it is.
  QPointF pointOnPage(int page, QPointF viewportPos) const;
  // The annotation under a viewport position: page and position in its cached list.
  bool annotationUnder(QPointF viewportPos, int& page, int& listPos) const;
  const pdfz::Annotation* selected() const;
  pdfz::RectF selectedBounds() const;  // display points
  int resizeHandleUnder(QPointF viewportPos) const;  // 0..3 (TL, TR, BL, BR), or -1
  void finishCreate();
  void paintOverlays(QPainter& p);
  void layoutEditor(bool restyle);  // over the box, at the zoom; `restyle`: font and colors too
  void fitEditorHeight();

  PageRenderer* renderer_;
  TextCache* text_;
  TextSearch* search_;
  AnnotationCache* annots_;
  Tool tool_ = Tool::Select;
  MarkupTool markupTool_ = MarkupTool::Highlight;
  MarkupStyle markupStyle_;
  int selPage_ = -1;   // the selected annotation
  int selIndex_ = -1;
  // A drag in progress on page dragPage_, in its display points.
  enum class Drag { None, MoveAnnot, ResizeAnnot, Create };
  Drag drag_ = Drag::None;
  int dragPage_ = -1;
  QPointF dragStart_;
  QPointF dragNow_;
  bool dragMoved_ = false;
  int resizeCorner_ = -1;
  std::vector<pdfz::PointF> inkPoints_;
  TextPos anchor_;
  TextPos focus_;
  bool selecting_ = false;
  QTimer autoScroll_;
  QPoint lastMousePos_;
  QElapsedTimer sinceDoubleClick_;  // for triple clicks (select the line)
  QPoint doubleClickPos_;
  double zoom_ = 1.0;
  FitMode fitMode_ = FitMode::Width;
  std::vector<QRectF> pageRects_;  // content coordinates (logical pixels)
  QSizeF contentSize_;
  int currentPage_ = 0;
  double layoutDpi_ = 0;  // the screen DPI the layout was made for
  QTimer requestTimer_;
  bool panning_ = false;
  QPoint panLast_;
  // The text box being typed in.
  InlineTextEdit* editor_ = nullptr;
  int editPage_ = -1;
  int editIndex_ = -1;
  pdfz::Annotation editAnnot_;
  pdfz::Annotation editOriginal_;
  quint64 editGeneration_ = 0;  // tells a stale editor's late signals apart
};
