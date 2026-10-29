// WordPieces: the spotter model's word pieces for any text, split the way sentencepiece splits it.

#include "voice/wake/WordPieces.h"

#include "QtStringMakers.h"

#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// The pieces of the spotter model's bpe.model that occur in the phrases below, with their scores, plus its
// lowest-scored piece (Q), which sets what an unknown character costs.
const QHash<QString, float> kPieces{
    {u"S"_s, -2.9737f},  {u"T"_s, -3.5683f},      {u"N"_s, -4.1642f},    {u"E"_s, -4.1970f},    {u"D"_s, -4.2265f},
    {u"Y"_s, -4.5183f},  {u"▁"_s, -4.6242f},      {u"O"_s, -4.6626f},    {u"C"_s, -4.6637f},    {u"R"_s, -4.7055f},
    {u"A"_s, -4.7523f},  {u"U"_s, -4.8031f},      {u"▁S"_s, -4.9738f},   {u"G"_s, -5.0245f},    {u"L"_s, -5.0477f},
    {u"I"_s, -5.1948f},  {u"▁SO"_s, -5.2595f},    {u"OR"_s, -5.2667f},   {u"AR"_s, -5.2759f},   {u"▁HE"_s, -5.3153f},
    {u"K"_s, -5.4402f},  {u"W"_s, -5.4856f},      {u"LL"_s, -5.5720f},   {u"▁T"_s, -5.6011f},   {u"▁W"_s, -5.7251f},
    {u"ON"_s, -5.8591f}, {u"RA"_s, -5.9295f},     {u"SE"_s, -6.0779f},   {u"V"_s, -6.0915f},    {u"NE"_s, -6.1160f},
    {u"H"_s, -6.1748f},  {u"NG"_s, -6.2455f},     {u"LO"_s, -6.2875f},   {u"CK"_s, -6.2890f},   {u"EL"_s, -6.3479f},
    {u"▁O"_s, -6.3548f}, {u"VI"_s, -6.6003f},     {u"Z"_s, -6.6597f},    {u"IS"_s, -6.7041f},   {u"US"_s, -6.7396f},
    {u"X"_s, -6.7431f},  {u"TA"_s, -7.0060f},     {u"HE"_s, -7.1057f},   {u"J"_s, -7.2631f},    {u"▁NE"_s, -7.2809f},
    {u"▁TA"_s, -7.5500f}, {u"▁WORLD"_s, -7.7007f}, {u"▁JA"_s, -7.9239f},  {u"▁NEXT"_s, -8.1131f}, {u"▁OKAY"_s, -8.2925f},
    {u"Q"_s, -9.8577f}};

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
    const WordPieces pieces(kPieces);
    for (const auto &[text, expected] : kSplits) {
        INFO(text.toStdString());
        CHECK(pieces.split(text).join(u' ') == expected);
    }
}

TEST_CASE("WordPieces reads a SentencePiece model and refuses a BPE one") {
    const Result<WordPieces> unigram = WordPieces::fromModel(modelProto(kPieces, 1));
    REQUIRE(unigram.isOk());
    CHECK(unigram.value().split(u"hey nexa"_s).join(u' ') == u"▁HE Y ▁NE X A"_s);

    const Result<WordPieces> bpe = WordPieces::fromModel(modelProto(kPieces, 2));
    REQUIRE_FALSE(bpe.isOk());
    CHECK(bpe.error().code == ErrorCode::Parse);
    CHECK(bpe.error().message.contains(u"not a SentencePiece unigram model"_s));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const Result<WordPieces> missing = WordPieces::fromModelFile(dir.filePath(u"bpe.model"_s));
    REQUIRE_FALSE(missing.isOk());
    CHECK(missing.error().code == ErrorCode::Io);
}
