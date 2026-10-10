#pragma once

#include "core/tools/ITool.h"

#include <functional>
#include <memory>

namespace nexa {

// Adapter: turns a spec plus a function into an ITool, so services expose tools without a class each.
class FunctionTool : public ITool {
public:
    using Invoke = std::function<void(const QJsonObject &arguments, Done done)>;
    using SyncInvoke = std::function<Result<ToolResult>(const QJsonObject &arguments)>;

    FunctionTool(ToolSpec spec, Invoke invoke, std::function<void()> cancel = {});

    // For tools that finish immediately (no I/O wait).
    static std::unique_ptr<FunctionTool> sync(ToolSpec spec, SyncInvoke invoke);

    const ToolSpec &spec() const override { return spec_; }
    void invoke(const QJsonObject &arguments, Done done) override;
    void cancel() override;

private:
    ToolSpec spec_;
    Invoke invoke_;
    std::function<void()> cancel_;
};

} // namespace nexa
