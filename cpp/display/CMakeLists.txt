# The Qt touchscreen panel: the QML and the C++ that feeds it, together. This
# component consumes the local HTTP API; it does not link to or alter the
# receiver/recorder runtime, so it links no rapid_* library.
qt_standard_project_setup()

qt_add_executable(rapid-qt-display
  qt_dashboard_main.cpp
  qt_dashboard_model.cpp
  qt_dashboard_model.hpp
)
# BASE keeps the resource aliases the ones the code already asks for -
# qrc:/display/Main.qml (qt_dashboard_main.cpp) and
# qrc:/assets/steering-wheel-cartoon.png (Main.qml) - now that this
# CMakeLists.txt sits one directory deeper than the paths they spell.
qt_add_resources(rapid-qt-display rapid_qt_display_assets
  PREFIX "/"
  BASE "${CMAKE_SOURCE_DIR}"
  FILES
    "${CMAKE_SOURCE_DIR}/display/Main.qml"
    "${CMAKE_SOURCE_DIR}/display/HistoryPlot.qml"
    "${CMAKE_SOURCE_DIR}/assets/steering-wheel-cartoon.png"
)
target_link_libraries(rapid-qt-display PRIVATE
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::WebSockets)
target_compile_options(rapid-qt-display PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  qt_add_executable(rapid-qt-tests tests/qt_dashboard_tests.cpp
    qt_dashboard_model.cpp qt_dashboard_model.hpp)
  target_link_libraries(rapid-qt-tests PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)
  target_compile_options(rapid-qt-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-qt-tests COMMAND rapid-qt-tests)
endif()
