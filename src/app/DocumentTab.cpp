// SPDX-License-Identifier: GPL-3.0-only
#include "DocumentTab.hpp"

#include "CommentDialog.hpp"
#include "CommentsPanel.hpp"
#include "DocumentView.hpp"
#include "ImageUtil.hpp"
#include "OfficeImport.hpp"
#include "PageRenderer.hpp"
#include "ThumbnailPanel.hpp"

#include "core/annotations.hpp"
#include "core/document.hpp"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
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

// ---------------------------------------------------------------------------------------------
// Copy, paste and draw order

namespace {
const char* const kContentMime = "application/x-pdfeditzd-content";
// A comment or stamp, as JSON: its fields, and a stamp's drawing (its shapes, and its picture
// as a PNG). It pastes at the same size and place, on any page of any document.
const char* const kAnnotMime = "application/x-pdfeditzd-annotations";

QJsonArray toJson(const pdfz::Color& c) { return {c.r, c.g, c.b}; }
pdfz::Color colorFrom(const QJsonValue& v) {
  const QJsonArray a = v.toArray();
  auto at = [&](int i) { return std::uint8_t(std::clamp(a.at(i).toInt(), 0, 255)); };
  return a.size() == 3 ? pdfz::Color{at(0), at(1), at(2)} : pdfz::Color{};
}
QJsonArray toJson(const pdfz::RectF& r) { return {r.left, r.top, r.right, r.bottom}; }
pdfz::RectF rectFrom(const QJsonValue& v) {
  const QJsonArray a = v.toArray();
  return a.size() == 4 ? pdfz::RectF{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()} : pdfz::RectF{};
}

QJsonObject toJson(const pdfz::Annotation& a, const pdfz::StampDrawing* drawing) {
  QJsonObject o;
  o["kind"] = int(a.kind);
  o["rect"] = toJson(a.rect);
  o["color"] = toJson(a.color);
  o["hasFill"] = a.hasFill;
  o["fill"] = toJson(a.fill);
  o["opacity"] = a.opacity;
  o["borderWidth"] = a.borderWidth;
  QJsonArray ink;
  for (const auto& stroke : a.ink) {
    QJsonArray s;
    for (const pdfz::PointF& p : stroke) s.append(QJsonArray{p.x, p.y});
    ink.append(s);
  }
  o["ink"] = ink;
  o["fontSize"] = a.fontSize;
  o["fontName"] = QString::fromStdString(a.fontName);
  o["textColor"] = toJson(a.textColor);
  o["align"] = a.align;
  o["contents"] = QString::fromStdString(a.contents);
  o["author"] = QString::fromStdString(a.author);
  o["subject"] = QString::fromStdString(a.subject);
  if (drawing) {
    QJsonObject d;
    d["width"] = drawing->width;
    d["height"] = drawing->height;
    QJsonArray paths;
    for (const pdfz::StampDrawing::Path& path : drawing->paths) {
      QJsonArray ops;
      for (const pdfz::StampDrawing::Op& op : path.ops) {
        QJsonArray j{int(op.kind)};
        const int points = op.kind == pdfz::StampDrawing::Op::CubicTo ? 3 : op.kind == pdfz::StampDrawing::Op::Close ? 0 : 1;
        for (int i = 0; i < points; ++i) {
          j.append(op.p[i].x);
          j.append(op.p[i].y);
        }
        ops.append(j);
      }
      paths.append(QJsonObject{{"color", toJson(path.color)}, {"evenOdd", path.evenOdd}, {"ops", ops}});
    }
    d["paths"] = paths;
    if (drawing->imageWidth > 0 && drawing->imageHeight > 0 &&
        drawing->imageBgra.size() == std::size_t(drawing->imageWidth) * drawing->imageHeight * 4) {
      // ARGB32 is BGRA in memory, with straight alpha: the drawing's own layout.
      QImage img(drawing->imageWidth, drawing->imageHeight, QImage::Format_ARGB32);
      for (int y = 0; y < img.height(); ++y)
        std::memcpy(img.scanLine(y), drawing->imageBgra.data() + std::size_t(y) * img.width() * 4, std::size_t(img.width()) * 4);
      QByteArray png;
      QBuffer buf(&png);
      buf.open(QIODevice::WriteOnly);
      img.save(&buf, "PNG");
      d["image"] = QString::fromLatin1(png.toBase64());
    }
    o["drawing"] = d;
  }
  return o;
}

bool fromJson(const QJsonObject& o, pdfz::Annotation& a, pdfz::StampDrawing& drawing, bool& hasDrawing) {
  const int kind = o["kind"].toInt(-1);
  if (kind < 0 || kind > int(pdfz::AnnotKind::Other)) return false;
  a = {};
  a.kind = pdfz::AnnotKind(kind);
  a.rect = rectFrom(o["rect"]);
  if (a.rect.empty()) return false;
  a.color = colorFrom(o["color"]);
  a.hasFill = o["hasFill"].toBool();
  a.fill = colorFrom(o["fill"]);
  a.opacity = std::clamp(o["opacity"].toDouble(1), 0.0, 1.0);
  a.borderWidth = std::max(0.0, o["borderWidth"].toDouble(1));
  for (const QJsonValue& s : o["ink"].toArray()) {
    std::vector<pdfz::PointF> stroke;
    for (const QJsonValue& p : s.toArray()) stroke.push_back({p.toArray().at(0).toDouble(), p.toArray().at(1).toDouble()});
    if (!stroke.empty()) a.ink.push_back(std::move(stroke));
  }
  a.fontSize = o["fontSize"].toDouble(12);
  a.fontName = o["fontName"].toString("Helv").toStdString();
  a.textColor = colorFrom(o["textColor"]);
  a.align = std::clamp(o["align"].toInt(), 0, 2);
  a.contents = o["contents"].toString().toStdString();
  a.author = o["author"].toString().toStdString();
  a.subject = o["subject"].toString().toStdString();
  hasDrawing = o.contains("drawing");
  if (!hasDrawing) return true;
  const QJsonObject d = o["drawing"].toObject();
  drawing = {};
  drawing.width = d["width"].toDouble(1);
  drawing.height = d["height"].toDouble(1);
  if (!(drawing.width > 0) || !(drawing.height > 0)) return false;
  for (const QJsonValue& pv : d["paths"].toArray()) {
    const QJsonObject po = pv.toObject();
    pdfz::StampDrawing::Path path;
    path.color = colorFrom(po["color"]);
    path.evenOdd = po["evenOdd"].toBool();
    for (const QJsonValue& ov : po["ops"].toArray()) {
      const QJsonArray j = ov.toArray();
      const int k = j.at(0).toInt(-1);
      if (k < 0 || k > int(pdfz::StampDrawing::Op::Close)) return false;
      pdfz::StampDrawing::Op op;
      op.kind = pdfz::StampDrawing::Op::Kind(k);
      const int points = op.kind == pdfz::StampDrawing::Op::CubicTo ? 3 : op.kind == pdfz::StampDrawing::Op::Close ? 0 : 1;
      if (j.size() != 1 + 2 * points) return false;
      for (int i = 0; i < points; ++i) op.p[i] = {j[1 + 2 * i].toDouble(), j[2 + 2 * i].toDouble()};
      path.ops.push_back(op);
    }
    drawing.paths.push_back(std::move(path));
  }
  if (d.contains("image")) {
    QImage img;
    if (!img.loadFromData(QByteArray::fromBase64(d["image"].toString().toLatin1()), "PNG")) return false;
    img = img.convertToFormat(QImage::Format_ARGB32);
    drawing.imageWidth = img.width();
    drawing.imageHeight = img.height();
    drawing.imageBgra.resize(std::size_t(img.width()) * img.height() * 4);
    for (int y = 0; y < img.height(); ++y)
      std::memcpy(drawing.imageBgra.data() + std::size_t(y) * img.width() * 4, img.constScanLine(y), std::size_t(img.width()) * 4);
  }
  return !drawing.paths.empty() || !drawing.imageBgra.empty();
}

// Comments that copy as themselves: not the text markups (they belong to the text under them).
bool copiesAsFields(pdfz::AnnotKind k) {
  switch (k) {
    case pdfz::AnnotKind::Note: case pdfz::AnnotKind::FreeText: case pdfz::AnnotKind::Ink:
    case pdfz::AnnotKind::Square: case pdfz::AnnotKind::Circle: case pdfz::AnnotKind::Line:
      return true;
    default:
      return false;
  }
}
}  // namespace

