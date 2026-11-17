add_library(nexa_core STATIC
    Result.h
    Logging.h Logging.cc
    ProcessUtils.h ProcessUtils.cc
    account/Entitlements.h account/Entitlements.cc
    account/Obfuscation.h
    config/Config.h config/Config.cc
    config/ConfigStore.h config/ConfigStore.cc
    config/Portable.h config/Portable.cc
    net/HttpClient.h net/HttpClient.cc
    net/MultipartForm.h net/MultipartForm.cc
    net/SseParser.h net/SseParser.cc
    ai/AgyEvents.h ai/AgyEvents.cc
    ai/AgyProvider.h ai/AgyProvider.cc
    ai/AgySession.h ai/AgySession.cc
    ai/AgyAcp.h ai/AgyAcp.cc
    ai/AgySignIn.h ai/AgySignIn.cc
    ai/CodexEvents.h ai/CodexEvents.cc
    ai/CodexProtocol.h ai/CodexProtocol.cc
    ai/CodexProvider.h ai/CodexProvider.cc
    ai/CodexSession.h ai/CodexSession.cc
    ai/CodexSignIn.h ai/CodexSignIn.cc
    ai/ClaudeCodeEvents.h ai/ClaudeCodeEvents.cc
    ai/ClaudeCodeProvider.h ai/ClaudeCodeProvider.cc
    ai/ClaudeCodeSession.h ai/ClaudeCodeSession.cc
    ai/ClaudeCodeSignIn.h ai/ClaudeCodeSignIn.cc
    ai/OpenCodeSignIn.h ai/OpenCodeSignIn.cc
    ai/PresetFilter.h ai/PresetFilter.cc
    ai/ProviderCatalog.h ai/ProviderCatalog.cc
    ai/ProviderCatalogData.cc
    ai/SignIn.h
    ai/SignInPresenter.h ai/SignInPresenter.cc
    ai/AnthropicProvider.h ai/AnthropicProvider.cc
    ai/ChatRequest.h ai/ChatRequest.cc
    ai/ChatStream.h ai/ChatStream.cc
    ai/GeminiProvider.h ai/GeminiProvider.cc
    ai/IAiProvider.h
    ai/ToolCallAccumulator.h ai/ToolCallAccumulator.cc
    ai/TranscriptWriter.h ai/TranscriptWriter.cc
    ai/TurnLedger.h ai/TurnLedger.cc
    ai/OpenAiProvider.h ai/OpenAiProvider.cc
    ai/OpenCodeProvider.h ai/OpenCodeProvider.cc
    ai/OpenCodeServer.h ai/OpenCodeServer.cc
    ai/OpenCodeWire.h ai/OpenCodeWire.cc
    ai/ProgramLookup.h ai/ProgramLookup.cc
    ai/ProviderChain.h ai/ProviderChain.cc
    ai/ProviderRegistry.h ai/ProviderRegistry.cc
    ai/SseChatStream.h ai/SseChatStream.cc
    agent/Agent.h agent/Agent.cc
    agent/AppContext.h agent/AppContext.cc
    agent/Attachments.h agent/Attachments.cc
    agent/Conversation.h agent/Conversation.cc
    agent/VoicePrompt.h agent/VoicePrompt.cc
    mission/ActivityFeed.h mission/ActivityFeed.cc
    mission/AgentRole.h mission/AgentRole.cc
    mission/DelegateTool.h mission/DelegateTool.cc
    mission/HardcoreMode.h mission/HardcoreMode.cc
    mission/IInputLock.h
    mission/MissionControl.h mission/MissionControl.cc
    mission/ModelFit.h mission/ModelFit.cc
    mission/RoleClassifier.h mission/RoleClassifier.cc
    mission/Task.h mission/Task.cc
    mission/TaskRunner.h mission/TaskRunner.cc
    mission/TaskTarget.h mission/TaskTarget.cc
    mission/TaskTranscript.h mission/TaskTranscript.cc
    audit/AuditLog.h audit/AuditLog.cc
    intent/IIntentHandler.h
    intent/IntentRouter.h intent/IntentRouter.cc
    intent/MemoryIntents.h intent/MemoryIntents.cc
    intent/MissionIntents.h intent/MissionIntents.cc
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

set(NEXA_ACCOUNT_SOURCES
    account/AccountManager.h account/AccountManager.cc
    account/CredentialStore.h account/CredentialStore.cc
    account/Guard.h account/Guard.cc
    account/KeyIntegrity.h account/KeyIntegrity.cc
    account/LicenseToken.h account/LicenseToken.cc
    account/UpdateChecker.h account/UpdateChecker.cc
)

# Production account & licensing library: verifies against NEXA_LICENSE_PUBLIC_KEY + KeyIntegrity,
# and applies obfuscation on Release shipping builds (unless NEXA_DEV_BUILD=ON).
add_library(nexa_account STATIC ${NEXA_ACCOUNT_SOURCES})
target_link_libraries(nexa_account
    PUBLIC
        nexa_core
        Qt6::Core
        Qt6::Network
    PRIVATE
        nexa_options
        OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(nexa_account PRIVATE advapi32 shell32)
endif()
if(NEXA_LICENSE_PUBLIC_KEY AND NOT NEXA_DEV_BUILD)
    target_compile_definitions(nexa_account PRIVATE
        NEXA_LICENSE_PUBLIC_KEY="${NEXA_LICENSE_PUBLIC_KEY}"
        NEXA_LICENSE_KEY_DIGEST="${NEXA_LICENSE_KEY_DIGEST}"
    )
endif()
if(NEXA_DEV_BUILD)
    target_compile_definitions(nexa_account PRIVATE NEXA_DEV_BUILD=1)
endif()
target_compile_definitions(nexa_account PRIVATE
    $<$<AND:$<BOOL:${NEXA_OBFUSCATE_LICENSE}>,$<NOT:$<BOOL:${NEXA_DEV_BUILD}>>,$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>>:NEXA_OBFUSCATE_LICENSE=1>
    NEXA_CHECK_ROTATION_SEED=${NEXA_CHECK_ROTATION_SEED}
)

# Development/test account library: compiled with the dev signing key + in-memory credential store
# so Catch2 suites exercise full HTTP flows without touching the host keyring or production key.
if(NEXA_BUILD_TESTS)
    add_library(nexa_account_dev STATIC ${NEXA_ACCOUNT_SOURCES})
    target_link_libraries(nexa_account_dev
        PUBLIC
            nexa_core
            Qt6::Core
            Qt6::Network
        PRIVATE
            nexa_options
            OpenSSL::Crypto
    )
    target_compile_definitions(nexa_account_dev PRIVATE
        NEXA_DEV_BUILD=1
        NEXA_TEST_CREDENTIAL_STORE=1
    )
    if(WIN32)
        target_link_libraries(nexa_account_dev PRIVATE advapi32 shell32)
    endif()
endif()
