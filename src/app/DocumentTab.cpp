// SPDX-License-Identifier: GPL-3.0-only
#include "DocumentTab.hpp"

#include "CommentDialog.hpp"
#include "CommentsPanel.hpp"
#include "DocumentView.hpp"
#include "PageRenderer.hpp"
#include "ThumbnailPanel.hpp"

#include "core/annotations.hpp"
#include "core/document.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

DocumentTab::DocumentTab(QWidget* parent) : QWidget(parent) {
  renderer_ = new PageRenderer(this);
  search_ = new TextSearch(this);
  view_ = new DocumentView(renderer_, &textCache_, search_, &annots_, this);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->addWidget(view_);
  // Not in this widget's layout: the main window puts them in its side panels.
  thumbs_ = new ThumbnailPanel(renderer_, nullptr);
  comments_ = new CommentsPanel(nullptr);

  // Queued: adding a note or a text box opens a dialog, which shouldn't run inside the view's
  // mouse handling.
  connect(view_, &DocumentView::annotationCreated, this, &DocumentTab::addAnnotation, Qt::QueuedConnection);
  connect(view_, &DocumentView::annotationMoved, this, &DocumentTab::moveAnnotation);
  connect(view_, &DocumentView::annotationResized, this, &DocumentTab::resizeAnnotation);
  connect(view_, &DocumentView::annotationEditRequested, this, &DocumentTab::editAnnotation);
  connect(view_, &DocumentView::annotationDeleteRequested, this, &DocumentTab::deleteAnnotation);
  connect(view_, &DocumentView::annotationSelectionChanged, this, [this](int page, int index) {
    if (page >= 0) comments_->showAnnotation(page, index);
  });
  connect(comments_, &CommentsPanel::activated, this, [this](int page, int index) {
    view_->goToPage(page);
    view_->selectAnnotation(page, index, true);
  });
  connect(comments_, &CommentsPanel::editRequested, this, &DocumentTab::editAnnotation);
  connect(view_, &DocumentView::textEditFinished, this, &DocumentTab::onTextEditFinished);
  connect(comments_, &CommentsPanel::deleteRequested, this, &DocumentTab::deleteAnnotation);
  connect(thumbs_, &ThumbnailPanel::pageActivated, view_, &DocumentView::goToPage);
}

DocumentTab::~DocumentTab() {
  // The panels live in the main window's docks; they and the view use the renderer, which
  // goes when this widget does.
  delete thumbs_;
  delete comments_;
  delete view_;
}

QString DocumentTab::displayName() const { return path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName(); }

void DocumentTab::setDocument(std::shared_ptr<pdfz::Document> doc, const QString& path) {
  finishTextEditing();
  doc_ = std::move(doc);
  path_ = path;
  history_.clear();
  view_->clearSelection();
  view_->clearAnnotationSelection();
  search_->clear();
  textCache_.setDocument(doc_);
  annots_.setDocument(doc_);
  renderer_->setDocument(doc_);
  thumbs_->documentChanged();
  comments_->setDocument(doc_);
  view_->documentChanged(false);
  emit stateChanged();
}

bool DocumentTab::saveTo(const QString& path) {
  if (!doc_) return false;
  finishTextEditing();
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const bool ok = doc_->save(std::filesystem::path(path.toStdU16String()));
  QApplication::restoreOverrideCursor();
  if (!ok) {
    QMessageBox::warning(this, tr("Save"), tr("Couldn't save %1. Is it open in another program, or read-only?")
                                               .arg(QDir::toNativeSeparators(path)));
    return false;
  }
  path_ = QFileInfo(path).absoluteFilePath();
  history_.markClean();
  emit stateChanged();
  emit statusMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 4000);
  return true;
}

bool DocumentTab::applyEdit(const QString& label, int page, Scope scope,
                            const std::function<bool(pdfz::Document&)>& change) {
  if (!doc_) return false;
  finishTextEditing();  // its own edit comes first
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto before = pdfz::EditHistory::capture(*doc_, view_->currentPage());
  QApplication::restoreOverrideCursor();
  if (!before.bytes) {
    QMessageBox::warning(this, label, tr("Couldn't prepare to undo this change, so it wasn't made."));
    return false;
  }
  if (!change(*doc_)) return false;
  history_.push(std::move(before), label.toStdString());
  refresh(scope, page, page);
  return true;
}

void DocumentTab::refresh(Scope scope, int page, int pageToShow) {
  if (scope == Scope::Annotations) {
    annots_.invalidate(page);
    renderer_->invalidatePage(page);
    thumbs_->pageChanged(page);
    view_->annotationsChanged(page);
    comments_->refreshPage(page);
    emit stateChanged();
    emit contentChanged(false);
    return;
  }
  view_->clearSelection();
  view_->clearAnnotationSelection();
  textCache_.setDocument(doc_);
  annots_.setDocument(doc_);
  renderer_->invalidate();
  thumbs_->documentChanged();
  comments_->setDocument(doc_);
  view_->documentChanged(true);
  view_->goToPage(std::clamp(pageToShow, 0, doc_->pageCount() - 1));
  emit stateChanged();
  emit contentChanged(true);
}