bool DocumentTab::canCopyAnnotation() {
  int page, index;
  if (!doc_ || !view_->selectedAnnotation(page, index)) return false;
  const pdfz::Annotation* a = annotation(page, index);
  return a && (a->kind == pdfz::AnnotKind::Stamp || copiesAsFields(a->kind));
}

bool DocumentTab::copyAnnotation(bool cut) {
  int page, index;
  if (!doc_ || !view_->selectedAnnotation(page, index)) return false;
  finishTextEditing();
  const pdfz::Annotation* found = annotation(page, index);
  if (!found) return false;
  const pdfz::Annotation a = *found;
  pdfz::StampDrawing drawing;
  if (a.kind == pdfz::AnnotKind::Stamp) {
    if (!doc_->stampDrawing(page, index, drawing)) {
      emit statusMessage(tr("This stamp was made by another program and can't be copied."), 4000);
      return true;  // handled: nothing else to copy
    }
  } else if (!copiesAsFields(a.kind)) {
    return false;
  }
  const QJsonObject o{{"version", 1}, {"annotations", QJsonArray{toJson(a, a.kind == pdfz::AnnotKind::Stamp ? &drawing : nullptr)}}};
  auto* mime = new QMimeData;
  mime->setData(kAnnotMime, QJsonDocument(o).toJson(QJsonDocument::Compact));
  if (!a.contents.empty() && a.kind != pdfz::AnnotKind::Stamp) mime->setText(QString::fromStdString(a.contents));
  QGuiApplication::clipboard()->setMimeData(mime);
  if (!cut) {
    emit statusMessage(tr("Copied the %1 (Ctrl+V pastes it at the same size and place)").arg(kindLabel(a).toLower()), 3000);
    return true;
  }
  view_->clearAnnotationSelection();
  if (applyEdit(tr("Cut %1").arg(kindLabel(a)), page, Scope::Annotations,
                [&](pdfz::Document& d) { return d.removeAnnotation(page, index); }))
    emit statusMessage(tr("Cut (Ctrl+V pastes it)"), 3000);
  return true;
}

