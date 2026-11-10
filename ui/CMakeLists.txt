# nexa_ui: the look — the Nexa core (the state light that is also the brand), the theme's tokens and
# stylesheet, and painted icons. Shared by the desktop (app/) and the phone app (android/), so it
# depends on Qt Widgets alone: nothing of core/ or system/.
qt_add_library(nexa_ui STATIC
    ElidedLabel.h ElidedLabel.cc
    NexaCorePainter.h NexaCorePainter.cc
    NexaCoreSpec.h NexaCoreSpec.cc
    NexaCoreWidget.h NexaCoreWidget.cc
    NexaDesigns.h NexaDesigns.cc
    NexaIcons.h NexaIcons.cc
    NexaTheme.h NexaTheme.cc
    NexaThemes.h NexaThemes.cc
    Sparkline.h Sparkline.cc
)

# The stylesheet and its two pictures, built in; NexaTheme fills the tokens in at run time.
qt_add_resources(nexa_ui "nexa_theme"
    PREFIX "/nexa/ui"
    FILES theme.qss chevron-down.svg check.svg
)

target_include_directories(nexa_ui PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(nexa_ui PUBLIC Qt6::Widgets PRIVATE nexa_options)
