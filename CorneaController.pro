QT += core gui widgets network concurrent

CONFIG += c++17

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
