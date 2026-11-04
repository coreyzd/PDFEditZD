// SPDX-License-Identifier: GPL-3.0-only
// One open document: its pages view, thumbnails, comments list, undo history and caches. The
// main window shows one tab at a time and sends its menu and toolbar commands to it.
#pragma once

#include "Markup.hpp"
#include "TextSearch.hpp"

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

  // What an edit changes, for redrawing: all the pages (and their layout), or one page's comments.
  enum class Scope { Pages, Annotations };
  // An undoable edit: snapshots the document, runs `change` (true if it changed anything), then
  // redraws and shows `page`.
  bool applyEdit(const QString& label, int page, Scope scope, const std::function<bool(pdfz::Document&)>& change);
  void undo();
  void redo();

  // Comments. Indices are the annotation's index on its page.
  void addAnnotation(int page, pdfz::Annotation a);  // asks for a note's text
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

signals:
  void stateChanged();                // name, modified, undo/redo, page count
  void contentChanged(bool pages);    // after an edit, undo or redo; `pages` if the text may have changed
  void statusMessage(const QString& text, int timeoutMs);

private:
  void refresh(Scope scope, int page, int pageToShow);
  void showDocument(std::shared_ptr<pdfz::Document> doc, int pageToShow);  // undo/redo swap
  QString kindLabel(const pdfz::Annotation& a) const;
  void beginTextEdit(int page, int index);
  void onTextEditFinished(int page, int index, pdfz::Annotation a, bool changed);
  void refreshRendering(int page);  // the page's look changed, but not as an edit

  std::shared_ptr<pdfz::Document> doc_;
  QString path_;
  pdfz::EditHistory history_;
  PageRenderer* renderer_;
  TextCache textCache_;
  AnnotationCache annots_;
  TextSearch* search_;
  DocumentView* view_;
  ThumbnailPanel* thumbs_;    // shown in the main window's Pages panel
  CommentsPanel* comments_;   // shown in its Comments panel
  std::shared_ptr<const AnnotationList> keepAlive_;  // for annotation()
  // While a text box already in the document is typed in: the document from before, for undo.
  pdfz::EditHistory::Snapshot textEditBefore_;
};