void DocumentTab::showDocument(std::shared_ptr<pdfz::Document> doc, int pageToShow) {
  doc_ = std::move(doc);
  renderer_->setDocument(doc_);
  refresh(Scope::Pages, pageToShow, pageToShow);
}

void DocumentTab::undo() {
  finishTextEditing();
  if (!doc_ || !history_.canUndo()) return;
  const QString what = QString::fromStdString(history_.undoLabel());
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto r = history_.undo(*doc_, view_->currentPage());
  QApplication::restoreOverrideCursor();
  if (!r.document) {
    QMessageBox::warning(this, tr("Undo"), tr("Couldn't undo %1.").arg(what));
    return;
  }
  showDocument(std::shared_ptr<pdfz::Document>(std::move(r.document)), r.page);
  emit statusMessage(tr("Undid %1").arg(what), 3000);
}

void DocumentTab::redo() {
  finishTextEditing();
  if (!doc_ || !history_.canRedo()) return;
  const QString what = QString::fromStdString(history_.redoLabel());
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto r = history_.redo(*doc_, view_->currentPage());
  QApplication::restoreOverrideCursor();
  if (!r.document) {
    QMessageBox::warning(this, tr("Redo"), tr("Couldn't redo %1.").arg(what));
    return;
  }
  showDocument(std::shared_ptr<pdfz::Document>(std::move(r.document)), r.page);
  emit statusMessage(tr("Redid %1").arg(what), 3000);
}

// ---------------------------------------------------------------------------------------------
// Comments

QString DocumentTab::kindLabel(const pdfz::Annotation& a) const {
  if (a.subject == "Arrow") return tr("Arrow");
  if (a.subject == "Line") return tr("Line");
  if (a.kind == pdfz::AnnotKind::Note) return tr("Sticky Note");
  if (a.kind == pdfz::AnnotKind::StrikeOut) return tr("Strikethrough");
  return tr(pdfz::kindName(a.kind));
}

const pdfz::Annotation* DocumentTab::annotation(int page, int index) {
  keepAlive_ = annots_.get(page);
  if (!keepAlive_) return nullptr;
  for (const pdfz::Annotation& a : *keepAlive_)
    if (a.index == index) return &a;
  return nullptr;
}

void DocumentTab::addAnnotation(int page, pdfz::Annotation a) {
  if (!doc_) return;
  a.author = commentAuthor().toStdString();
  if (a.kind == pdfz::AnnotKind::Note) {
    CommentDialog dlg(kindLabel(a), QString::fromStdString(a.author), {}, this);
    if (dlg.exec() != QDialog::Accepted) return;
    a.contents = dlg.text().toStdString();
  } else if (a.kind == pdfz::AnnotKind::FreeText) {
    a.appearance = freeTextAppearance(a, doc_->pageFrame(page));
  }
  int added = -1;
  if (applyEdit(tr("Add %1").arg(kindLabel(a)), page, Scope::Annotations, [&](pdfz::Document& d) {
        added = d.addAnnotation(page, a);
        return added >= 0;
      })) {
    if (!pdfz::isTextMarkup(a.kind)) view_->selectAnnotation(page, added, false);
  }
}

void DocumentTab::editAnnotation(int page, int index) {
  const pdfz::Annotation* found = annotation(page, index);
  if (!found) return;
  if (found->kind == pdfz::AnnotKind::FreeText) {
    beginTextEdit(page, index);
    return;
  }
  const pdfz::Annotation a = *found;
  QString byline = QString::fromStdString(a.author);
  const QString when = formatPdfDate(a.modified);
  if (!when.isEmpty()) byline += (byline.isEmpty() ? "" : " · ") + when;
  CommentDialog dlg(kindLabel(a), byline, QString::fromStdString(a.contents), this);
  if (dlg.exec() != QDialog::Accepted) return;
  const std::string text = dlg.text().toStdString();
  if (text == a.contents) return;
  applyEdit(tr("Edit Comment"), page, Scope::Annotations,
            [&](pdfz::Document& d) { return d.setAnnotationContents(page, index, text); });
  view_->selectAnnotation(page, index, false);
}

void DocumentTab::finishTextEditing() { view_->commitTextEdit(); }

void DocumentTab::refreshRendering(int page) {
  annots_.invalidate(page);
  renderer_->invalidatePage(page);
  view_->annotationsChanged(page);
}

