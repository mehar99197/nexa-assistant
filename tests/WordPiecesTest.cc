// WordPieces: the spotter model's word pieces for any text, split the way sentencepiece splits it.

#include "voice/wake/WordPieces.h"

#include "QtStringMakers.h"
#include "WakeTestPieces.h"

#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// What sentencepiece itself made of these phrases with the real model.
const QList<std::pair<QString, QString>> kSplits{
    {u"hey nexa"_s, u"▁HE Y ▁NE X A"_s},          {u"hey jarvis"_s, u"▁HE Y ▁JA R VI S"_s},
    {u"hey tauseeq"_s, u"▁HE Y ▁TA U SE E Q"_s},  {u"hello world"_s, u"▁HE LL O ▁WORLD"_s},
    {u"necksa"_s, u"▁NE CK S A"_s},               {u"okay nexa"_s, u"▁OKAY ▁NE X A"_s},
    {u"hey zara"_s, u"▁HE Y ▁ Z AR A"_s},         {u"next song"_s, u"▁NEXT ▁SO NG"_s}};

void varint(QByteArray &out, quint64 value) {
    while (value >= 0x80) {
        out.append(char((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.append(char(value));
}

void lengthDelimited(QByteArray &out, int field, const QByteArray &bytes) {
    varint(out, quint64(field) << 3 | 2);
    varint(out, quint64(bytes.size()));
    out.append(bytes);
}

// A SentencePiece ModelProto: every piece (field 1: piece, score, type NORMAL) and a TrainerSpec (field 2)
// saying which kind of model it is (1 unigram, 2 BPE).
QByteArray modelProto(const QHash<QString, float> &pieces, int modelType) {
    QByteArray model;
    for (auto it = pieces.cbegin(); it != pieces.cend(); ++it) {
        QByteArray piece;
        lengthDelimited(piece, 1, it.key().toUtf8());
        varint(piece, 2 << 3 | 5);
        const float score = it.value();
        char bytes[4];
        std::memcpy(bytes, &score, 4);
        piece.append(bytes, 4);
        varint(piece, 3 << 3 | 0);
        varint(piece, 1);
        lengthDelimited(model, 1, piece);
    }
    QByteArray trainer;
    varint(trainer, 3 << 3 | 0);
    varint(trainer, quint64(modelType));
    lengthDelimited(model, 2, trainer);
    return model;
}

} // namespace

TEST_CASE("WordPieces splits like sentencepiece") {
    const WordPieces pieces(testpieces::kPieces);
    for (const auto &[text, expected] : kSplits) {
        INFO(text.toStdString());
        CHECK(pieces.split(text).join(u' ') == expected);
    }
}

TEST_CASE("WordPieces reads a SentencePiece model and refuses a BPE one") {
    const Result<WordPieces> unigram = WordPieces::fromModel(modelProto(testpieces::kPieces, 1));
    REQUIRE(unigram.isOk());
    CHECK(unigram.value().split(u"hey nexa"_s).join(u' ') == u"▁HE Y ▁NE X A"_s);

    const Result<WordPieces> bpe = WordPieces::fromModel(modelProto(testpieces::kPieces, 2));
    REQUIRE_FALSE(bpe.isOk());
    CHECK(bpe.error().code == ErrorCode::Parse);
    CHECK(bpe.error().message.contains(u"not a SentencePiece unigram model"_s));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const Result<WordPieces> missing = WordPieces::fromModelFile(dir.filePath(u"bpe.model"_s));
    REQUIRE_FALSE(missing.isOk());
    CHECK(missing.error().code == ErrorCode::Io);
}
