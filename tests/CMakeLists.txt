find_package(Catch2 3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(nexa_tests
    TestMain.cc
    AdbParsersTest.cc
    AdbShellTest.cc
    FakeHttpServer.h
    QtStringMakers.h
    AgentTest.cc
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
    OpenAiProviderTest.cc
    OpenAiStreamTest.cc
    PairingManagerTest.cc
    PhoneIntentTest.cc
    PhoneLookupTest.cc
    PolicyEngineTest.cc
    PortalInputBackendTest.cc
    QrCodeTest.cc
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
    nexa_options
    Qt6::Test
    Catch2::Catch2
)

include(Catch)
catch_discover_tests(nexa_tests)
