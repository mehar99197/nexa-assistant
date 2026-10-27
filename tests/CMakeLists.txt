find_package(Catch2 3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(nexa_tests
    TestMain.cc
    AdbDiscoveryTest.cc
    AdbParsersTest.cc
    AdbShellTest.cc
    FakeHttpServer.h
    QtStringMakers.h
    AgentTest.cc
    AgyEventsTest.cc
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
    KeyMapTest.cc
    LocalCommandRunnerTest.cc
    McpClientTest.cc
    MemoryIntentTest.cc
    MemoryStoreTest.cc
    OdfWriterTest.cc
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
    ProviderRegistryTest.cc
    SkillCatalogTest.cc
    SkillParserTest.cc
    PortalInputBackendTest.cc
    QrCodeTest.cc
    PhoneWindowTest.cc
    RemoteClientTest.cc
    RemoteDesktopSessionTest.cc
    RemoteProtocolTest.cc
    RemoteServerTest.cc
    RemoteServerIntegrationTest.cc
    # The helper's own authorisation record. Pure enough to test here without any root privilege,
    # and worth testing: it is what keeps the password prompt from coming back.
    SessionGrantsTest.cc ../helper/SessionGrants.cc
    SseParserTest.cc
    SystemPureTest.cc
    TextToolParserTest.cc
    ToolCallAccumulatorTest.cc
    TtsFileRendererTest.cc
    TurnLedgerTest.cc
    VoicePipelineTest.cc
    VoicePureTest.cc
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
