#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"

#include <QList>
#include <QString>

#include <optional>

namespace nexa {

// A file the user gives the model with a request (value): what it is. Pictures go to the model as pictures, words as
// words (a text file as it is, a document converted), and anything else by its path, for read_file.
struct Attachment {
    enum class Kind { Image, Text, Document, Other };
    QString path;
    QString name;
    qint64 size{0};
    QString mimeType;
    Kind kind{Kind::Other};
};

// The same file, prepared (AttachmentLoader): what the model is given of it.
struct PreparedAttachment {
    Attachment file;
    QString text;                           // its words, cut at kInlineBytes
    std::optional<ImageAttachment> image;   // a picture, for a model that sees
    QString note;                           // why there are no words, or that they were cut
    QString pathForTools;                   // where read_file reaches it (a copy in the home folder when it was elsewhere)
    bool truncated{false};
};

namespace attachments {

inline constexpr qint64 kInlineBytes = 200 * 1024;          // of words per file, in the request itself
inline constexpr qint64 kImageBytes = 8 * 1024 * 1024;
inline constexpr int kMaxFiles = 10;

// A file that exists and can be read, with its kind from what it is; a folder, a missing or unreadable file: an error.
Result<Attachment> describe(const QString &path);
Attachment::Kind kindOf(const QString &mimeType, const QString &name);
QString sizeInWords(qint64 bytes);                          // "12 KB", "1.4 MB"

// The request as the model reads it: the user's words first, then every file as labelled data — the model is told
// they are the file's contents, never instructions — each between lines that name it.
QString promptWith(const QString &userText, const QList<PreparedAttachment> &files);
QString block(const PreparedAttachment &file);
QList<ImageAttachment> imagesOf(const QList<PreparedAttachment> &files);
// What the chat shows under the user's words: "📎 report.pdf (1.2 MB) · notes.txt (3 KB)".
QString summary(const QList<PreparedAttachment> &files);

} // namespace attachments

} // namespace nexa
