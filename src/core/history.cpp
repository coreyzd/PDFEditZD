// SPDX-License-Identifier: GPL-3.0-only
#include "core/history.hpp"

namespace pdfz {

namespace {
const std::string kEmpty;
}

EditHistory::EditHistory(std::size_t maxBytes) : maxBytes_(maxBytes) {}

EditHistory::Snapshot EditHistory::capture(const Document& doc, int page) { return {doc.snapshot(), page}; }

void EditHistory::push(Snapshot before, std::string label) {
  if (!before.bytes) return;
  if (cleanPosition_ > position_) cleanPosition_ = -1;  // the saved state was in the redo steps
  redo_.clear();
  undo_.push_back({std::move(before), std::move(label)});
  ++position_;
  trim();
}

const std::string& EditHistory::undoLabel() const { return undo_.empty() ? kEmpty : undo_.back().label; }
const std::string& EditHistory::redoLabel() const { return redo_.empty() ? kEmpty : redo_.back().label; }

EditHistory::Restored EditHistory::undo(const Document& current, int page) {
  return move(undo_, redo_, current, page, -1);
}

EditHistory::Restored EditHistory::redo(const Document& current, int page) {
  return move(redo_, undo_, current, page, +1);
}

EditHistory::Restored EditHistory::move(std::deque<Step>& from, std::deque<Step>& to, const Document& current,
                                        int page, int direction) {
  if (from.empty()) return {};
  Snapshot now{current.snapshot(), page};
  if (!now.bytes) return {};
  auto [doc, err] = Document::openBytes(from.back().state.bytes, current.password());
  if (!doc) return {};
  Restored r{std::move(doc), from.back().state.page};
  // The step keeps its label: undoing "Delete Pages" makes "Delete Pages" the thing to redo.
  to.push_back({std::move(now), std::move(from.back().label)});
  from.pop_back();
  position_ += direction;
  trim();
  return r;
}

std::size_t EditHistory::bytesUsed() const {
  // Snapshots of an unedited document share its bytes; count each buffer once per step anyway
  // (a slight overestimate is the safe side).
  std::size_t total = 0;
  for (const auto* q : {&undo_, &redo_})
    for (const Step& s : *q) total += s.state.bytes ? s.state.bytes->size() : 0;
  return total;
}

void EditHistory::trim() {
  // Oldest undo steps go first, then the furthest redo steps; the newest undo step stays.
  while (bytesUsed() > maxBytes_ && undo_.size() + redo_.size() > 1) {
    if (undo_.size() > 1) {
      undo_.pop_front();
    } else if (!redo_.empty()) {
      redo_.pop_front();
      if (cleanPosition_ > position_) cleanPosition_ = -1;  // may have been in a dropped redo step
    } else {
      break;
    }
  }
}

void EditHistory::clear() {
  undo_.clear();
  redo_.clear();
  position_ = 0;
  cleanPosition_ = 0;
}

}  // namespace pdfz
