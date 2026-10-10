# nexa_remote: the web remote — Nexa's own small web server, so a phone's browser can talk to it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(QRENCODE REQUIRED IMPORTED_TARGET libqrencode)

add_library(nexa_remote STATIC
    IRemoteHost.h
    PairingManager.h PairingManager.cc
    QrCode.h QrCode.cc
    RemoteProtocol.h RemoteProtocol.cc
    RemoteServer.h RemoteServer.cc
    RemoteSession.h RemoteSession.cc
    TlsIdentity.h TlsIdentity.cc
)

# The page, built in: the server hands out these files and nothing from the disk.
qt_add_resources(nexa_remote "nexa_web"
    PREFIX "/nexa/web"
    BASE web
    FILES
        web/index.html
        web/app.js
        web/worklet.js
        web/sw.js
        web/style.css
        web/manifest.json
        web/icon.svg
)

target_link_libraries(nexa_remote
    PUBLIC
        nexa_core
        Qt6::Gui
        Qt6::Network
        Qt6::HttpServer
        Qt6::WebSockets
    PRIVATE
        nexa_options
        PkgConfig::QRENCODE
)
