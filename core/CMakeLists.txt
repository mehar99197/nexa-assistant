add_library(nexa_core STATIC
    Result.h
    Logging.h Logging.cc
    config/Config.h config/Config.cc
    config/ConfigStore.h config/ConfigStore.cc
    net/HttpClient.h net/HttpClient.cc
    net/MultipartForm.h net/MultipartForm.cc
    net/SseParser.h net/SseParser.cc
    ai/ChatRequest.h ai/ChatRequest.cc
    ai/ChatStream.h ai/ChatStream.cc
    ai/IAiProvider.h
    ai/ToolCallAccumulator.h ai/ToolCallAccumulator.cc
    ai/OpenAiProvider.h ai/OpenAiProvider.cc
    ai/ProviderRegistry.h ai/ProviderRegistry.cc
    agent/Agent.h agent/Agent.cc
    agent/Conversation.h agent/Conversation.cc
    audit/AuditLog.h audit/AuditLog.cc
    intent/IIntentHandler.h
    intent/IntentRouter.h intent/IntentRouter.cc
    intent/PcIntents.h intent/PcIntents.cc
    intent/PhoneIntents.h intent/PhoneIntents.cc
    policy/PanicController.h policy/PanicController.cc
    policy/PolicyEngine.h policy/PolicyEngine.cc
    tools/FunctionTool.h tools/FunctionTool.cc
    tools/ITool.h
    tools/ToolArguments.h tools/ToolArguments.cc
    tools/ToolExecutor.h tools/ToolExecutor.cc
    tools/ToolRegistry.h tools/ToolRegistry.cc
    tools/ToolSpec.h tools/ToolSpec.cc
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
