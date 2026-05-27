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
    src/tcpserver.cpp \
    src/panelsubprocess.cpp

HEADERS += \
    src/corneawidget.h \
    src/corneaconfig.h \
    src/pythonbridge.h \
    src/corneacontroller.h \
    src/imageloader.h \
    src/devicecontrolpanel.h \
    src/tcpserver.h \
    src/panelsubprocess.h

FORMS += \
    src/corneawidget.ui

RESOURCES += \
    resources/resources.qrc

# Copy Python DLL + panel_worker.py to the build output. panel_worker.py
# lives in <exe>/python/ so CorneaWidget's default workerScriptPath resolver
# (QCoreApplication::applicationDirPath() + "/python/panel_worker.py") finds
# it without any operator setup. Same idea for the Phase 1 smoke test.
#
# qmake places release/debug exes in $$OUT_PWD/release or $$OUT_PWD/debug
# (Windows convention with the default win32-msvc spec). Mirror that.
win32 {
    CONFIG(release, debug|release):  EXE_DIR = $$OUT_PWD/release
    CONFIG(debug, debug|release):    EXE_DIR = $$OUT_PWD/debug
    PANEL_WORKER_SRC = $$shell_path($$PWD/python/panel_worker.py)
    SMOKE_TEST_SRC   = $$shell_path($$PWD/python/smoke_test_worker.py)
    PANEL_WORKER_DST = $$shell_path($$EXE_DIR/python)
    EXE_DIR_NATIVE   = $$shell_path($$EXE_DIR)
    QMAKE_POST_LINK += $$quote(cmd /c copy /Y \"$$PYTHON_PATH\\python312.dll\" \"$$EXE_DIR_NATIVE\")
    QMAKE_POST_LINK += $$escape_expand(\\n\\t)$$quote(cmd /c if not exist \"$$PANEL_WORKER_DST\" mkdir \"$$PANEL_WORKER_DST\")
    QMAKE_POST_LINK += $$escape_expand(\\n\\t)$$quote(cmd /c copy /Y \"$$PANEL_WORKER_SRC\" \"$$PANEL_WORKER_DST\")
    QMAKE_POST_LINK += $$escape_expand(\\n\\t)$$quote(cmd /c copy /Y \"$$SMOKE_TEST_SRC\" \"$$PANEL_WORKER_DST\")
}

# Default rules for deployment
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
