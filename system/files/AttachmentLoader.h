#pragma once

#include "core/agent/Attachments.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>

namespace nexa {

// Prepares the files a request carries, off the GUI thread (Facade over the readers): a picture is read as it is, a
// text file read (and cut at the cap), a document turned into words — PDFs with pdftotext, office files with
// LibreOffice headless — into a scratch folder, and a file outside the home folder copied under it, so read_file can
// reach it. The user's own file is never changed.
class AttachmentLoader : public QObject {
    Q_OBJECT
public:
    explicit AttachmentLoader(QString keepDir = defaultKeepDir(), QObject *parent = nullptr);

    static QString defaultKeepDir();        // ~/.local/share/nexa/attachments
    // The converters found (set in tests to skip them).
    void setConverters(const QString &soffice, const QString &pdftotext);

    // `done` is called on this object's thread, once, with one entry per file, in order.
    void prepare(const QList<Attachment> &files, std::function<void(QList<PreparedAttachment>)> done);

    // A picture pasted from the clipboard becomes a PNG in the keep folder (0600), so it can be attached like a file.
    Result<QString> keepPastedImage(const QImage &image) const;

    // One file, synchronously (the worker calls it; tests may too).
    static PreparedAttachment prepareOne(const Attachment &file, const QString &keepDir, const QString &soffice,
                                         const QString &pdftotext);

private:
    QString keepDir_;
    QString soffice_;
    QString pdftotext_;
};

} // namespace nexa
