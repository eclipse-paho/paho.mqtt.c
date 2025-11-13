Summary:            MQTT C Client
Name:               paho-c
Version:            1.3.2
Release:            3%{?dist}
License:            Eclipse Distribution License 1.0 and Eclipse Public License 2.0
Group:              Development/Tools
Source:             https://github.com/eclipse/paho.mqtt.c/archive/v%{version}.tar.gz
URL:                https://eclipse.org/paho/clients/c/
BuildRequires:      cmake
BuildRequires:      gcc
BuildRequires:      graphviz
BuildRequires:      doxygen
BuildRequires:      openssl-devel
Requires:           openssl


%description
The Paho MQTT C Client is a fully fledged MQTT client written in ANSI standard C.


%package devel
Summary:            MQTT C Client development kit
Group:              Development/Libraries
Requires:           paho-c

%description devel
Development files and samples for the the Paho MQTT C Client.


%package devel-docs
Summary:            MQTT C Client development kit documentation
Group:              Development/Libraries

%description devel-docs
Development documentation files for the the Paho MQTT C Client.

%prep
%autosetup -n paho-c-%{version}

%build
mkdir build.paho && cd build.paho
%cmake -DPAHO_WITH_SSL=TRUE -DPAHO_BUILD_DOCUMENTATION=TRUE -DPAHO_BUILD_SAMPLES=TRUE ..
make %{?_smp_mflags}

%install
cd build.paho
make install DESTDIR=%{buildroot}

%files
%doc edl-v10 epl-v20
%{_libdir}/*

%files devel
%{_bindir}/*
%{_includedir}/*

%files devel-docs
%{_datadir}/*

%changelog
* Thu Jul 27 2017 Otavio R. Piske <opiske@redhat.com> - 1.2.0-4
- Enabled generation of debuginfo package

* Thu Jul 27 2017 Otavio R. Piske <opiske@redhat.com> - 1.2.0-3
- Fixed changelog issues pointed by rpmlint

* Thu Jul 27 2017 Otavio R. Piske <opiske@redhat.com> - 1.2.0-2
- Updated changelog to comply with Fedora packaging guidelines

* Wed Jul 26 2017 Otavio R. Piske <opiske@redhat.com> - 1.2.0-1
- Fixed rpmlint warnings: replaced cmake call with builtin macro
- Fixed rpmlint warnings: removed buildroot reference from build section

* Fri Jun 30 2017 Otavio R. Piske <opiske@redhat.com> - 1.2.0
- Updated package to version 1.2.0

* Sat Dec 31 2016 Otavio R. Piske <opiske@redhat.com> - 1.1.0
- Initial packaging
