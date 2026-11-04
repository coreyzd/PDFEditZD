// SPDX-License-Identifier: GPL-3.0-only
#include <doctest.h>

#include "core/annotations.hpp"
#include "core/document.hpp"
#include "core/forms.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using pdfz::Document;
using pdfz::FieldType;
using pdfz::FormField;

namespace {

// A one-page form (300 x 400 pt), written out here so the test needs no files: a text field, a
// multi-line one, a read-only one, a check box, a radio group of two, a combo box and a list
// box. Text and choice fields have no appearances (NeedAppearances), as many forms are made.
std::vector<std::uint8_t> formPdf() {
  const char* box = "q 0 0 1 rg 2 2 10 10 re f Q";  // the check box's and radio buttons' "on" look
  std::vector<std::string> objs = {
      "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R 5 0 R 6 0 R 7 0 R 8 0 R 11 0 R 12 0 R] "
      "/NeedAppearances true /DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv 13 0 R >> >> >> >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Annots [4 0 R 5 0 R 6 0 R 7 0 R 9 0 R 10 0 R 11 0 R "
      "12 0 R] /Resources << >> >>",
      "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /TU (Your name) /Rect [20 350 200 370] /F 4 /P 3 0 R "
      "/DA (/Helv 12 Tf 0 g) /MaxLen 30 >>",
      "<< /Type /Annot /Subtype /Widget /FT /Tx /T (notes) /Ff 4096 /Rect [20 250 280 330] /F 4 /P 3 0 R "
      "/DA (/Helv 10 Tf 0 g) >>",
      "<< /Type /Annot /Subtype /Widget /FT /Tx /T (cc) /Ff 1 /V (CC-1) /Rect [20 220 120 240] /F 4 /P 3 0 R "
      "/DA (/Helv 10 Tf 0 g) >>",
      "<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off /Rect [20 190 34 204] /F 4 /P 3 0 R "
      "/MK << >> /AP << /N << /Yes 14 0 R /Off 15 0 R >> >> >>",
      "<< /FT /Btn /Ff 49152 /T (ship) /V /site /Kids [9 0 R 10 0 R] >>",
      "<< /Type /Annot /Subtype /Widget /Parent 8 0 R /AS /Off /Rect [20 160 34 174] /F 4 /P 3 0 R "
      "/AP << /N << /office 14 0 R /Off 15 0 R >> >> >>",
      "<< /Type /Annot /Subtype /Widget /Parent 8 0 R /AS /site /Rect [60 160 74 174] /F 4 /P 3 0 R "
      "/AP << /N << /site 14 0 R /Off 15 0 R >> >> >>",
      "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (dept) /Opt [(Engineering) (Finance) (Facilities)] "
      "/V (Finance) /Rect [20 120 160 140] /F 4 /P 3 0 R /DA (/Helv 10 Tf 0 g) >>",
      "<< /Type /Annot /Subtype /Widget /FT /Ch /T (color) /Opt [(Red) (Green) (Blue)] /Rect [180 80 280 140] "
      "/F 4 /P 3 0 R /DA (/Helv 10 Tf 0 g) >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
      "<< /Type /XObject /Subtype /Form /BBox [0 0 14 14] /Length " + std::to_string(std::strlen(box)) +
          " >>\nstream\n" + box + "\nendstream",
      "<< /Type /XObject /Subtype /Form /BBox [0 0 14 14] /Length 0 >>\nstream\n\nendstream",
  };
  std::string pdf = "%PDF-1.7\n";
  std::vector<std::size_t> offsets;
  for (std::size_t i = 0; i < objs.size(); ++i) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
  }
  const std::size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
  for (std::size_t o : offsets) {
    char line[32];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
    pdf += line;
  }
  pdf += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::unique_ptr<Document> openForm() {
  auto [doc, err] = Document::openMemory(formPdf());
  REQUIRE(doc);
  return std::move(doc);
}

std::unique_ptr<Document> reopened(const Document& d) {
  auto [doc, err] = Document::openMemory(d.saveToMemory());
  REQUIRE(doc);
  return std::move(doc);
}

const FormField* field(const std::vector<FormField>& fs, const std::string& name, int nth = 0) {
  for (const FormField& f : fs)
    if (f.name == name && nth-- == 0) return &f;
  return nullptr;
}

// Pixels that aren't white in a display rect, rendered at 1 px per point: does the page show
// something there?
int inkIn(const Document& doc, const pdfz::RectF& r) {
  const auto bmp = doc.render(0, 1.0, {0, 0, 300, 400});
  int n = 0;
  for (int y = int(r.top); y < int(r.bottom); ++y)
    for (int x = int(r.left); x < int(r.right); ++x)
      if ((bmp.pixel(x, y) & 0xFFFFFF) != 0xFFFFFF) ++n;
  return n;
}

}  // namespace

