find_package(Catch2 3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(nexa_tests
    TestMain.cc
    FakeHttpServer.h
    QtStringMakers.h
    AgentTest.cc
    ConfigStoreTest.cc
    ConversationTest.cc
    DesktopEntryTest.cc
    FileAccessTest.cc
    IntentRouterTest.cc
    KeyMapTest.cc
    LocalCommandRunnerTest.cc
    OpenAiProviderTest.cc
    OpenAiStreamTest.cc
    PolicyEngineTest.cc
    SseParserTest.cc
    SystemPureTest.cc
    ToolCallAccumulatorTest.cc
    VoicePipelineTest.cc
    VoicePureTest.cc
    VoiceTestAudio.h
)

target_link_libraries(nexa_tests PRIVATE
    nexa_core
    nexa_proto
    nexa_system
    nexa_voice
    nexa_options
    Qt6::Test
    Catch2::Catch2
)

include(Catch)
catch_discover_tests(nexa_tests)
