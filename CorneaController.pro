QT += core gui widgets network concurrent

CONFIG += c++17

# Disable the QT_DEBUG SIM path inside pythonbridge.cpp so Debug builds exercise
# real Python + FTDI even with hardware attached. Comment out this line if you
# want a SIM build (e.g. CI on a machine without driver boards).
DEFINES += DISABLE_SIM


# Emit .pdb in Release builds so Windows Error Reporting / Visual Studio can
# resolve crash offsets to function + line number. `force_debug_info` adds
# debug info without disabling optimizations. Without this, the exe ships
# optimized but with no symbols, and WER crash reports give only raw offsets.
CONFIG += force_debug_info
QMAKE_CXXFLAGS_RELEASE += /Zi
QMAKE_LFLAGS_RELEASE   += /DEBUG /OPT:REF /OPT:ICF

TARGET = CorneaController
TEMPLATE = app

# Python configuration
PYTHON_PATH = C:/Python312
VENV_PATH = "C:/google_env/station_venv"

INCLUDEPATH += $$PYTHON_PATH/include
INCLUDEPATH += $$VENV_PATH/Lib/site-packages/numpy/_core/include

LIBS += -L$$PYTHON_PATH/libs -lpython312

# Source files
SOURCES += \
    src/main.cpp \
    src/corneawidget.cpp \
    src/corneaconfig.cpp \
    src/pythonbridge.cpp \
    src/corneacontroller.cpp \
    src/imageloader.cpp \
    src/devicecontrolpanel.cpp \
    src/tcpserver.cpp

HEADERS += \
    src/corneawidget.h \
    src/corneaconfig.h \
    src/pythonbridge.h \
    src/corneacontroller.h \
    src/imageloader.h \
    src/devicecontrolpanel.h \
    src/tcpserver.h

FORMS += \
    src/corneawidget.ui

RESOURCES += \
    resources/resources.qrc

# Copy Python DLL
win32 {
    QMAKE_POST_LINK += $$quote(cmd /c copy /Y \"$$PYTHON_PATH\\python312.dll\" \"$$OUT_PWD\")
}

# Default rules for deployment
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
