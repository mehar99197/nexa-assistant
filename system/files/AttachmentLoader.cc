#include "system/files/AttachmentLoader.h"

#include "core/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include "core/config/Portable.h"
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

bool insideHomeOrTmp(const QString &path) {
    const QString clean = QDir::cleanPath(path);
    const QString home = QDir::cleanPath(QDir::homePath());
    const QString temp = QDir::cleanPath(QDir::tempPath());
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif
    const auto underRoot = [kPathCase](const QString &candidate, const QString &root) {
        if (root.isEmpty()) return false;
        const QString prefix = root.endsWith(u'/') ? root : root + u'/';
        return candidate.compare(root, kPathCase) == 0 || candidate.startsWith(prefix, kPathCase);
    };
    return underRoot(clean, home) || underRoot(clean, temp)
#ifndef Q_OS_WIN
        || clean.startsWith(u"/tmp/"_s)
#endif
        ;
}

// A copy under the home folder (0600), so the tools confined there can read it; the name says when and what.
QString keepCopy(const QString &path, const QString &name, const QString &keepDir) {
    if (!QDir().mkpath(keepDir)) return {};
#ifdef Q_OS_UNIX
    ::chmod(QFile::encodeName(keepDir).constData(), 0700);
#endif
    const QString target = u"%1/%2-%3"_s.arg(keepDir, QDateTime::currentDateTime().toString(u"yyyyMMdd-HHmmss"_s), name);
    if (QFile::exists(target) && !QFile::remove(target)) return {};
    if (!QFile::copy(path, target)) return {};
    QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return target;
}

QString readWords(const QString &path, bool *truncated) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.read(attachments::kInlineBytes + 1);
    *truncated = bytes.size() > attachments::kInlineBytes;
    QString text = QString::fromUtf8(bytes.left(attachments::kInlineBytes));
    if (text.contains(QChar::ReplacementCharacter)) text = QString::fromLatin1(bytes.left(attachments::kInlineBytes));
    return text;
}

// Runs a converter into `outDir` and returns the one file it wrote there.
QString convert(const QString &program, const QStringList &arguments, const QString &outDir) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForFinished(60'000)) {
        process.kill();
        return {};
    }
    const QStringList made = QDir(outDir).entryList(QDir::Files, QDir::Name);
    return made.isEmpty() ? QString() : outDir + u'/' + made.first();
}

} // namespace

AttachmentLoader::AttachmentLoader(QString keepDir, QObject *parent) : QObject(parent), keepDir_(std::move(keepDir)) {
    QStringList extraDirs;
#ifdef Q_OS_WIN
    for (const QString &base : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"),
                                qEnvironmentVariable("LOCALAPPDATA") + u"/Programs"_s}) {
        if (!base.isEmpty()) extraDirs.append(base + u"/LibreOffice/program"_s);
    }
#endif
    for (const QString &name : {u"soffice"_s, u"libreoffice"_s}) {
        QString found = QStandardPaths::findExecutable(name);
        if (found.isEmpty() && !extraDirs.isEmpty()) found = QStandardPaths::findExecutable(name, extraDirs);
        if (!found.isEmpty()) {
            soffice_ = found;
            break;
        }
    }
    pdftotext_ = QStandardPaths::findExecutable(u"pdftotext"_s);
}

QString AttachmentLoader::defaultKeepDir() {
    return portable::dataDir() + u"/attachments"_s;
}

void AttachmentLoader::setConverters(const QString &soffice, const QString &pdftotext) {
    soffice_ = soffice;
    pdftotext_ = pdftotext;
}

Result<QString> AttachmentLoader::keepPastedImage(const QImage &image) const {
    if (image.isNull()) return Error{ErrorCode::InvalidArgument, u"The clipboard holds no picture."_s};
    if (!QDir().mkpath(keepDir_)) return Error{ErrorCode::Io, u"Cannot make %1."_s.arg(keepDir_)};
#ifdef Q_OS_UNIX
    ::chmod(QFile::encodeName(keepDir_).constData(), 0700);
#endif
    const QString path = u"%1/pasted-%2.png"_s.arg(keepDir_, QDateTime::currentDateTime().toString(u"yyyyMMdd-HHmmss-zzz"_s));
    if (!image.save(path, "PNG")) return Error{ErrorCode::Io, u"Cannot write %1."_s.arg(path)};
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return path;
}