bool DocumentTab::hasAnnotationsToPaste() {
  const QMimeData* m = QGuiApplication::clipboard()->mimeData();
  return m && m->hasFormat(kAnnotMime);
}

bool DocumentTab::pasteAnnotations(int page) {
  if (!doc_ || page < 0 || page >= doc_->pageCount()) return false;
  finishTextEditing();
  const QMimeData* m = QGuiApplication::clipboard()->mimeData();
  if (!m || !m->hasFormat(kAnnotMime)) return false;
  const QJsonArray list = QJsonDocument::fromJson(m->data(kAnnotMime)).object()["annotations"].toArray();
  if (list.isEmpty()) return false;
  pdfz::Annotation a;
  pdfz::StampDrawing drawing;
  bool hasDrawing = false;
  if (!fromJson(list.first().toObject(), a, drawing, hasDrawing)) return false;
  if (a.kind == pdfz::AnnotKind::Stamp ? !hasDrawing : !copiesAsFields(a.kind)) return false;
  if (a.kind == pdfz::AnnotKind::Stamp && a.subject.empty()) a.subject = "Stamp";
  if (a.author.empty()) a.author = commentAuthor().toStdString();

  // Where it was on its page. Past this page's edge (a smaller page): moved in just far enough,
  // so a stamp in a corner stays in that corner; bigger than the page, in its middle.
  const pdfz::SizeF ps = doc_->pageSize(page);
  auto into = [](double lo, double hi, double size) {
    if (hi - lo > size) return (size - (hi - lo)) / 2 - lo;
    if (lo < 0) return -lo;
    if (hi > size) return size - hi;
    return 0.0;
  };
  double dx = into(a.rect.left, a.rect.right, ps.width), dy = into(a.rect.top, a.rect.bottom, ps.height);
  // Pasted again where one already is (the one copied, say): a little down and right.
  if (const auto existing = annots_.get(page)) {
    auto taken = [&](double x, double y) {
      for (const pdfz::Annotation& b : *existing)
        if (b.kind == a.kind && std::abs(b.rect.left - (a.rect.left + x)) < 0.5 && std::abs(b.rect.top - (a.rect.top + y)) < 0.5 &&
            std::abs(b.rect.width() - a.rect.width()) < 0.5 && std::abs(b.rect.height() - a.rect.height()) < 0.5)
          return true;
      return false;
    };
    for (int i = 0; i < 50 && taken(dx, dy); ++i) {
      dx += 10;
      dy += 10;
    }
  }
  if (dx != 0 || dy != 0) {
    a.rect = {a.rect.left + dx, a.rect.top + dy, a.rect.right + dx, a.rect.bottom + dy};
    for (auto& stroke : a.ink)
      for (pdfz::PointF& p : stroke) p = {p.x + dx, p.y + dy};
  }
  if (a.kind == pdfz::AnnotKind::FreeText) a.appearance = freeTextAppearance(a, doc_->pageFrame(page));

  int added = -1;
  if (!applyEdit(tr("Paste %1").arg(kindLabel(a)), page, Scope::Annotations, [&](pdfz::Document& d) {
        added = a.kind == pdfz::AnnotKind::Stamp ? d.addStamp(page, a, drawing) : d.addAnnotation(page, a);
        return added >= 0;
      })) {
    emit statusMessage(tr("That couldn't be pasted."), 3000);
    return true;
  }
  view_->selectAnnotation(page, added, false);
  return true;
}

