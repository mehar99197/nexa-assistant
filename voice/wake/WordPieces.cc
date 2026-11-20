#include "voice/wake/WordPieces.h"

#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kNormalPiece = 1;     // SentencePiece::Type NORMAL
constexpr int kUnigramModel = 1;    // TrainerSpec::ModelType UNIGRAM (also the default)

// Just enough of the protobuf wire format for a ModelProto.
class ProtoReader {
public:
    explicit ProtoReader(QByteArrayView bytes) : bytes_(bytes) {}

    bool atEnd() const { return pos_ >= bytes_.size(); }
    bool failed() const { return failed_; }

    std::optional<quint64> varint() {
        quint64 value = 0;
        for (int shift = 0; shift < 64 && pos_ < bytes_.size(); shift += 7) {
            const auto byte = quint8(bytes_[pos_++]);
            value |= quint64(byte & 0x7F) << shift;
            if (byte < 0x80) return value;
        }
        failed_ = true;
        return std::nullopt;
    }

    QByteArrayView take(qsizetype size) {
        if (size < 0 || pos_ + size > bytes_.size()) {
            failed_ = true;
            return {};
        }
        const QByteArrayView view = bytes_.sliced(pos_, size);
        pos_ += size;
        return view;
    }

    // One field: its number and wire type, with the payload of a length-delimited or fixed32 field.
    struct Field {
        int number{0};
        int wire{0};
        quint64 value{0};       // varint
        QByteArrayView bytes;   // length-delimited, fixed32, fixed64
    };

    std::optional<Field> next() {
        const std::optional<quint64> key = varint();
        if (!key) return std::nullopt;
        Field field{.number = int(*key >> 3), .wire = int(*key & 7)};
        switch (field.wire) {
        case 0:
            if (const std::optional<quint64> value = varint()) {
                field.value = *value;
                return field;
            }
            return std::nullopt;
        case 1: field.bytes = take(8); break;
        case 2:
            if (const std::optional<quint64> size = varint()) {
                field.bytes = take(qsizetype(*size));
                break;
            }
            return std::nullopt;
        case 5: field.bytes = take(4); break;
        default: failed_ = true; return std::nullopt;     // groups are not used by SentencePiece
        }
        if (failed_) return std::nullopt;
        return field;
    }

private:
    QByteArrayView bytes_;
    qsizetype pos_{0};
    bool failed_{false};
};

Error unreadable(const QString &why) {
    return Error{ErrorCode::Parse, u"The word-piece model is unreadable: %1."_s.arg(why)};
}

} // namespace

WordPieces::WordPieces(QHash<QString, float> normalPieces) : pieces_(std::move(normalPieces)) {
    float lowest = 0.0f;
    for (auto it = pieces_.cbegin(); it != pieces_.cend(); ++it) {
        lowest = std::min(lowest, it.value());
        longest_ = std::max(longest_, it.key().size());
    }
    unknownScore_ = double(lowest) - 10.0;
}

Result<WordPieces> WordPieces::fromModel(const QByteArray &modelProto) {
    QHash<QString, float> pieces;
    ProtoReader model(modelProto);
    while (!model.atEnd()) {
        const std::optional<ProtoReader::Field> field = model.next();
        if (!field) return unreadable(u"it is cut short"_s);
        if (field->number == 1 && field->wire == 2) {
            QString piece;
            float score = 0.0f;
            int type = kNormalPiece;
            ProtoReader reader(field->bytes);
            while (!reader.atEnd()) {
                const std::optional<ProtoReader::Field> part = reader.next();
                if (!part) return unreadable(u"a piece is cut short"_s);
                if (part->number == 1 && part->wire == 2) piece = QString::fromUtf8(part->bytes);
                if (part->number == 2 && part->wire == 5) std::memcpy(&score, part->bytes.data(), sizeof score);
                if (part->number == 3 && part->wire == 0) type = int(part->value);
            }
            if (type == kNormalPiece && !piece.isEmpty()) pieces.insert(piece, score);
        } else if (field->number == 2 && field->wire == 2) {
            ProtoReader trainer(field->bytes);
            while (!trainer.atEnd()) {
                const std::optional<ProtoReader::Field> part = trainer.next();
                if (!part) return unreadable(u"its trainer settings are cut short"_s);
                if (part->number == 3 && part->wire == 0 && int(part->value) != kUnigramModel) {
                    return Error{ErrorCode::Parse, u"The word-piece model is not a SentencePiece unigram model."_s};
                }
            }
        }
    }
    if (pieces.isEmpty()) return unreadable(u"it has no word pieces"_s);
    return WordPieces(std::move(pieces));
}

Result<WordPieces> WordPieces::fromModelFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Error{ErrorCode::Io, u"Cannot read %1: %2"_s.arg(path, file.errorString())};
    }
    return fromModel(file.readAll());
}

QStringList WordPieces::split(const QString &text) const {
    static const QRegularExpression spaces(u"\\s+"_s);
    const QStringList words = text.toUpper().split(spaces, Qt::SkipEmptyParts);
    if (words.isEmpty()) return {};
    const QString marker = u"▁"_s;
    const QString line = marker + words.join(marker);

    // best[end]: the highest total score of a split of line[0, end), and where its last piece starts.
    const qsizetype length = line.size();
    constexpr double kNone = -std::numeric_limits<double>::infinity();
    std::vector<double> best(size_t(length + 1), kNone);
    std::vector<qsizetype> from(size_t(length + 1), -1);
    best[0] = 0.0;
    for (qsizetype end = 1; end <= length; ++end) {
        // Longest candidates first, keeping the first best: the order sentencepiece's own split agrees with.
        for (qsizetype start = std::max<qsizetype>(0, end - longest_); start < end; ++start) {
            if (best[size_t(start)] == kNone) continue;
            const QString piece = line.sliced(start, end - start);
            double score = kNone;
            if (const auto found = pieces_.constFind(piece); found != pieces_.cend()) {
                score = best[size_t(start)] + double(found.value());
            } else if (end - start == 1) {
                score = best[size_t(start)] + unknownScore_;
            }
            if (score > best[size_t(end)]) {
                best[size_t(end)] = score;
                from[size_t(end)] = start;
            }
        }
    }

    QStringList split;
    for (qsizetype end = length; end > 0;) {
        const qsizetype start = from[size_t(end)];
        split.prepend(line.sliced(start, end - start));
        end = start;
    }
    return split;
}

} // namespace nexa
