#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

namespace nexa {

// The word pieces of a SentencePiece unigram model — the keyword spotter's bpe.model — and the split
// sentencepiece makes of a text with them: the most probable sequence of pieces (Viterbi over the
// pieces' scores). Lets Nexa turn any wake name into the spotter's keywords without sentencepiece. Pure.
class WordPieces {
public:
    explicit WordPieces(QHash<QString, float> normalPieces);

    // A serialized SentencePiece ModelProto; refused unless it is a unigram model.
    static Result<WordPieces> fromModel(const QByteArray &modelProto);
    static Result<WordPieces> fromModelFile(const QString &path);

    // "hey nexa" → {"▁HE", "Y", "▁NE", "X", "A"}: upper case, each word after a ▁.
    QStringList split(const QString &text) const;

private:
    QHash<QString, float> pieces_;
    double unknownScore_{-10.0};     // a character that is no piece: the lowest score, minus 10
    qsizetype longest_{1};
};

} // namespace nexa
