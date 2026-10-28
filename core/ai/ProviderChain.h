#pragma once

#include "core/Result.h"
#include "core/ai/IAiProvider.h"

#include <QDeadlineTimer>
#include <QString>

#include <memory>
#include <vector>

namespace nexa {

class ChainStream;

// The providers in the user's order, answering as one (Chain of Responsibility, like
// RecognizerChain): a request goes to the first that can answer, each with its own model. The chain
// moves on only when a provider cannot answer now — not running, not installed, not signed in, out
// of quota, overloaded — and only before anything has streamed, so no words and no tool call are
// ever produced twice. A provider that could not answer sits out for a minute; one that answers is
// back at once. The stream's route() says who answered when it was not the first choice.
class ProviderChain : public IAiProvider {
    Q_OBJECT
public:
    struct Link {
        std::unique_ptr<IAiProvider> provider;
        QString model;      // that provider's own chosen model; the first link takes the request's
    };

    explicit ProviderChain(std::vector<Link> links, QObject *parent = nullptr);
    ~ProviderChain() override;

    QString id() const override;
    QString displayName() const override;
    Capabilities capabilities() const override;     // the first link not sitting out
    void listModels(std::function<void(Result<QStringList>)> done) override;
    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override;
    void warmUp() override;

    void setSkipWindowMs(int ms) { skipWindowMs_ = ms; }
    // An error that means "this provider cannot answer now", as opposed to "this request is wrong".
    static bool meansUnavailable(const Error &error);

private:
    friend class ChainStream;

    // The first link at or after `from` not sitting out; links_.size() when there is none.
    size_t firstUsable(size_t from) const;
    void sitOut(size_t link);
    void answered(size_t link);

    std::vector<Link> links_;
    std::vector<QDeadlineTimer> skipUntil_;
    std::vector<QString> lastFailure_;
    int skipWindowMs_{60'000};
};

} // namespace nexa
