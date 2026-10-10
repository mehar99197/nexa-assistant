#include <QCoreApplication>

#include <catch2/catch_session.hpp>

// Network tests need an event loop, so the Catch2 session runs inside a QCoreApplication.
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