void DocumentTab::copyContent(bool cut) {
  if (!doc_) return;
  const int page = view_->contentSelectionPage();
  const std::vector<int> objects = view_->contentSelectionObjects();
  if (page < 0 || objects.empty()) return;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const pdfz::Bytes copied = doc_->copyContent(page, objects);
  auto* mime = new QMimeData;
  if (copied) {
    mime->setData(kContentMime, QByteArray(reinterpret_cast<const char*>(copied->data()), qsizetype(copied->size())));
    // A picture of it for other programs (at 200 DPI), cut to what was copied.
    auto [clip, err] = pdfz::Document::openBytes(copied);
    if (clip) {
      pdfz::RectF r;
      for (const pdfz::ContentBlock& b : clip->contentBlocks(0)) r = r.united(b.rect);
      const double scale = 200.0 / 72;
      if (!r.empty() && r.width() * scale < 8000 && r.height() * scale < 8000) {
        const pdfz::PixelRect px{int(std::floor(r.left * scale)), int(std::floor(r.top * scale)),
                                 std::max(1, int(std::ceil(r.width() * scale))), std::max(1, int(std::ceil(r.height() * scale)))};
        mime->setImageData(toQImage(clip->render(0, scale, px, pdfz::RenderForPrinting)).copy());
      }
    }
  }
  const QString text = view_->contentSelectionText();
  if (!text.isEmpty()) mime->setText(text);
  QApplication::restoreOverrideCursor();
  if (mime->formats().isEmpty()) {
    delete mime;
    emit statusMessage(tr("That couldn't be copied."), 3000);
    return;
  }
  QGuiApplication::clipboard()->setMimeData(mime);
  if (!cut) {
    emit statusMessage(tr("Copied"), 2000);
    return;
  }
  view_->clearContentSelection();
  if (applyEdit(tr("Cut"), page, Scope::Content, [&](pdfz::Document& d) { return d.deleteContent(page, objects); }))
    emit statusMessage(tr("Cut (Ctrl+V pastes it)"), 3000);
}

bool DocumentTab::canPaste() {
  const QMimeData* m = QGuiApplication::clipboard()->mimeData();
  return m && (m->hasFormat(kContentMime) || m->hasFormat(kAnnotMime) || m->hasImage());
}

bool DocumentTab::pasteContent(int page) {
  if (!doc_ || page < 0 || page >= doc_->pageCount()) return false;
  finishTextEditing();
  const QMimeData* m = QGuiApplication::clipboard()->mimeData();
  if (!m) return false;
  if (m->hasFormat(kContentMime)) {
    const QByteArray copied = m->data(kContentMime);
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(copied.begin(), copied.end());
    std::vector<int> pasted;
    if (!applyEdit(tr("Paste"), page, Scope::Content, [&](pdfz::Document& d) {
          pasted = d.pasteContent(page, bytes);
          return !pasted.empty();
        })) {
      emit statusMessage(tr("That couldn't be pasted."), 3000);
      return false;
    }
    view_->selectContentObjects(page, pasted);
    return true;
  }
  if (m->hasImage()) {
    // A picture from another program: in the middle of what's showing, at most three inches.
    const QImage img = qvariant_cast<QImage>(m->imageData());
    if (img.isNull()) return false;
    int at = page;
    pdfz::PointF c;
    if (!view_->visibleCenter(at, c) || at != page) {
      const pdfz::SizeF ps = doc_->pageSize(page);
      c = {ps.width / 2, ps.height / 2};
    }
    const pdfz::SizeF ps = doc_->pageSize(page);
    const double maxSide = std::min({216.0, ps.width * 0.9, ps.height * 0.9});
    const double k = maxSide / std::max(img.width(), img.height());
    const double w = img.width() * k, h = img.height() * k;
    const double x = std::clamp(c.x - w / 2, 0.0, std::max(0.0, ps.width - w));
    const double y = std::clamp(c.y - h / 2, 0.0, std::max(0.0, ps.height - h));
    addContentImage(page, pageImageFrom(img), pdfz::RectF{x, y, x + w, y + h});
    return true;
  }
  return false;
}

void DocumentTab::arrangeContent(pdfz::Arrange how) {
  if (!doc_) return;
  const int page = view_->contentSelectionPage();
  const std::vector<int> objects = view_->contentSelectionObjects();
  if (page < 0 || objects.empty()) return;
  QString label;
  switch (how) {
    case pdfz::Arrange::ToFront: label = tr("Bring to Front"); break;
    case pdfz::Arrange::ToBack: label = tr("Send to Back"); break;
    case pdfz::Arrange::Forward: label = tr("Bring Forward"); break;
    case pdfz::Arrange::Backward: label = tr("Send Backward"); break;
  }
  std::vector<int> now;
  if (applyEdit(label, page, Scope::Content, [&](pdfz::Document& d) {
        now = d.arrangeContent(page, objects, how);
        return !now.empty();
      }))
    view_->selectContentObjects(page, now);
}
