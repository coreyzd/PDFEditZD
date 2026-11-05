// SPDX-License-Identifier: GPL-3.0-only
// The pages, one under another, scrolled continuously. Draws each page from the tiles
// PageRenderer makes at the current zoom, with the page's preview underneath until they arrive.
// On top: the text selection, the find bar's matches, and the selected comment. The markup
// tools draw their shapes here too; the finished annotation is handed on (annotationCreated),
// and the document tab adds it as an undoable edit.
#pragma once

#include "ContentEdit.hpp"
#include "Forms.hpp"
#include "Markup.hpp"

#include "core/annotations.hpp"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QTimer>

#include <climits>
#include <vector>

class AnnotationCache;
class ContentCache;
class FieldCache;
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
  // markup tool set with setMarkupTool(). Place: a click puts down the stamp given to
  // startPlacing() (a signature), then it's back to Select. Edit: Edit PDF, the page's own
  // text and pictures.
  enum class Tool { Select, Hand, Markup, Place, Edit };

  DocumentView(PageRenderer* renderer, TextCache* text, TextSearch* search, AnnotationCache* annots,
               FieldCache* fields, ContentCache* content, QWidget* parent = nullptr);

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
  // Shows `preview` under the mouse, `size` points big, until a click on a page places the
  // stamp there (stampCreated).
  void startPlacing(const pdfz::Annotation& stamp, const pdfz::StampDrawing& drawing, const QImage& preview,
                    QSizeF size);

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
  // A Fill & Sign tool is in use: it stays in use after each thing it places, so a form can be
  // filled in one click after another.
  bool isFillingIn() const { return tool_ == Tool::Markup && isFillSignTool(markupTool_); }
  void startTextEdit(int page, int index, const pdfz::Annotation& a);
  // Ends the typing: emits textEditFinished. Nothing happens if there's none.
  void commitTextEdit();
  // Restyles the box being typed in.
  void restyleTextEdit(const StyleChange& change);
  // The box being typed in, as it stands (nullptr if none).
  const pdfz::Annotation* editedText() const { return editor_ ? &editAnnot_ : nullptr; }

  // Form fields: with the Select tool, a click fills one in (types in a text field on the
  // page, toggles a check box, opens a list of choices); Tab and Shift+Tab go through them.
  void setHighlightFields(bool on);
  bool highlightFields() const { return highlightFields_; }
  // Hovering a comment shows its text (off for drawings whose text is all comments too).
  void setCommentTips(bool on);
  // The fields changed (the cache has been told): repaint.
  void fieldsChanged();
  // Ends typing in a form field: emits fieldTextChanged if the text changed.
  void commitFieldEdit();

  // Edit PDF. With the Edit tool, blocks of the page's text and its pictures show with a
  // frame, and shapes when the mouse is over them: click one to select it (Shift+click adds or
  // takes away one more), or drag a box around several. The selection moves by dragging (or
  // the arrow keys), resizes by its handles, turns by the handle above it, and Delete removes
  // it. Click selected text to type in it. With "add text" on, a click starts new text there.
  void setEditAddText(bool on);
  bool editAddText() const { return editAddText_; }
  void setNewTextStyle(const RichRun& style) { editNewStyle_ = style; }
  void contentChanged(int page);  // the cache has been told: repaint
  // The one block selected, if just one is.
  bool selectedContent(int& page, pdfz::ContentBlock& block) const;
  bool hasContentSelection() const { return contentPage_ >= 0; }
  void selectContentAt(int page, pdfz::PointF point);  // the block there, if any
  // The blocks with any of these objects (after they moved or turned).
  void selectContentObjects(int page, const std::vector<int>& objects);
  void clearContentSelection();
  // The selection's page and objects (all of its blocks').
  int contentSelectionPage() const { return contentPage_; }
  std::vector<int> contentSelectionObjects() const;
  // Text of the selected text blocks, in order (for copying).
  QString contentSelectionText() const;
  void selectAllContent(int page);
  void deleteContentSelection();
  // The selection moved, turned or flipped by `m` (about its middle: see selectionCenter()).
  void transformContentSelection(const pdfz::Affine& m, const QString& label);
  pdfz::PointF contentSelectionCenter() const;
  // Typing in text: an existing block's (which the tab has hidden) or new.
  void startContentEdit(const TextEditing& editing);
  void commitContentEdit();
  bool isEditingContent() const { return contentEditor_ != nullptr; }
  const TextEditing* contentEditing() const { return contentEditor_ ? &editing_ : nullptr; }
  // The style where the typing is (of the first selected character, if any are).
  RichRun contentEditStyle() const;
  // Restyles the selected characters, or all the text if none are selected.
  void restyleContentEdit(const TextStyleChange& change);
  // Where new text or a picture would go: the middle of what's showing of the current page.
  bool visibleCenter(int& page, pdfz::PointF& point) const;

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
  void stampCreated(int page, const pdfz::Annotation& stamp, const pdfz::StampDrawing& drawing);
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
  // Filling in a form field (`index`: the widget's annotation index on `page`).
  void fieldTextChanged(int page, int index, const QString& text);
  void fieldCheckRequested(int page, int index, bool checked);
  void fieldChoiceRequested(int page, int index, int option);
  // A click on a signature field: sign there.
  void signatureFieldClicked(int page, const pdfz::RectF& rect);
  // Edit PDF.
  void contentSelectionChanged();
  void contentTransformed(int page, const std::vector<int>& objects, const pdfz::Affine& m, const QString& label);
  void contentDeleteRequested(int page, const std::vector<int>& objects);
  void contentEditRequested(int page, const pdfz::ContentBlock& block);
  void contentEditStarted();
  void contentEditStyleChanged();  // the typing moved to text in another style
  void contentEditFinished(const TextEditing& editing, const RichText& text);
  void editAddTextEnded();  // a click placed the new text: "add text" is off again

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
  bool focusNextPrevChild(bool next) override;

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
  pdfz::RectF resizedBounds() const;  // the selected comment's rect as the corner drag has it
  void placeMark(int page, QPointF point);  // a check mark, cross or dot, centered there
  void placeDate(int page, QPointF point);
  void finishCreate();
  void paintOverlays(QPainter& p);
  void layoutEditor(bool restyle);  // over the box, at the zoom; `restyle`: font and colors too
  void fitEditorHeight();  // and, for text typed on a form, its width
  // Form fields.
  bool fieldUnder(QPointF viewportPos, int& page, int& listPos) const;
  void activateField(int page, const pdfz::FormField& f);  // what a click (or Space) does
  void startFieldEdit(int page, const pdfz::FormField& f);
  void showChoices(int page, const pdfz::FormField& f);
  void moveFieldFocus(bool backwards);
  void setFieldFocus(int page, int index);
  const pdfz::FormField* focusedField() const;
  void layoutFieldEditor(bool restyle);
  // Edit PDF.
  bool contentUnder(QPointF viewportPos, int& page, int& listPos) const;
  // The selection's handles: 0-7 round it from the top left, clockwise; 8 the turning one.
  int contentHandleUnder(QPointF viewportPos) const;
  bool contentHandlesShown() const;
  pdfz::RectF contentSelectionRect() const;
  pdfz::Affine contentDragAffine() const;  // what the drag does to the selection
  QPolygonF contentOutline(int page, const pdfz::RectF& r, const pdfz::Affine& m) const;  // viewport
  void editPress(QMouseEvent* e);
  void finishContentDrag();  // drag_ says which
  void startNewText(int page, QPointF point);
  void layoutContentEditor(bool restyle);
  void fitContentEditor();
  void paintContent(QPainter& p);

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
  enum class Drag { None, MoveAnnot, ResizeAnnot, Create, MoveContent, ResizeContent, RotateContent, MarqueeContent };
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
  bool editAutoWidth_ = false;  // Fill & Sign text: as wide as its longest line
  double editMinWidth_ = 0;     // ...but at least this (points)
  // Placing a stamp.
  pdfz::Annotation placeStamp_;
  pdfz::StampDrawing placeDrawing_;
  QImage placePreview_;
  QSizeF placeSize_;
  int hoverPage_ = -1;
  QPointF hoverPoint_;  // on hoverPage_, in display points
  // Form fields.
  FieldCache* fields_;
  bool highlightFields_ = true;
  bool commentTips_ = true;
  QWidget* fieldEditor_ = nullptr;  // FieldLineEdit or InlineTextEdit
  int fieldPage_ = -1;
  pdfz::FormField fieldEdited_;
  quint64 fieldGeneration_ = 0;
  int focusPage_ = -1;   // the field with the keyboard (Tab, Space), if any
  int focusIndex_ = -1;
  // Edit PDF.
  ContentCache* content_;
  bool editAddText_ = false;
  bool editPressWasSelected_ = false;  // the block pressed on was selected already
  int contentPage_ = -1;           // the selected blocks' page
  std::vector<pdfz::ContentBlock> contentSel_;
  bool dragShift_ = false;         // Shift held at the press
  int hoverContentPage_ = -1;      // a shape under the mouse
  pdfz::RectF hoverContentRect_;
  InlineTextEdit* contentEditor_ = nullptr;
  TextEditing editing_;
  quint64 contentGeneration_ = 0;
  RichRun editNewStyle_{QString(), EditFont{QStringLiteral("Helvetica")}, 12, Qt::black, 0};  // new text
};