TEST_CASE("reading a form's fields") {
  auto doc = openForm();
  CHECK(doc->hasForm());
  const auto fs = doc->formFields(0);
  REQUIRE(fs.size() == 8);  // two widgets for the radio group

  const FormField* name = field(fs, "name");
  REQUIRE(name);
  CHECK(name->type == FieldType::Text);
  CHECK(name->tooltip == "Your name");
  CHECK(name->maxLength == 30);
  CHECK(name->fontSize == doctest::Approx(12));
  CHECK(!name->readOnly);
  // Display frame: y down from the page's top.
  CHECK(name->rect.left == doctest::Approx(20));
  CHECK(name->rect.top == doctest::Approx(30));
  CHECK(name->rect.bottom == doctest::Approx(50));

  CHECK(field(fs, "notes")->multiline);
  CHECK(field(fs, "cc")->readOnly);
  CHECK(field(fs, "cc")->value == "CC-1");

  const FormField* agree = field(fs, "agree");
  REQUIRE(agree);
  CHECK(agree->type == FieldType::CheckBox);
  CHECK(!agree->checked);
  CHECK(agree->onValue == "Yes");

  const FormField* office = field(fs, "ship", 0);
  const FormField* site = field(fs, "ship", 1);
  REQUIRE(office);
  REQUIRE(site);
  CHECK(office->type == FieldType::RadioButton);
  CHECK(!office->checked);
  CHECK(site->checked);
  CHECK(site->value == "site");

  const FormField* dept = field(fs, "dept");
  REQUIRE(dept);
  CHECK(dept->type == FieldType::ComboBox);
  CHECK(dept->options == std::vector<std::string>{"Engineering", "Finance", "Facilities"});
  CHECK(dept->selected == std::vector<bool>{false, true, false});
  CHECK(field(fs, "color")->type == FieldType::ListBox);

  // A document without a form has no fields.
  auto plain = Document::create();
  REQUIRE(plain->insertBlankPage(0, {100, 100}));
  CHECK(!plain->hasForm());
  CHECK(plain->formFields(0).empty());
}

TEST_CASE("filling in a form, and the values surviving a save") {
  auto doc = openForm();
  auto fs = doc->formFields(0);
  CHECK(inkIn(*doc, field(fs, "name")->rect) == 0);
  REQUIRE(doc->setFieldText(0, field(fs, "name")->index, "Corey"));
  REQUIRE(doc->setFieldText(0, field(fs, "notes")->index, "first line\nsecond line"));
  REQUIRE(doc->setFieldChecked(0, field(fs, "agree")->index, true));
  REQUIRE(doc->setFieldChecked(0, field(fs, "ship", 0)->index, true));
  REQUIRE(doc->setFieldChoice(0, field(fs, "dept")->index, 2));
  REQUIRE(doc->setFieldChoice(0, field(fs, "color")->index, 1));
  CHECK(doc->modified());

  // Read-only fields can't be filled in; a radio button can't be turned off by itself.
  CHECK(!doc->setFieldText(0, field(fs, "cc")->index, "hacked"));
  CHECK(!doc->setFieldChecked(0, field(fs, "ship", 0)->index, false));
  // Setting the wrong kind of value fails.
  CHECK(!doc->setFieldChecked(0, field(fs, "name")->index, true));
  CHECK(!doc->setFieldChoice(0, field(fs, "dept")->index, 7));

  auto saved = reopened(*doc);
  for (const Document* d : {doc.get(), saved.get()}) {
    fs = d->formFields(0);
    CHECK(field(fs, "name")->value == "Corey");
    CHECK(field(fs, "notes")->value == "first line\nsecond line");
    CHECK(field(fs, "cc")->value == "CC-1");
    CHECK(field(fs, "agree")->checked);
    CHECK(field(fs, "ship", 0)->checked);
    CHECK(!field(fs, "ship", 1)->checked);
    CHECK(field(fs, "dept")->value == "Facilities");
    CHECK(field(fs, "color")->selected == std::vector<bool>{false, true, false});
    // The filled-in text is drawn, and so is the checked box.
    CHECK(inkIn(*d, field(fs, "name")->rect) > 20);
    CHECK(inkIn(*d, field(fs, "agree")->rect) > 20);
  }
  // Tiles show the fields where the whole page does (the form layer is drawn separately).
  const auto whole = saved->render(0, 2.0, {0, 0, 600, 800});
  const auto tile = saved->render(0, 2.0, {30, 50, 256, 256});
  int differ = 0;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x)
      if (tile.pixel(x, y) != whole.pixel(x + 30, y + 50)) ++differ;
  CHECK(differ < 50);
  // Unchecking a check box.
  REQUIRE(saved->setFieldChecked(0, field(saved->formFields(0), "agree")->index, false));
  CHECK(!field(saved->formFields(0), "agree")->checked);
}

