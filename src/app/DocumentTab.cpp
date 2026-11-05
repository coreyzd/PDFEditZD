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
  view_ = new DocumentView(renderer_, &textCache_, search_, &annots_, &fields_, &content_, this);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->addWidget(view_);
  // Not in this widget's layout: the main window puts them in its side panels.
  thumbs_ = new ThumbnailPanel(renderer_, nullptr);
  comments_ = new CommentsPanel(nullptr);

  // Queued: adding a note or a text box opens a dialog, which shouldn't run inside the view's
  // mouse handling.
  connect(view_, &DocumentView::annotationCreated, this, &DocumentTab::addAnnotation, Qt::QueuedConnection);
  connect(view_, &DocumentView::stampCreated, this, &DocumentTab::addStamp);
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
  connect(view_, &DocumentView::fieldTextChanged, this, &DocumentTab::setFieldText);
  connect(view_, &DocumentView::fieldCheckRequested, this, &DocumentTab::setFieldChecked);
  connect(view_, &DocumentView::fieldChoiceRequested, this, &DocumentTab::setFieldChoice);
  connect(comments_, &CommentsPanel::deleteRequested, this, &DocumentTab::deleteAnnotation);
  connect(thumbs_, &ThumbnailPanel::pageActivated, view_, &DocumentView::goToPage);
  connect(view_, &DocumentView::contentTransformed, this, &DocumentTab::transformContent);
  connect(view_, &DocumentView::contentDeleteRequested, this, &DocumentTab::deleteContent);
  connect(view_, &DocumentView::contentEditRequested, this, &DocumentTab::beginContentEdit);
  // Direct: a click that ends the typing then finds the page's blocks as they are now.
  connect(view_, &DocumentView::contentEditFinished, this, &DocumentTab::onContentEditFinished);
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
  fields_.setDocument(doc_);
  content_.setDocument(doc_);
  view_->clearContentSelection();
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
  if (scope == Scope::Field) {
    // The field may be shown on other pages too (those looked at so far are known).
    std::vector<int> pages = {page};
    for (const pdfz::FormField& f : *fields_.get(page))
      for (int p : fields_.pagesWithField(f.name))
        if (std::find(pages.begin(), pages.end(), p) == pages.end()) pages.push_back(p);
    fields_.invalidate();
    for (int p : pages) {
      renderer_->invalidatePage(p);
      thumbs_->pageChanged(p);
    }
    view_->fieldsChanged();
    emit stateChanged();
    emit contentChanged(false);
    return;
  }
  if (scope == Scope::Content) {
    content_.invalidate(page);
    textCache_.invalidate(page);
    search_->clear();
    renderer_->invalidatePage(page);
    thumbs_->pageChanged(page);
    view_->contentChanged(page);
    emit stateChanged();
    emit contentChanged(true);
    return;
  }
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
  view_->clearContentSelection();
  textCache_.setDocument(doc_);
  annots_.setDocument(doc_);
  fields_.setDocument(doc_);
  content_.setDocument(doc_);
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
  if (a.kind == pdfz::AnnotKind::Stamp) {
    if (a.subject == "Signature") return tr("Signature");
    if (a.subject == "Initials") return tr("Initials");
    if (a.subject == "Check") return tr("Checkmark");
    if (a.subject == "Cross") return tr("Cross");
    if (a.subject == "Dot") return tr("Dot");
    return tr("Stamp");
  }
  if (a.kind == pdfz::AnnotKind::FreeText && a.subject == "Date") return tr("Date");
  if (a.kind == pdfz::AnnotKind::FreeText && a.subject == "Fill Text") return tr("Text");
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
    // Not while filling in a form: the tool goes on to the next field.
    if (!pdfz::isTextMarkup(a.kind) && !view_->isFillingIn()) view_->selectAnnotation(page, added, false);
  }
}

void DocumentTab::addStamp(int page, pdfz::Annotation a, const pdfz::StampDrawing& drawing) {
  if (!doc_) return;
  a.kind = pdfz::AnnotKind::Stamp;
  a.author = commentAuthor().toStdString();
  int added = -1;
  if (applyEdit(tr("Add %1").arg(kindLabel(a)), page, Scope::Annotations, [&](pdfz::Document& d) {
        added = d.addStamp(page, a, drawing);
        return added >= 0;
      }) && !view_->isFillingIn())
    view_->selectAnnotation(page, added, false);
}

