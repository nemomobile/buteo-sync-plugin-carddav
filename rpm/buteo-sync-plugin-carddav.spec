Name:       buteo-sync-plugin-carddav
Summary:    Syncs calendar data from CardDAV services
Version:    0.0.7
Release:    1
Group:      System/Libraries
License:    LGPLv2.1
URL:        https://github.com/nemomobile/buteo-sync-plugin-carddav
Source0:    %{name}-%{version}.tar.bz2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(Qt5Versit)
BuildRequires:  pkgconfig(mlite5)
BuildRequires:  pkgconfig(buteosyncfw5)
BuildRequires:  pkgconfig(accounts-qt5)
BuildRequires:  pkgconfig(libsignon-qt5)
BuildRequires:  pkgconfig(libsailfishkeyprovider)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions)
Requires: buteo-syncfw-qt5-msyncd

%description
A Buteo plugin which syncs contact data from CardDAV services

%files
%defattr(-,root,root,-)
#out-of-process-plugin
/usr/lib/buteo-plugins-qt5/oopp/carddav-client
#in-process-plugin
#/usr/lib/buteo-plugins-qt5/libcarddav-client.so
%config %{_sysconfdir}/buteo/profiles/client/carddav.xml
%config %{_sysconfdir}/buteo/profiles/sync/carddav.Contacts.xml

%prep
%setup -q -n %{name}-%{version}

%build
%qmake5 "DEFINES+=BUTEO_OUT_OF_PROCESS_SUPPORT"
make %{?jobs:-j%jobs}

%pre
rm -f /home/nemo/.cache/msyncd/sync/client/carddav.xml
rm -f /home/nemo/.cache/msyncd/sync/carddav.Contacts.xml

%install
rm -rf %{buildroot}
%qmake5_install

%post
su nemo -c "systemctl --user restart msyncd.service" || :
