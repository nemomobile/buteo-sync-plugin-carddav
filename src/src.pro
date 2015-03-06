TARGET    = carddav-client

QT       -= gui
QT       += network dbus

CONFIG += link_pkgconfig console
PKGCONFIG += buteosyncfw5 libsignon-qt5 accounts-qt5 libsailfishkeyprovider
PKGCONFIG += Qt5Versit Qt5Contacts qtcontacts-sqlite-qt5-extensions contactcache-qt5
QT += contacts-private

QMAKE_CXXFLAGS = -Wall \
    -g \
    -Wno-cast-align \
    -O2 -finline-functions

SOURCES += \
    carddavclient.cpp \
    syncer.cpp \
    auth.cpp \
    carddav.cpp \
    requestgenerator.cpp \
    replyparser.cpp

HEADERS += \
    carddavclient.h \
    syncer_p.h \
    auth_p.h \
    carddav_p.h \
    requestgenerator_p.h \
    replyparser_p.h

OTHER_FILES += \
    carddav.xml \
    carddav.Contacts.xml

!contains (DEFINES, BUTEO_OUT_OF_PROCESS_SUPPORT) {
    TEMPLATE = lib
    CONFIG += plugin
    target.path = /usr/lib/buteo-plugins-qt5
}

contains (DEFINES, BUTEO_OUT_OF_PROCESS_SUPPORT) {
    TEMPLATE = app
    target.path = /usr/lib/buteo-plugins-qt5/oopp
    DEFINES += CLIENT_PLUGIN
    DEFINES += "CLASSNAME=CardDavClient"
    DEFINES += CLASSNAME_H=\\\"carddavclient.h\\\"
    INCLUDE_DIR = $$system(pkg-config --cflags buteosyncfw5|cut -f2 -d'I')

    HEADERS += $$INCLUDE_DIR/ButeoPluginIfaceAdaptor.h   \
               $$INCLUDE_DIR/PluginCbImpl.h              \
               $$INCLUDE_DIR/PluginServiceObj.h

    SOURCES += $$INCLUDE_DIR/ButeoPluginIfaceAdaptor.cpp \
               $$INCLUDE_DIR/PluginCbImpl.cpp            \
               $$INCLUDE_DIR/PluginServiceObj.cpp        \
               $$INCLUDE_DIR/plugin_main.cpp
}

sync.path = /etc/buteo/profiles/sync
sync.files = carddav.Contacts.xml

client.path = /etc/buteo/profiles/client
client.files = carddav.xml

INSTALLS += target sync client
