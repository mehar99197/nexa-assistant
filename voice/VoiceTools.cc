#include "voice/VoiceTools.h"

#include "core/tools/FunctionTool.h"
#include "core/tools/ToolRegistry.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QPointer>

using namespace Qt::StringLiterals;

namespace nexa {

void registerVoiceTools(ToolRegistry &registry, VoicePipeline *pipeline) {
    ToolSpec spec{u"speak"_s,
                  u"Say text aloud through the PC's speakers (text-to-speech). Use it when the user asks you "
                  "to say, read out or announce something; replies to spoken requests are read aloud anyway."_s,
                  schema::object({{u"text"_s, schema::string(u"What to say, as plain sentences"_s)}}, {u"text"_s})};
    spec.mutating = false;
    const QPointer<VoicePipeline> voice(pipeline);
    registry.add(FunctionTool::sync(spec, [voice](const QJsonObject &args) -> Result<ToolResult> {
        const QString text = args.value(u"text"_s).toString().trimmed();
        if (text.isEmpty()) return Error{ErrorCode::InvalidArgument, u"Nothing to say."_s};
        if (!voice || !voice->canSpeak()) {
            return Error{ErrorCode::NotSupported, u"Speech output is off (Settings → Voice)."_s};
        }
        voice->speak(text);
        return ToolResult{u"Speaking with %1: %2"_s.arg(voice->speechName(), text.left(200))};
    }));
}

} // namespace nexa
