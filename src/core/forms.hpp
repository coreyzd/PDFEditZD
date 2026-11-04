// SPDX-License-Identifier: GPL-3.0-only
// Form fields (AcroForm): what's on a page, and filling them in (Document::formFields and the
// setters). PDFium's form layer does the filling, so field formatting and appearances are its,
// and other readers show the values as filled in. Geometry is in the display frame.
#pragma once

#include "core/annotations.hpp"
#include "core/geometry.hpp"

#include <string>
#include <vector>

namespace pdfz {

enum class FieldType { Unknown, PushButton, CheckBox, RadioButton, ComboBox, ListBox, Text, Signature };

// One widget of a field: a field shown in several places (a radio group's buttons, or the same
// field on several pages) has a widget for each, sharing the field's value.
struct FormField {
  int index = -1;  // the widget's annotation index on its page
  FieldType type = FieldType::Unknown;
  std::string name;     // fully qualified, e.g. "applicant.name"
  std::string tooltip;  // /TU, what the form calls the field
  RectF rect;
  std::string value;    // text and choices: the value; check boxes and radio buttons: the field's
  std::string onValue;  // check boxes and radio buttons: this widget's value when it's on
  bool checked = false;
  bool readOnly = false;
  bool required = false;
  bool multiline = false;   // text
  bool password = false;    // text
  bool comb = false;        // text: one character per cell
  bool editable = false;    // combo box: any text, not just the options
  bool multiSelect = false; // list box
  int maxLength = 0;        // text: 0 for no limit
  int alignment = 0;        // 0 left, 1 center, 2 right
  double fontSize = 0;      // 0: sized to fit
  Color textColor{0, 0, 0};
  std::vector<std::string> options;  // choices: the labels shown
  std::vector<bool> selected;        // choices: which are selected

  bool isText() const { return type == FieldType::Text || (type == FieldType::ComboBox && editable); }
  bool isButton() const { return type == FieldType::CheckBox || type == FieldType::RadioButton; }
  bool isChoice() const { return type == FieldType::ComboBox || type == FieldType::ListBox; }
};

}  // namespace pdfz
