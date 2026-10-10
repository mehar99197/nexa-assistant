#include "core/net/MultipartForm.h"

#include <QRandomGenerator>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Header parameter values are quoted; quotes and line breaks would end the header early.
QByteArray quoted(const QString &value) {
    QByteArray bytes = value.toUtf8();
    bytes.replace('\\', "\\\\").replace('"', "\\\"").replace('\r', ' ').replace('\n', ' ');
    return '"' + bytes + '"';
}

} // namespace

MultipartForm::MultipartForm()
    : boundary_("nexa-" + QByteArray::number(QRandomGenerator::global()->generate64(), 36)
                + QByteArray::number(QRandomGenerator::global()->generate64(), 36)) {}

MultipartForm::MultipartForm(QByteArray boundary) : boundary_(std::move(boundary)) {}

MultipartForm &MultipartForm::addField(const QString &name, const QString &value) {
    beginPart(name);
    parts_ += "\r\n" + value.toUtf8() + "\r\n";
    return *this;
}

MultipartForm &MultipartForm::addFile(const QString &name, const QString &fileName, const QByteArray &contentType,
                                      const QByteArray &data) {
    beginPart(name, fileName);
    parts_ += "Content-Type: " + contentType + "\r\n\r\n";
    parts_ += data;
    parts_ += "\r\n";
    return *this;
}

QByteArray MultipartForm::contentType() const {
    return "multipart/form-data; boundary=" + boundary_;
}

QByteArray MultipartForm::body() const {
    return parts_ + "--" + boundary_ + "--\r\n";
}

void MultipartForm::beginPart(const QString &name, const QString &fileName) {
    parts_ += "--" + boundary_ + "\r\n";
    parts_ += "Content-Disposition: form-data; name=" + quoted(name);
    if (!fileName.isEmpty()) parts_ += "; filename=" + quoted(fileName);
    parts_ += "\r\n";
}

} // namespace nexa
