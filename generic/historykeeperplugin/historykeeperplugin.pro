CONFIG += release
isEmpty(PSISDK) {
    include(../../psiplugin.pri)
} else {
    include($$PSISDK/plugins/psiplugin.pri)
}
SOURCES += historykeeperplugin.cpp
RESOURCES += resources.qrc