TEST_CASE("form fields on pages that were moved or added") {
  auto doc = openForm();
  REQUIRE(doc->insertBlankPage(0, {300, 400}));
  CHECK(doc->formFields(0).empty());
  auto fs = doc->formFields(1);
  REQUIRE(fs.size() == 8);
  REQUIRE(doc->setFieldText(1, field(fs, "name")->index, "after insert"));
  CHECK(field(doc->formFields(1), "name")->value == "after insert");
}

TEST_CASE("comments on a form field are drawn over it") {
  auto doc = openForm();
  auto fs = doc->formFields(0);
  REQUIRE(doc->setFieldText(0, field(fs, "name")->index, "WWWWWWWWWWWWWWWW"));
  pdfz::Annotation sq;
  sq.kind = pdfz::AnnotKind::Square;
  sq.rect = {30, 32, 90, 48};  // inside the name field, over its text
  sq.color = {0, 160, 0};
  sq.hasFill = true;
  sq.fill = {0, 160, 0};
  REQUIRE(doc->addAnnotation(0, sq) >= 0);
  const auto bmp = doc->render(0, 1.0, {0, 0, 300, 400});
  int green = 0, dark = 0;
  for (int y = 34; y < 46; ++y) {
    for (int x = 32; x < 88; ++x)
      if (bmp.pixel(x, y) == 0xFF00A000u) ++green;
    for (int x = 100; x < 190; ++x)  // the field's text beside it still shows
      if ((bmp.pixel(x, y) & 0xFF) < 100) ++dark;
  }
  CHECK(green > 600);
  CHECK(dark > 20);
}

TEST_CASE("extracted pages keep their form fields") {
  auto doc = openForm();
  REQUIRE(doc->insertBlankPage(0, {300, 400}));
  REQUIRE(doc->setFieldText(1, field(doc->formFields(1), "name")->index, "kept"));
  auto part = doc->extract({1});
  REQUIRE(part);
  CHECK(part->hasForm());
  const auto fs = part->formFields(0);
  CHECK(fs.size() == 8);
  CHECK(field(fs, "name")->value == "kept");
}

TEST_CASE("saving flattened") {
  auto doc = openForm();
  auto fs = doc->formFields(0);
  REQUIRE(doc->setFieldText(0, field(fs, "name")->index, "Flat"));
  pdfz::Annotation note;
  note.kind = pdfz::AnnotKind::Square;
  note.rect = {150, 340, 250, 390};
  note.color = {255, 0, 0};
  note.borderWidth = 3;
  REQUIRE(doc->addAnnotation(0, note) >= 0);

  const auto path = std::filesystem::temp_directory_path() / "pdfz_flattened_test.pdf";
  REQUIRE(doc->saveFlattened(path));
  auto [flat, err] = Document::open(path);
  REQUIRE(flat);
  std::filesystem::remove(path);
  // No fields or comments left, but the page shows them.
  CHECK(!flat->hasForm());
  CHECK(flat->formFields(0).empty());
  CHECK(flat->annotations(0).empty());
  CHECK(inkIn(*flat, {20, 30, 200, 50}) > 20);
  // This document is still editable.
  CHECK(doc->formFields(0).size() == 8);
  CHECK(doc->annotations(0).size() == 1);
}
