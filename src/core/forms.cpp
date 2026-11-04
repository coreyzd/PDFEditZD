// SPDX-License-Identifier: GPL-3.0-only
#include "core/forms.hpp"

#include "core/document.hpp"
#include "core/engine.hpp"

#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>

namespace pdfz {

namespace detail {
std::u16string toUtf16(const std::string& s);
std::string toUtf8(const std::u16string& s);
PageFrame frameOfPage(void* page);
}  // namespace detail

namespace {

FPDF_FORMHANDLE asForm(void* f) { return static_cast<FPDF_FORMHANDLE>(f); }

// Field flags (/Ff), PDF 32000-1 tables 221, 226, 228, 230.
constexpr int kReadOnly = 1 << 0;
constexpr int kRequired = 1 << 1;
constexpr int kMultiline = 1 << 12;
constexpr int kPassword = 1 << 13;
constexpr int kEdit = 1 << 18;
constexpr int kMultiSelect = 1 << 21;
constexpr int kComb = 1 << 24;

struct Annot {
  FPDF_ANNOTATION a = nullptr;
  explicit Annot(FPDF_ANNOTATION h) : a(h) {}
  ~Annot() {
    if (a) FPDFPage_CloseAnnot(a);
  }
  Annot(const Annot&) = delete;
  Annot& operator=(const Annot&) = delete;
};

// A string from one of PDFium's "buffer and length in bytes" UTF-16 getters.
template <typename Get>
std::string utf16From(Get&& get) {
  const unsigned long bytes = get(nullptr, 0);
  if (bytes <= 2) return {};
  std::u16string buf(bytes / 2, u'\0');
  get(reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
  buf.resize(bytes / 2 - 1);  // the terminator
  return detail::toUtf8(buf);
}

FieldType typeOf(int t) {
  switch (t) {
    case FPDF_FORMFIELD_PUSHBUTTON: return FieldType::PushButton;
    case FPDF_FORMFIELD_CHECKBOX: return FieldType::CheckBox;
    case FPDF_FORMFIELD_RADIOBUTTON: return FieldType::RadioButton;
    case FPDF_FORMFIELD_COMBOBOX: return FieldType::ComboBox;
    case FPDF_FORMFIELD_LISTBOX: return FieldType::ListBox;
    case FPDF_FORMFIELD_TEXTFIELD: return FieldType::Text;
    case FPDF_FORMFIELD_SIGNATURE: return FieldType::Signature;
    default: return FieldType::Unknown;
  }
}

RectF displayRect(FPDF_ANNOTATION a, const PageFrame& f) {
  FS_RECTF r{};
  if (!FPDFAnnot_GetRect(a, &r)) return {};
  return f.toDisplay(RectF{std::min(r.left, r.right), std::min(r.top, r.bottom), std::max(r.left, r.right),
                           std::max(r.top, r.bottom)});
}

}  // namespace

bool Document::hasForm() const { return hasForm_; }

std::vector<FormField> Document::formFields(int pageIndex) const {
  std::vector<FormField> out;
  if (!hasForm_ || pageIndex < 0 || pageIndex >= pageCount()) return out;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return out;
  FPDF_FORMHANDLE form = asForm(form_);
  const PageFrame frame = detail::frameOfPage(page);
  const int n = FPDFPage_GetAnnotCount(page);
  for (int i = 0; i < n; ++i) {
    Annot h(FPDFPage_GetAnnot(page, i));
    if (!h.a || FPDFAnnot_GetSubtype(h.a) != FPDF_ANNOT_WIDGET) continue;
    FormField f;
    f.index = i;
    f.type = typeOf(FPDFAnnot_GetFormFieldType(form, h.a));
    if (f.type == FieldType::Unknown) continue;
    f.name = utf16From([&](FPDF_WCHAR* b, unsigned long len) { return FPDFAnnot_GetFormFieldName(form, h.a, b, len); });
    f.tooltip = utf16From([&](FPDF_WCHAR* b, unsigned long len) { return FPDFAnnot_GetFormFieldAlternateName(form, h.a, b, len); });
    f.value = utf16From([&](FPDF_WCHAR* b, unsigned long len) { return FPDFAnnot_GetFormFieldValue(form, h.a, b, len); });
    // Line breaks as \n, whatever the file has (PDFium writes \r\n).
    for (std::size_t at = 0; (at = f.value.find('\r', at)) != std::string::npos;)
      f.value.replace(at, at + 1 < f.value.size() && f.value[at + 1] == '\n' ? 2 : 1, "\n");
    f.rect = displayRect(h.a, frame);
    const int ff = FPDFAnnot_GetFormFieldFlags(form, h.a);
    // A widget can also be made read-only by its annotation flags.
    f.readOnly = (ff & kReadOnly) != 0 || (FPDFAnnot_GetFlags(h.a) & FPDF_ANNOT_FLAG_READONLY) != 0;
    f.required = (ff & kRequired) != 0;
    if (f.type == FieldType::Text) {
      f.multiline = (ff & kMultiline) != 0;
      f.password = (ff & kPassword) != 0;
      f.comb = (ff & kComb) != 0;
      float maxLen = 0;
      if (FPDFAnnot_GetNumberValue(h.a, "MaxLen", &maxLen)) f.maxLength = std::max(0, int(maxLen));
    }
    if (f.isButton()) {
      f.checked = FPDFAnnot_IsChecked(form, h.a);
      f.onValue = utf16From([&](FPDF_WCHAR* b, unsigned long len) { return FPDFAnnot_GetFormFieldExportValue(form, h.a, b, len); });
    }
    if (f.isChoice()) {
      f.editable = f.type == FieldType::ComboBox && (ff & kEdit) != 0;
      f.multiSelect = f.type == FieldType::ListBox && (ff & kMultiSelect) != 0;
      const int count = FPDFAnnot_GetOptionCount(form, h.a);
      for (int o = 0; o < count; ++o) {
        f.options.push_back(utf16From([&](FPDF_WCHAR* b, unsigned long len) { return FPDFAnnot_GetOptionLabel(form, h.a, o, b, len); }));
        f.selected.push_back(FPDFAnnot_IsOptionSelected(form, h.a, o));
      }
    }
    float q = 0;
    if (FPDFAnnot_GetNumberValue(h.a, "Q", &q)) f.alignment = std::clamp(int(q), 0, 2);
    float size = 0;
    if (FPDFAnnot_GetFontSize(form, h.a, &size)) f.fontSize = size;
    unsigned int r = 0, g = 0, b = 0;
    if (FPDFAnnot_GetFontColor(form, h.a, &r, &g, &b)) f.textColor = {std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)};
    out.push_back(std::move(f));
  }
  return out;
}

namespace {

// The field's flags, if it's a widget of the expected kind that may be changed; -1 otherwise.
int writableField(FPDF_FORMHANDLE form, FPDF_ANNOTATION a) {
  if (!a || FPDFAnnot_GetSubtype(a) != FPDF_ANNOT_WIDGET) return -1;
  const int ff = FPDFAnnot_GetFormFieldFlags(form, a);
  if ((ff & kReadOnly) || (FPDFAnnot_GetFlags(a) & FPDF_ANNOT_FLAG_READONLY)) return -1;
  return ff;
}

}  // namespace

bool Document::setFieldText(int pageIndex, int index, const std::string& text) {
  if (!hasForm_ || pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  FPDF_FORMHANDLE form = asForm(form_);
  Annot h(FPDFPage_GetAnnot(page, index));
  if (writableField(form, h.a) < 0) return false;
  const int type = FPDFAnnot_GetFormFieldType(form, h.a);
  if (type != FPDF_FORMFIELD_TEXTFIELD && type != FPDF_FORMFIELD_COMBOBOX) return false;
  // As if typed: focus the field, replace all its text, and leave it, which commits the value
  // (with the field's own formatting) and has PDFium draw its appearance.
  if (!FORM_SetFocusedAnnot(form, h.a)) return false;
  FORM_SelectAllText(form, page);
  // PDFium takes line breaks as \r in multi-line fields.
  std::string t = text;
  std::string unified;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '\r' && i + 1 < t.size() && t[i + 1] == '\n') continue;
    unified += t[i] == '\n' ? '\r' : t[i];
  }
  const std::u16string w = detail::toUtf16(unified);
  FORM_ReplaceSelection(form, page, reinterpret_cast<FPDF_WIDESTRING>(w.c_str()));
  FORM_ForceToKillFocus(form);
  markEdited();
  return true;
}

