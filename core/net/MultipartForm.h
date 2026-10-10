#pragma once

#include <QByteArray>
#include <QString>

namespace nexa {

// Builder for multipart/form-data bodies (file uploads such as /v1/audio/transcriptions). Pure;
// the boundary is random unless given, so tests can pin it.
class MultipartForm {
public:
    MultipartForm();
    explicit MultipartForm(QByteArray boundary);

    MultipartForm &addField(const QString &name, const QString &value);
    MultipartForm &addFile(const QString &name, const QString &fileName, const QByteArray &contentType,
                           const QByteArray &data);

    QByteArray contentType() const;     // for the Content-Type header, boundary included
    QByteArray body() const;            // the parts plus the closing boundary

private:
    void beginPart(const QString &name, const QString &fileName = {});

    QByteArray boundary_;
    QByteArray parts_;
};

} // namespace nexa
