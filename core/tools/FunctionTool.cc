#include "core/tools/FunctionTool.h"

namespace nexa {

FunctionTool::FunctionTool(ToolSpec spec, Invoke invoke, std::function<void()> cancel)
    : spec_(std::move(spec)), invoke_(std::move(invoke)), cancel_(std::move(cancel)) {}

std::unique_ptr<FunctionTool> FunctionTool::sync(ToolSpec spec, SyncInvoke invoke) {
    return std::make_unique<FunctionTool>(
        std::move(spec), [invoke = std::move(invoke)](const QJsonObject &arguments, Done done) {
            done(invoke(arguments));
        });
}

void FunctionTool::invoke(const QJsonObject &arguments, Done done) {
    invoke_(arguments, std::move(done));
}

void FunctionTool::cancel() {
    if (cancel_) cancel_();
}

} // namespace nexa
