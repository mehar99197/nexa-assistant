#pragma once

namespace nexa {

class ToolRegistry;
class VoicePipeline;

// Registers the voice tools the model can call: speak (say something aloud).
void registerVoiceTools(ToolRegistry &registry, VoicePipeline *pipeline);

} // namespace nexa