void DocumentTab::editAnnotation(int page, int index) {
  const pdfz::Annotation* found = annotation(page, index);
  if (!found) return;
  if (found->kind == pdfz::AnnotKind::FreeText) {
    beginTextEdit(page, index);
    return;
  }
  if (found->kind == pdfz::AnnotKind::Stamp) return;  // a signature or mark has nothing to edit
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

void DocumentTab::finishTextEditing() {
  view_->commitTextEdit();
  view_->commitFieldEdit();
  view_->commitContentEdit();
}

bool DocumentTab::fillField(int page, int index, const std::function<bool(pdfz::Document&)>& change) {
  QString label = tr("Fill In Form");
  for (const pdfz::FormField& f : *fields_.get(page))
    if (f.index == index) label = tr("Fill In %1").arg(fieldLabel(f));
  if (applyEdit(label, page, Scope::Field, change)) return true;
  emit statusMessage(tr("That field can't be filled in."), 3000);
  return false;
}

void DocumentTab::setFieldText(int page, int index, const QString& text) {
  const std::string t = text.toStdString();
  fillField(page, index, [&](pdfz::Document& d) { return d.setFieldText(page, index, t); });
}

void DocumentTab::setFieldChecked(int page, int index, bool checked) {
  fillField(page, index, [&](pdfz::Document& d) { return d.setFieldChecked(page, index, checked); });
}

void DocumentTab::setFieldChoice(int page, int index, int option) {
  fillField(page, index, [&](pdfz::Document& d) { return d.setFieldChoice(page, index, option); });
}

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
    if (!view_->isFillingIn()) view_->selectAnnotation(page, index, false);
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
  if (!view_->isFillingIn()) view_->selectAnnotation(page, now, false);
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
  if (!found->editable() && !isMark(*found)) {
    emit statusMessage(tr("This kind of comment can't be restyled here."), 3000);
    return;
  }
  pdfz::Annotation a = *found;
  applyStyleChange(a, change);
  if (isMark(a)) {  // a check mark, cross or dot: drawn again in the new color
    if (!change.color) return;
    int now = -1;
    applyEdit(styleChangeLabel(change), page, Scope::Annotations, [&](pdfz::Document& d) {
      if (!d.removeAnnotation(page, index)) return false;
      now = d.addStamp(page, a, markDrawing(a.subject, toQColor(a.color)));
      return now >= 0;
    });
    if (now >= 0) view_->selectAnnotation(page, now, false);
    return;
  }
  if (a.kind == pdfz::AnnotKind::FreeText) a.appearance = freeTextAppearance(a, doc_->pageFrame(page));
  int now = -1;
  applyEdit(styleChangeLabel(change), page, Scope::Annotations, [&](pdfz::Document& d) {
    now = d.updateAnnotation(page, index, a);
    return now >= 0;
  });
  if (now >= 0) view_->selectAnnotation(page, now, false);
}

// ---------------------------------------------------------------------------------------------
// Edit PDF

namespace {
pdfz::PointF centerOf(const pdfz::RectF& r) { return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }
}  // namespace

void DocumentTab::refreshContentRendering(int page) {
  content_.invalidate(page);
  renderer_->invalidatePage(page);
  view_->contentChanged(page);
}

void DocumentTab::transformContent(int page, std::vector<int> objects, pdfz::Affine m, QString label) {
  if (!doc_ || objects.empty()) return;
  if (applyEdit(label, page, Scope::Content, [&](pdfz::Document& d) { return d.transformContent(page, objects, m); }))
    view_->selectContentObjects(page, objects);  // moving and turning keep the objects' places
}

void DocumentTab::deleteContent(int page, std::vector<int> objects) {
  if (!doc_) return;
  view_->clearContentSelection();
  if (applyEdit(tr("Delete"), page, Scope::Content, [&](pdfz::Document& d) { return d.deleteContent(page, objects); }))
    emit statusMessage(tr("Deleted (Ctrl+Z to undo)"), 3000);
}

void DocumentTab::addContentImage(int page, const pdfz::Document::PageImage& image, const pdfz::RectF& rect) {
  if (!doc_) return;
  if (applyEdit(tr("Add Image"), page, Scope::Content,
                [&](pdfz::Document& d) { return d.addContentImage(page, image, rect); }))
    view_->selectContentAt(page, centerOf(rect));
  else
    QMessageBox::warning(this, tr("Add Image"), tr("Couldn't add the picture to the page."));
}

TextEditing DocumentTab::editingFor(int page, const pdfz::ContentBlock& block) {
  TextEditing t;
  t.page = page;
  t.block = block;
  t.original = richFromBlock(block);
  t.style = mainStyle(t.original);
  if (t.style.size <= 0) t.style.size = 12;
  t.align = block.align;
  t.lineSpacing = block.lineSpacing > 0 ? block.lineSpacing : t.style.size * 1.2;
  // A paragraph keeps its box, and its lines wrap again in it; one line (lined up on the
  // left) grows as it's typed in.
  const bool paragraph = block.lineSpacing > 0 || plainText(t.original).contains('\n');
  if (block.align == pdfz::TextAlign::Left) {
    t.origin = block.origin;
    t.tabs = block.tabStops;
    // (A little more than the ink: the last character's advance, and rounding, mustn't wrap
    // a line that fitted.)
    t.width = paragraph ? std::max(t.style.size, block.rect.right - block.origin.x + 0.3 * t.style.size) : 0;
  } else {
    t.origin = {block.rect.left, block.origin.y};
    t.width = std::max(t.style.size, block.rect.width() + 0.3 * t.style.size);
    t.origin.x -= 0.15 * t.style.size;
  }
  return t;
}