bool Document::setFieldChecked(int pageIndex, int index, bool checked) {
  if (!hasForm_ || pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  FPDF_FORMHANDLE form = asForm(form_);
  Annot h(FPDFPage_GetAnnot(page, index));
  if (writableField(form, h.a) < 0) return false;
  const int type = FPDFAnnot_GetFormFieldType(form, h.a);
  if (type != FPDF_FORMFIELD_CHECKBOX && type != FPDF_FORMFIELD_RADIOBUTTON) return false;
  if (bool(FPDFAnnot_IsChecked(form, h.a)) == checked) return true;
  if (type == FPDF_FORMFIELD_RADIOBUTTON && !checked) return false;  // turned off by choosing another
  // A click in the middle of the button: PDFium's own toggling, which keeps the group's other
  // buttons and the appearance states (/AS) consistent.
  FS_RECTF r{};
  if (!FPDFAnnot_GetRect(h.a, &r)) return false;
  const double x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
  FORM_OnLButtonDown(form, page, 0, x, y);
  FORM_OnLButtonUp(form, page, 0, x, y);
  FORM_ForceToKillFocus(form);
  markEdited();
  return bool(FPDFAnnot_IsChecked(form, h.a)) == checked;
}

bool Document::setFieldChoice(int pageIndex, int index, int option) {
  if (!hasForm_ || pageIndex < 0 || pageIndex >= pageCount()) return false;
  std::lock_guard lock(engineMutex());
  auto page = static_cast<FPDF_PAGE>(cachedPage(pageIndex));
  if (!page) return false;
  FPDF_FORMHANDLE form = asForm(form_);
  Annot h(FPDFPage_GetAnnot(page, index));
  const int ff = writableField(form, h.a);
  if (ff < 0) return false;
  const int type = FPDFAnnot_GetFormFieldType(form, h.a);
  if (type != FPDF_FORMFIELD_COMBOBOX && type != FPDF_FORMFIELD_LISTBOX) return false;
  const int count = FPDFAnnot_GetOptionCount(form, h.a);
  if (option < 0 || option >= count) return false;
  if (!FORM_SetFocusedAnnot(form, h.a)) return false;
  bool ok = true;
  if (type == FPDF_FORMFIELD_LISTBOX && (ff & kMultiSelect))
    for (int o = 0; o < count; ++o)
      if (o != option && FPDFAnnot_IsOptionSelected(form, h.a, o)) FORM_SetIndexSelected(form, page, o, false);
  ok = FORM_SetIndexSelected(form, page, option, true);
  FORM_ForceToKillFocus(form);
  markEdited();
  return ok;
}

}  // namespace pdfz
