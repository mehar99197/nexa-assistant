find_package(Catch2 3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(nexa_tests
    TestMain.cc
    AdbDiscoveryTest.cc
    AdbParsersTest.cc
    AdbShellTest.cc
    AndroidToolsTest.cc
    AtspiScreenReaderTest.cc
    AssetDownloaderTest.cc
    EngineAssetsTest.cc
    EngineLocatorTest.cc
    ScreenOutlineTest.cc
    FakeScreenReader.h
    ScreenToolsTest.cc
    FakeHttpServer.h
    QtStringMakers.h
    ScriptedProvider.h
    MissionRig.h
    AgentTest.cc
    AttachmentsTest.cc
    AgyEventsTest.cc
    AgyAcpTest.cc
    AgySignInTest.cc
    CodexProtocolTest.cc
    CodexProviderTest.cc
    CodexSignInTest.cc
    ClaudeCodeEventsTest.cc
    ClaudeCodeProviderTest.cc
    ClaudeCodeSignInTest.cc
    OpenCodeSignInTest.cc
    PresetFilterTest.cc
    ProgramLookupTest.cc
    ToolArgumentsTest.cc
    ToolExecutorTest.cc
    ActivityFeedTest.cc
    AgentRoleTest.cc
    ModelFitTest.cc
    RoleClassifierTest.cc
    TaskTest.cc
    TaskTranscriptTest.cc
    TaskRunnerTest.cc
    TaskTargetTest.cc
    WhisperGateTest.cc
    SpeakerGateTest.cc
    ProcessStatsTest.cc
    MissionControlTest.cc
    HardcoreModeTest.cc
    FakeInputLock.h
    SignInPresenterTest.cc
    ProviderCatalogTest.cc
    AgyProviderTest.cc
    AppContextTest.cc
    BrowserServiceTest.cc
    ChromeProfilesTest.cc
    AnthropicProviderTest.cc
    ChatTestUtil.h
    CombinedInputBackendTest.cc
    ConfigStoreTest.cc
    ConversationTest.cc
    DesktopEntryTest.cc
    FileAccessTest.cc
    GeminiProviderTest.cc
    IntentRouterTest.cc
    ListeningFrontTest.cc
    KeyMapTest.cc
    LocalCommandRunnerTest.cc
    McpClientTest.cc
    MemoryIntentTest.cc
    MicrophoneCheckTest.cc
    MemoryStoreTest.cc
    OdfWriterTest.cc
    OpenCodeProviderTest.cc
    OpenCodeWireTest.cc
    OfficeToolsTest.cc
    OpenAiProviderTest.cc
    OpenAiStreamTest.cc
    PairingManagerTest.cc
    MdnsBrowserTest.cc
    MdnsPacketTest.cc
    NexaChromeTest.cc
    NexaCoreSpecTest.cc
    NexaDesignsTest.cc
    NexaThemesTest.cc
    PhoneIntentTest.cc
    PhoneLookupTest.cc
    # The window's Phone panel, built from app/ like the helper's record below.
    PhonePanelTest.cc ../app/PhonePanel.cc
    PolicyEngineTest.cc
    ProviderChainTest.cc
    # The Providers tab's sign-in block and the provider gallery, built from app/ like the Phone panel above.
    ProviderSignInBlockTest.cc ../app/ProviderSignInBlock.cc
    AppControllerTest.cc ../app/AppController.cc ../app/MicrophoneLeveler.cc
    MissionBoardTest.cc AgentsRailTest.cc ActivityStreamTest.cc AddTasksDialogTest.cc TaskTranscriptDrawerTest.cc
        ../app/MissionPanel.cc ../app/TaskListModel.cc ../app/AgentsRail.cc ../app/MissionBoard.cc ../app/ActivityStream.cc
        ../app/AddTasksDialog.cc ../app/TaskTranscriptDrawer.cc ../app/TaskStrip.cc ../app/MetricsTiles.cc ../app/CoreCard.cc
        ../app/ChatView.cc
    ProviderGalleryDialogTest.cc ../app/ProviderGalleryDialog.cc
    ProviderRegistryTest.cc
    # The dialog itself, for the sign-in block it holds per program row, built from app/ with its pages.
    SettingsDialogProvidersTest.cc SettingsDialogThemesTest.cc
        ../app/SettingsDialog.cc ../app/AccountSettingsPage.cc ../app/BrowserSettingsPage.cc ../app/RemoteAccess.cc
        ../app/MemorySettingsPage.cc ../app/MissionSettingsPage.cc ../app/SkillsSettingsPage.cc ../app/PhoneLink.cc
        ../app/GlobalShortcuts.cc ../app/VoiceEnrolmentDialog.cc ../app/VoiceSampleDialog.cc
    SkillCatalogTest.cc
    SkillParserTest.cc
    PortalInputBackendTest.cc
    QrCodeTest.cc
    RemoteClientTest.cc
    RemoteDesktopSessionTest.cc
    RemoteProtocolTest.cc
    RemoteRoutesTest.cc
    TunnelLogTest.cc
    TunnelProcessTest.cc
    UiHierarchyTest.cc
    RemoteServerTest.cc
    RemoteServerIntegrationTest.cc
    SherpaTest.cc
    SseParserTest.cc
    SystemPureTest.cc
    AccountManagerTest.cc
    AccountIntegrationTest.cc
    GuardTest.cc
    LicenseTokenTest.cc
    LocalApiServerTest.cc
    PortableTest.cc
    TextToolParserTest.cc
    UpdateCheckerTest.cc
    ToolCallAccumulatorTest.cc
    TtsFileRendererTest.cc
    TurnLedgerTest.cc
    VoiceBackendsTest.cc
    VoiceSampleTest.cc
    PhonemizerTest.cc
    ClonedVoiceSynthesizerTest.cc
    VoicePipelineTest.cc
    VoicePromptTest.cc
    VoicePureTest.cc
    WakePhrasesTest.cc
    WordPiecesTest.cc
    VoiceTestAudio.h
)

target_link_libraries(nexa_tests PRIVATE
    nexa_core
    nexa_account_dev
    nexa_proto
    nexa_system
    nexa_voice
    nexa_remote
    nexa_ui
    $<$<BOOL:${NEXA_BUILD_PHONE}>:nexa_phone_ui>
    nexa_options
    OpenSSL::Crypto
    Qt6::Test
    Catch2::Catch2
)

if(UNIX AND NOT ANDROID AND NOT APPLE)
    target_sources(nexa_tests PRIVATE
        AtspiCacheTest.cc
        SessionGrantsTest.cc ../helper/SessionGrants.cc
        InputDevicesTest.cc ../helper/InputDevices.cc
        DeviceLockTest.cc ../helper/DeviceLock.cc
    )
    target_link_libraries(nexa_tests PRIVATE Qt6::DBus)
elseif(WIN32)
    target_link_libraries(nexa_tests PRIVATE user32)
endif()

# Standalone obfuscated Guard binary: proves identical truth-table behaviour under NEXA_OBFUSCATE_LICENSE.
add_executable(nexa_guard_obf_test
    TestMain.cc
    GuardTest.cc
    ../core/account/Guard.cc
)
target_include_directories(nexa_guard_obf_test PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(nexa_guard_obf_test PRIVATE
    nexa_options
    Qt6::Widgets
    Catch2::Catch2
)
target_compile_definitions(nexa_guard_obf_test PRIVATE
    NEXA_OBFUSCATE_LICENSE=1
    NEXA_CHECK_ROTATION_SEED=${NEXA_CHECK_ROTATION_SEED}
)
add_test(NAME nexa_guard_obf_test COMMAND nexa_guard_obf_test)

# Where the test data lives (the fake MCP server script).
target_compile_definitions(nexa_tests PRIVATE NEXA_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

include(Catch)
catch_discover_tests(nexa_tests
    # A Catch2 run whose every case was skipped (no models, no bus, no LibreOffice on a runner) exits 4: that is a skip,
    # not a failure, to ctest too. As a test property, because Ubuntu 24.04's Catch2 (3.4) predates the
    # SKIP_RETURN_CODE argument of catch_discover_tests and ignores it.
    PROPERTIES SKIP_RETURN_CODE 4
)

# The phone app's screens against the real server: only with the phone app built for the desktop (NEXA_BUILD_PHONE).
if(NEXA_BUILD_PHONE)
    target_sources(nexa_tests PRIVATE PhoneWindowTest.cc)
endif()