bool DocumentTab::layoutFor(const TextEditing& e, const RichText& text, pdfz::PlacedText& out) {
  bool complete = true;
  out = layoutRich(text, e.align, e.width, e.lineSpacing, e.style.size, e.origin, e.tabs, &complete);
  if (out.fonts.empty()) {
    QMessageBox::warning(this, tr("Edit Text"), tr("No font on this computer can show that text."));
    return false;
  }
  if (!complete)
    emit statusMessage(tr("Some of the characters aren't in any font that can be put in the PDF, so they won't show."), 6000);
  return true;
}

void DocumentTab::restyleContent(int page, const pdfz::ContentBlock& block, const TextStyleChange& change) {
  if (!doc_ || block.kind != pdfz::ContentBlock::Kind::Text || !block.editable || change.empty()) return;
  TextEditing e = editingFor(page, block);
  RichText text = e.original;
  if (change.align) {
    e.align = *change.align;
    if (e.align != pdfz::TextAlign::Left && e.width <= 0) {  // one line: lined up in its own box
      e.origin.x = block.rect.left - 0.15 * e.style.size;
      e.width = std::max(e.style.size, block.rect.width() + 0.3 * e.style.size);
    }
  }
  TextStyleChange chars = change;
  chars.align.reset();
  if (!chars.empty()) {
    const double from = e.style.size;
    for (RichRun& r : text) applyStyle(r, chars, from);
    if (chars.size && from > 0) {
      e.lineSpacing *= *chars.size / from;
      // The first line's top stays where it was.
      e.origin.y += (*chars.size - from) * 0.8;
    }
    applyStyle(e.style, chars, from);
  }
  pdfz::PlacedText placed;
  if (!layoutFor(e, text, placed)) return;
  if (applyEdit(change.align && chars.empty() ? tr("Align Text") : tr("Change Text Style"), page, Scope::Content,
                [&](pdfz::Document& d) { return d.replaceText(page, block.objects, placed); }))
    view_->selectContentAt(page, {e.origin.x + e.style.size * 0.3, e.origin.y - e.style.size * 0.3});
}

// Like a text box: the block is hidden while it's typed in, so the page shows only the
// editor's text, and the snapshot from before is the undo step.
void DocumentTab::beginContentEdit(int page, pdfz::ContentBlock block) {
  finishTextEditing();
  if (!doc_ || block.kind != pdfz::ContentBlock::Kind::Text || !block.editable) return;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  auto before = pdfz::EditHistory::capture(*doc_, page);
  QApplication::restoreOverrideCursor();
  if (!before.bytes) {
    QMessageBox::warning(this, tr("Edit Text"), tr("Couldn't prepare to undo this change, so it can't be made."));
    return;
  }
  contentEditBefore_ = std::move(before);
  doc_->setContentHidden(page, block.objects, true);
  view_->clearContentSelection();
  refreshContentRendering(page);
  view_->startContentEdit(editingFor(page, block));
}

void DocumentTab::onContentEditFinished(TextEditing e, RichText text) {
  if (!doc_ || e.page < 0 || e.page >= doc_->pageCount()) return;
  const int page = e.page;
  // Line breaks and spaces at the end would only add empty lines.
  while (!text.empty()) {
    QString& last = text.back().text;
    while (!last.isEmpty() && (last.back() == '\n' || last.back() == ' ')) last.chop(1);
    if (!last.isEmpty()) break;
    text.pop_back();
  }
  const bool empty = plainText(text).trimmed().isEmpty();
  const bool unchanged = !e.isNew && sameText(text, e.original) && e.align == e.block.align;

  if (e.isNew && empty) return;
  if (!e.isNew) {
    if (!contentEditBefore_.bytes) return;  // never started
    if (unchanged) {  // shown again as it was
      contentEditBefore_ = {};
      doc_->setContentHidden(page, e.block.objects, false);
      refreshContentRendering(page);
      view_->selectContentObjects(page, e.block.objects);
      return;
    }
  }
  pdfz::PlacedText placed;
  if (!empty && !layoutFor(e, text, placed)) {
    if (!e.isNew) {
      contentEditBefore_ = {};
      doc_->setContentHidden(page, e.block.objects, false);
      refreshContentRendering(page);
    }
    return;
  }
  const pdfz::PointF selectAt{e.origin.x + e.style.size * 0.3, e.origin.y - e.style.size * 0.3};

  if (e.isNew) {
    if (applyEdit(tr("Add Text"), page, Scope::Content, [&](pdfz::Document& d) { return d.replaceText(page, {}, placed); }))
      view_->selectContentAt(page, selectAt);
    return;
  }

  pdfz::EditHistory::Snapshot before = std::move(contentEditBefore_);
  contentEditBefore_ = {};
  const bool ok = empty ? doc_->deleteContent(page, e.block.objects) : doc_->replaceText(page, e.block.objects, placed);
  if (!ok) {
    doc_->setContentHidden(page, e.block.objects, false);
    refreshContentRendering(page);
    QMessageBox::warning(this, tr("Edit Text"), tr("Couldn't change the text."));
    return;
  }
  history_.push(std::move(before), (empty ? tr("Delete Text") : tr("Edit Text")).toStdString());
  refresh(Scope::Content, page, page);
  if (!empty) view_->selectContentAt(page, selectAt);
}
