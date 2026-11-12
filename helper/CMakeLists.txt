# nexa-helper: the root-privileged daemon on the system bus. Deliberately small, and linked
# against as little as possible — Qt Core, QtDBus and polkit-qt, no GUI.
find_package(PolkitQt6-1 REQUIRED)

qt_add_executable(nexa_helper
    main.cc
    DeviceLock.h DeviceLock.cc
    HelperService.h HelperService.cc
    InputDevices.h InputDevices.cc
    JobManager.h JobManager.cc
    LoginSession.h
    LoginSessions.h LoginSessions.cc
    PolkitAuthorizer.h PolkitAuthorizer.cc
    SessionGrants.h SessionGrants.cc
    UinputDevice.h UinputDevice.cc
)
set_target_properties(nexa_helper PROPERTIES OUTPUT_NAME nexa-helper)

# The service side of the contract: the adaptor is generated from the same XML as the client proxy,
# so the published interface and HelperService's slots are checked against each other at build time.
qt_add_dbus_adaptor(nexa_helper_adaptor "${NEXA_HELPER_XML}" helper/HelperService.h nexa::HelperService
    helperadaptor OrgNexaHelperAdaptor)
target_sources(nexa_helper PRIVATE ${nexa_helper_adaptor})

# The generated adaptor header lands here.
target_include_directories(nexa_helper PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

install(TARGETS nexa_helper RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/nexa-assistant")

target_link_libraries(nexa_helper PRIVATE
    nexa_proto
    nexa_options
    Qt6::DBus
    PolkitQt6-1::Core
)