void AttachmentLoader::prepare(const QList<Attachment> &files, std::function<void(QList<PreparedAttachment>)> done) {
    const QPointer<AttachmentLoader> self(this);
    const QString keepDir = keepDir_;
    const QString soffice = soffice_;
    const QString pdftotext = pdftotext_;
    QThreadPool::globalInstance()->start([self, files, done = std::move(done), keepDir, soffice, pdftotext] {
        QList<PreparedAttachment> prepared;
        for (const Attachment &file : files) prepared.append(prepareOne(file, keepDir, soffice, pdftotext));
        if (!self) return;
        QMetaObject::invokeMethod(self, [done, prepared] { done(prepared); }, Qt::QueuedConnection);
    });
}

PreparedAttachment AttachmentLoader::prepareOne(const Attachment &file, const QString &keepDir, const QString &soffice,
                                                const QString &pdftotext) {
    PreparedAttachment prepared;
    prepared.file = file;
    prepared.pathForTools = insideHomeOrTmp(file.path) ? file.path : keepCopy(file.path, file.name, keepDir);
    switch (file.kind) {
    case Attachment::Kind::Image: {
        if (file.size > attachments::kImageBytes) {
            prepared.note = u"a picture of %1, too large to send (the limit is %2)"_s.arg(
                attachments::sizeInWords(file.size), attachments::sizeInWords(attachments::kImageBytes));
            return prepared;
        }
        QFile image(file.path);
        if (image.open(QIODevice::ReadOnly)) prepared.image = ImageAttachment{image.readAll(), file.mimeType};
        else prepared.note = u"the picture could not be read"_s;
        return prepared;
    }
    case Attachment::Kind::Text:
        prepared.text = readWords(file.path, &prepared.truncated);
        if (prepared.text.isEmpty()) prepared.note = u"empty, or it could not be read"_s;
        return prepared;
    case Attachment::Kind::Document: {
        QTemporaryDir scratch;
        const QString suffix = QFileInfo(file.name).suffix().toLower();
        QString words;
        if (suffix == u"pdf"_s || file.mimeType == u"application/pdf"_s) {
            if (pdftotext.isEmpty()) {
#ifdef Q_OS_WIN
                prepared.note = u"a PDF; its words could not be taken out (pdftotext is not installed)"_s;
#else
                prepared.note = u"a PDF; its words could not be taken out (pdftotext is not installed: apt install poppler-utils)"_s;
#endif
                return prepared;
            }
            words = convert(pdftotext, {u"-layout"_s, file.path, scratch.filePath(u"words.txt"_s)}, scratch.path());
        } else {
            if (soffice.isEmpty()) {
                prepared.note = u"a document; its words could not be taken out (LibreOffice is not installed)"_s;
                return prepared;
            }
            static const QStringList sheets{u"xlsx"_s, u"xls"_s, u"ods"_s};
            const QString format = sheets.contains(suffix) ? u"csv"_s : u"txt:Text"_s;
            words = convert(soffice, {u"--headless"_s, u"--convert-to"_s, format, u"--outdir"_s, scratch.path(), file.path},
                            scratch.path());
        }
        if (words.isEmpty()) {
            prepared.note = u"a document whose words could not be taken out"_s;
            return prepared;
        }
        prepared.text = readWords(words, &prepared.truncated);
        if (prepared.text.trimmed().isEmpty()) prepared.note = u"a document with no words in it (a scan, or pictures only)"_s;
        return prepared;
    }
    case Attachment::Kind::Other:
        prepared.note = u"not words or a picture; read_file can read it only if it is text"_s;
        return prepared;
    }
    return prepared;
}

} // namespace nexa
