#-------------------------------------------------
#
# Project created by QtCreator 2011-05-03T18:35:56
#
#-------------------------------------------------

QT       -= gui core
#QT += core

TARGET = mo_shared
TEMPLATE = lib
CONFIG += staticlib

INCLUDEPATH += ../bsatk "$(BOOSTPATH)"

CONFIG(debug, debug|release) {
		LIBS += -L$$OUT_PWD/../bsatk/debug
	LIBS += -lDbgHelp
} else {
		LIBS += -L$$OUT_PWD/../bsatk/release
}

LIBS += -lbsatk

DEFINES += UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS

DEFINES += BOOST_DISABLE_ASSERTS NDEBUG

# QMAKE_CXXFLAGS += /analyze

SOURCES += \
    inject.cpp \
    windows_error.cpp \
    error_report.cpp \
    directoryentry.cpp \
    gameinfo.cpp \
    oblivioninfo.cpp \
    fallout3info.cpp \
    falloutnvinfo.cpp \
    util.cpp \
    skyriminfo.cpp \
    appconfig.cpp \
    leaktrace.cpp

HEADERS += \
    inject.h \
    windows_error.h \
    error_report.h \
    directoryentry.h \
    gameinfo.h \
    oblivioninfo.h \
    fallout3info.h \
    falloutnvinfo.h \
    util.h \
    skyriminfo.h \
    appconfig.h \
    appconfig.inc \
    leaktrace.h