// The box is hidden in the document while it's typed in, so the page shows only the editor's
// text. The snapshot from before becomes the undo step if anything changed.
void DocumentTab::beginTextEdit(int page, int index) {
  finishTextEditing();
  const pdfz::Annotation* found = annotation(page, index);
  if (!doc_ || !found || found->kind != pdfz::AnnotKind::FreeText) return;
  const pdfz::Annotation a = *found;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto before = pdfz::EditHistory::capture(*doc_, page);
  QApplication::restoreOverrideCursor();
  if (!before.bytes) {
    QMessageBox::warning(this, tr("Edit Text Box"), tr("Couldn't prepare to undo this change, so it can't be made."));
    return;
  }
  textEditBefore_ = std::move(before);
  doc_->setAnnotationHidden(page, index, true);
  view_->clearAnnotationSelection();
  refreshRendering(page);
  view_->startTextEdit(page, index, a);
}

void DocumentTab::onTextEditFinished(int page, int index, pdfz::Annotation a, bool changed) {
  if (!doc_) return;
  const bool empty = QString::fromStdString(a.contents).trimmed().isEmpty();
  if (index < 0) {  // a new box: added now, unless nothing was typed in it
    if (!empty) addAnnotation(page, std::move(a));
    return;
  }
  pdfz::EditHistory::Snapshot before = std::move(textEditBefore_);
  textEditBefore_ = {};
  if (!changed || !before.bytes) {
    doc_->setAnnotationHidden(page, index, false);
    refreshRendering(page);
    view_->selectAnnotation(page, index, false);
    return;
  }
  if (empty) {  // all the text deleted: so is the box
    if (doc_->removeAnnotation(page, index)) {
      history_.push(std::move(before), tr("Delete Text Box").toStdString());
      refresh(Scope::Annotations, page, page);
    }
    return;
  }
  a.hidden = false;
  a.appearance = freeTextAppearance(a, doc_->pageFrame(page));
  const int now = doc_->updateAnnotation(page, index, a);
  if (now < 0) {
    doc_->setAnnotationHidden(page, index, false);
    refreshRendering(page);
    QMessageBox::warning(this, tr("Edit Text Box"), tr("Couldn't change the text box."));
    return;
  }
  history_.push(std::move(before), tr("Edit Text Box").toStdString());
  refresh(Scope::Annotations, page, page);
  view_->selectAnnotation(page, now, false);
}

void DocumentTab::deleteAnnotation(int page, int index) {
  const pdfz::Annotation* a = annotation(page, index);
  if (!a) return;
  const QString label = tr("Delete %1").arg(kindLabel(*a));
  view_->clearAnnotationSelection();
  if (applyEdit(label, page, Scope::Annotations, [&](pdfz::Document& d) { return d.removeAnnotation(page, index); }))
    emit statusMessage(tr("Deleted the comment (Ctrl+Z to undo)"), 3000);
}

void DocumentTab::moveAnnotation(int page, int index, double dx, double dy) {
  const pdfz::Annotation* a = annotation(page, index);
  if (!a) return;
  applyEdit(tr("Move %1").arg(kindLabel(*a)), page, Scope::Annotations,
            [&](pdfz::Document& d) { return d.moveAnnotation(page, index, dx, dy); });
  view_->selectAnnotation(page, index, false);
}

void DocumentTab::resizeAnnotation(int page, int index, const pdfz::RectF& rect) {
  const pdfz::Annotation* found = annotation(page, index);
  if (!found || !canResize(*found)) return;
  pdfz::Annotation a = *found;
  a.rect = rect;
  if (a.kind == pdfz::AnnotKind::FreeText) a.appearance = freeTextAppearance(a, doc_->pageFrame(page));
  int now = -1;
  applyEdit(tr("Resize %1").arg(kindLabel(a)), page, Scope::Annotations, [&](pdfz::Document& d) {
    now = d.updateAnnotation(page, index, a);
    return now >= 0;
  });
  if (now >= 0) view_->selectAnnotation(page, now, false);
}

void DocumentTab::restyleAnnotation(int page, int index, const StyleChange& change) {
  const pdfz::Annotation* found = annotation(page, index);
  if (!found) return;
  if (!found->editable()) {
    emit statusMessage(tr("This kind of comment can't be restyled here."), 3000);
    return;
  }
  pdfz::Annotation a = *found;
  applyStyleChange(a, change);
  if (a.kind == pdfz::AnnotKind::FreeText) a.appearance = freeTextAppearance(a, doc_->pageFrame(page));
  int now = -1;
  applyEdit(styleChangeLabel(change), page, Scope::Annotations, [&](pdfz::Document& d) {
    now = d.updateAnnotation(page, index, a);
    return now >= 0;
  });
  if (now >= 0) view_->selectAnnotation(page, now, false);
}
