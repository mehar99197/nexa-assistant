add_library(nexa_core STATIC
    Result.h
    Logging.h Logging.cc
    config/Config.h config/Config.cc
    config/ConfigStore.h config/ConfigStore.cc
    net/HttpClient.h net/HttpClient.cc
    net/MultipartForm.h net/MultipartForm.cc
    net/SseParser.h net/SseParser.cc
    ai/AnthropicProvider.h ai/AnthropicProvider.cc
    ai/ChatRequest.h ai/ChatRequest.cc
    ai/ChatStream.h ai/ChatStream.cc
    ai/GeminiProvider.h ai/GeminiProvider.cc
    ai/IAiProvider.h
    ai/ToolCallAccumulator.h ai/ToolCallAccumulator.cc
    ai/OpenAiProvider.h ai/OpenAiProvider.cc
    ai/ProgramLookup.h ai/ProgramLookup.cc
    ai/ProviderRegistry.h ai/ProviderRegistry.cc
    ai/SseChatStream.h ai/SseChatStream.cc
    agent/Agent.h agent/Agent.cc
    agent/AppContext.h agent/AppContext.cc
    agent/Conversation.h agent/Conversation.cc
    audit/AuditLog.h audit/AuditLog.cc
    intent/IIntentHandler.h
    intent/IntentRouter.h intent/IntentRouter.cc
    intent/MemoryIntents.h intent/MemoryIntents.cc
    intent/PcIntents.h intent/PcIntents.cc
    intent/PhoneIntents.h intent/PhoneIntents.cc
    memory/MemoryStore.h memory/MemoryStore.cc
    memory/MemoryTools.h memory/MemoryTools.cc
    mcp/McpClient.h mcp/McpClient.cc
    mcp/McpConnectors.h mcp/McpConnectors.cc
    mcp/McpTransport.h mcp/McpTransport.cc
    policy/PanicController.h policy/PanicController.cc
    skills/SkillCatalog.h skills/SkillCatalog.cc
    skills/SkillParser.h skills/SkillParser.cc
    skills/SkillTools.h skills/SkillTools.cc
    policy/PolicyEngine.h policy/PolicyEngine.cc
    tools/FunctionTool.h tools/FunctionTool.cc
    tools/ITool.h
    tools/TextToolParser.h tools/TextToolParser.cc
    tools/ToolArguments.h tools/ToolArguments.cc
    tools/ToolExecutor.h tools/ToolExecutor.cc
    tools/ToolRegistry.h tools/ToolRegistry.cc
    tools/ToolSpec.h tools/ToolSpec.cc
)

# Nexa's own skills, read by SkillCatalog from ":/nexa/skills/<name>/SKILL.md".
qt_add_resources(nexa_core "nexa_skills"
    PREFIX "/nexa/skills"
    BASE skills/builtin
    FILES
        skills/builtin/web-browsing/SKILL.md
)

# Includes are rooted at the project directory: #include "core/ai/IAiProvider.h"
target_include_directories(nexa_core PUBLIC "${PROJECT_SOURCE_DIR}")

target_link_libraries(nexa_core
    PUBLIC
        Qt6::Core
        Qt6::Network
    PRIVATE
        nexa_options
)
