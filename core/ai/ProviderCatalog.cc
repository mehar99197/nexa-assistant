#include "core/ai/ProviderCatalog.h"

#include <QHash>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

struct Catalog {
    QList<CatalogEntry> entries;
    QHash<QString, int> index;   // id -> position in `entries`

    Catalog() {
        entries.reserve(qsizetype(catalogdata::kRowCount));
        for (std::size_t i = 0; i < catalogdata::kRowCount; ++i) {
            const catalogdata::Row &row = catalogdata::kRows[i];
            entries.append(CatalogEntry{
                .id = QString::fromUtf8(row.id),
                .name = QString::fromUtf8(row.name),
                .kind = QString::fromLatin1(row.kind),
                .baseUrl = QString::fromLatin1(row.baseUrl),
                .modelsUrl = QString::fromLatin1(row.modelsUrl),
                .requiresKey = row.requiresKey,
                .category = QString::fromLatin1(row.category),
                .website = QString::fromLatin1(row.website),
                .hint = QString::fromUtf8(row.hint),
                .freeTier = row.freeTier,
            });
            index.insert(entries.last().id, int(i));
        }
    }
};

// Built on first use (and once: a function-local static is initialised thread-safely), never at load.
const Catalog &catalog() {
    static const Catalog instance;
    return instance;
}

} // namespace

const QList<CatalogEntry> &ProviderCatalog::entries() {
    return catalog().entries;
}

QStringList ProviderCatalog::categories() {
    return {u"Gateways"_s, u"Model makers"_s, u"Inference hosts"_s, u"Regional"_s,
            u"Cloud platforms"_s, u"Local"_s, u"Free"_s};
}

const CatalogEntry *ProviderCatalog::find(const QString &id) {
    const Catalog &all = catalog();
    const auto found = all.index.constFind(id);
    return found == all.index.constEnd() ? nullptr : &all.entries.at(found.value());
}

} // namespace nexa
