#pragma once

#include "core/agent/AppContext.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

class FileAccess;
class LocalCommandRunner;
class ToolRegistry;

// What the office tool needs; owned by the composition root.
struct OfficeServices {
    FileAccess *files{nullptr};                 // where a file may be written (home and /tmp)
    LocalCommandRunner *commands{nullptr};      // LibreOffice headless, for a docx/pptx/xlsx/pdf
    std::function<void(const OpenedApp &)> appOpened;   // LibreOffice is in front now (AppContext)
    QString soffice;                            // the LibreOffice program; empty: looked up on PATH
    QStringList sofficeArguments;               // before the file (tests: a profile of their own)
    bool openFiles{true};                       // false: write the file, never open LibreOffice (tests)
};

// Registers create_document: a text document, a presentation or a spreadsheet, written as a flat
// OpenDocument file (OdfWriter) and opened in LibreOffice Writer, Impress or Calc — or handed to
// LibreOffice first to become a docx, pptx, xlsx or pdf. The model gives the content; nothing is
// typed through the keyboard.
void registerOfficeTools(ToolRegistry &registry, const OfficeServices &services);

} // namespace nexa
