find_package(Catch2 3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(nexa_tests
    TestMain.cc
    AdbDiscoveryTest.cc
    AdbParsersTest.cc
    AdbShellTest.cc
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
    OpenCodeSignInTest.cc
    PresetFilterTest.cc
    ProgramLookupTest.cc
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
    SettingsDialogProvidersTest.cc ../app/SettingsDialog.cc ../app/BrowserSettingsPage.cc ../app/RemoteAccess.cc
        ../app/MemorySettingsPage.cc ../app/MissionSettingsPage.cc ../app/SkillsSettingsPage.cc ../app/PhoneLink.cc
        ../app/GlobalShortcuts.cc ../app/VoiceEnrolmentDialog.cc
    SkillCatalogTest.cc
    SkillParserTest.cc
    PortalInputBackendTest.cc
    QrCodeTest.cc
    PhoneWindowTest.cc
    RemoteClientTest.cc
    RemoteDesktopSessionTest.cc
    RemoteProtocolTest.cc
    RemoteRoutesTest.cc
    TunnelLogTest.cc
    TunnelProcessTest.cc
    RemoteServerTest.cc
    RemoteServerIntegrationTest.cc
    # The helper's own authorisation record. Pure enough to test here without any root privilege,
    # and worth testing: it is what keeps the password prompt from coming back.
    SessionGrantsTest.cc ../helper/SessionGrants.cc
    # Hardcore mode's lock of the keyboard and pointer, over a fake /sys/class/input.
    InputDevicesTest.cc ../helper/InputDevices.cc
    DeviceLockTest.cc ../helper/DeviceLock.cc
    SherpaTest.cc
    SseParserTest.cc
    SystemPureTest.cc
    TextToolParserTest.cc
    ToolCallAccumulatorTest.cc
    TtsFileRendererTest.cc
    TurnLedgerTest.cc
    VoiceBackendsTest.cc
    VoicePipelineTest.cc
    VoicePromptTest.cc
    VoicePureTest.cc
    WakePhrasesTest.cc
    WordPiecesTest.cc
    VoiceTestAudio.h
)

target_link_libraries(nexa_tests PRIVATE
    nexa_core
    nexa_proto
    nexa_system
    nexa_voice
    nexa_remote
    nexa_ui
    nexa_phone_ui
    nexa_options
    Qt6::Test
    Catch2::Catch2
)

# Where the test data lives (the fake MCP server script).
target_compile_definitions(nexa_tests PRIVATE NEXA_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

include(Catch)
catch_discover_tests(nexa_tests)
