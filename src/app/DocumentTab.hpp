// SPDX-License-Identifier: GPL-3.0-only
// One open document: its pages view, thumbnails, comments list, undo history and caches. The
// main window shows one tab at a time and sends its menu and toolbar commands to it.
#pragma once

#include "ContentEdit.hpp"
#include "Forms.hpp"
#include "Markup.hpp"
#include "TextSearch.hpp"

#include "core/document.hpp"
#include "core/history.hpp"

#include <QColor>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

namespace pdfz { class Document; }
class CommentsPanel;
class DocumentView;
class PageRenderer;
class ThumbnailPanel;

class DocumentTab : public QWidget {
  Q_OBJECT
public:
  explicit DocumentTab(QWidget* parent = nullptr);
  ~DocumentTab() override;

  bool hasDocument() const { return doc_ != nullptr; }
  const std::shared_ptr<pdfz::Document>& document() const { return doc_; }
  const QString& path() const { return path_; }
  QString displayName() const;  // the file name, or "Untitled"
  bool isModified() const { return history_.isModified(); }
  const pdfz::EditHistory& history() const { return history_; }

  DocumentView* view() const { return view_; }
  ThumbnailPanel* thumbnails() const { return thumbs_; }
  CommentsPanel* comments() const { return comments_; }
  TextSearch* search() const { return search_; }

  // A document just opened from `path` (empty for a new one).
  void setDocument(std::shared_ptr<pdfz::Document> doc, const QString& path);
  // Saves to `path`, which becomes the document's path; reports a failure itself.
  bool saveTo(const QString& path);

  // What an edit changes, for redrawing: all the pages (and their layout), one page's comments,
  // a form field's value (shown on `page`, and wherever else the field is), or one page's own
  // content (Edit PDF).
  enum class Scope { Pages, Annotations, Field, Content };
  // An undoable edit: snapshots the document, runs `change` (true if it changed anything), then
  // redraws and shows `page`.
  bool applyEdit(const QString& label, int page, Scope scope, const std::function<bool(pdfz::Document&)>& change);
  void undo();
  void redo();

  // Comments. Indices are the annotation's index on its page.
  void addAnnotation(int page, pdfz::Annotation a);  // asks for a note's text
  // A signature, initials or mark: `a` has its rect and subject.
  void addStamp(int page, pdfz::Annotation a, const pdfz::StampDrawing& drawing);
  // A note's text in a dialog; a text box is typed in where it is, on the page.
  void editAnnotation(int page, int index);
  void deleteAnnotation(int page, int index);
  void moveAnnotation(int page, int index, double dx, double dy);
  void resizeAnnotation(int page, int index, const pdfz::RectF& rect);
  // Restyles a comment.
  void restyleAnnotation(int page, int index, const StyleChange& change);
  // Typing in a text box on the page: ends it (adding or changing the box, one undo step).
  // Done before anything else changes or saves the document.
  void finishTextEditing();
  // The annotation as the cache has it, or nullptr.
  const pdfz::Annotation* annotation(int page, int index);

  // Form fields (`index`: the widget's annotation index).
  bool hasForm() const { return fields_.hasForm(); }
  void setFieldText(int page, int index, const QString& text);
  void setFieldChecked(int page, int index, bool checked);
  void setFieldChoice(int page, int index, int option);

  // Edit PDF: a picture added to the page's content, filling `rect` (display frame); it's then
  // selected.
  void addContentImage(int page, const pdfz::Document::PageImage& image, const pdfz::RectF& rect);
  // The selected content to the clipboard (as a PDF of its own, which pastes in the same place
  // in any document, and as a picture and text for other programs); cutting deletes it too.
  void copyContent(bool cut);
  // The clipboard's content (copied here) or picture onto `page`, then selected. False if
  // there's nothing to paste.
  bool pasteContent(int page);
  static bool canPaste();
  // The selection's draw order.
  void arrangeContent(pdfz::Arrange how);
  // A block of the page's text written again in another font, size or color.
  void restyleContent(int page, const pdfz::ContentBlock& block, const TextStyleChange& change);

signals:
  void stateChanged();                // name, modified, undo/redo, page count
  void contentChanged(bool pages);    // after an edit, undo or redo; `pages` if the text may have changed
  void statusMessage(const QString& text, int timeoutMs);

private:
  void refresh(Scope scope, int page, int pageToShow);
  void showDocument(std::shared_ptr<pdfz::Document> doc, int pageToShow);  // undo/redo swap
  QString kindLabel(const pdfz::Annotation& a) const;
  bool fillField(int page, int index, const std::function<bool(pdfz::Document&)>& change);
  void beginTextEdit(int page, int index);
  void onTextEditFinished(int page, int index, pdfz::Annotation a, bool changed);
  void refreshRendering(int page);  // the page's look changed, but not as an edit
  // Edit PDF.
  void transformContent(int page, std::vector<int> objects, pdfz::Affine m, QString label);
  void deleteContent(int page, std::vector<int> objects);
  void beginContentEdit(int page, pdfz::ContentBlock block);
  void onContentEditFinished(TextEditing editing, RichText text);
  void refreshContentRendering(int page);  // hidden or shown again, not as an edit
  static TextEditing editingFor(int page, const pdfz::ContentBlock& block);
  // The text laid out for the page; false (and a message) if no font can show it.
  bool layoutFor(const TextEditing& e, const RichText& text, pdfz::PlacedText& out);

  std::shared_ptr<pdfz::Document> doc_;
  QString path_;
  pdfz::EditHistory history_;
  PageRenderer* renderer_;
  TextCache textCache_;
  AnnotationCache annots_;
  FieldCache fields_;
  ContentCache content_;
  TextSearch* search_;
  DocumentView* view_;
  ThumbnailPanel* thumbs_;    // shown in the main window's Pages panel
  CommentsPanel* comments_;   // shown in its Comments panel
  std::shared_ptr<const AnnotationList> keepAlive_;  // for annotation()
  // While a text box already in the document is typed in: the document from before, for undo.
  pdfz::EditHistory::Snapshot textEditBefore_;
  // The same, while a block of the page's own text is typed in.
  pdfz::EditHistory::Snapshot contentEditBefore_;
};
