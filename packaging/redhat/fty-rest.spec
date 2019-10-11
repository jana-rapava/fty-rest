#
#    fty-rest - Common core REST API for 42ity project
#    NOTE: This file was customized after generation, be sure to keep it
#
#    Copyright (C) 2014 - 2018 Eaton
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License along
#    with this program; if not, write to the Free Software Foundation, Inc.,
#    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

# To build with draft APIs, use "--with drafts" in rpmbuild for local builds or add
#   Macros:
#   %_with_drafts 1
# at the BOTTOM of the OBS prjconf
%bcond_with drafts
%if %{with drafts}
%define DRAFTS yes
%else
%define DRAFTS no
%endif
Name:           fty-rest
Version:        1.0.0
Release:        1
Summary:        common core rest api for 42ity project
License:        GPL-2.0+
URL:            https://42ity.org
Source0:        %{name}-%{version}.tar.gz
Group:          System/Libraries
# Note: ghostscript is required by graphviz which is required by
#       asciidoc. On Fedora 24 the ghostscript dependencies cannot
#       be resolved automatically. Thus add working dependency here!
BuildRequires:  ghostscript
BuildRequires:  asciidoc
BuildRequires:  automake
BuildRequires:  autoconf
BuildRequires:  libtool
BuildRequires:  pkgconfig
# Note: customized dependency added - systemd
BuildRequires:  systemd-devel
BuildRequires:  systemd
%{?systemd_requires}
BuildRequires:  xmlto
# Note that with current implementation of zproject use-cxx-gcc-4-9 option,
# this effectively hardcodes the use of specifically 4.9, not allowing for
# "4.9 or newer".
BuildRequires:  devtoolset-3-gcc devtoolset-3-gcc-c++
BuildRequires:  gcc-c++ >= 4.9.0
BuildRequires:  libsodium-devel
BuildRequires:  zeromq-devel
BuildRequires:  czmq-devel >= 3.0.2
BuildRequires:  libcidr-devel
BuildRequires:  cxxtools-devel
BuildRequires:  tntnet-devel
BuildRequires:  tntdb-devel
BuildRequires:  file-devel
BuildRequires:  log4cplus-devel
BuildRequires:  fty-common-logging-devel
BuildRequires:  fty-common-devel
BuildRequires:  fty-common-db-devel
BuildRequires:  fty-common-rest-devel
BuildRequires:  malamute-devel >= 1.0.0
BuildRequires:  openssl-devel
BuildRequires:  fty-common-mlm-devel
BuildRequires:  cyrus-sasl-devel
BuildRequires:  fty-proto-devel >= 1.0.0
BuildRequires:  fty_shm-devel >= 1.0.0
BuildRequires:  fty-asset-activator-devel
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
fty-rest common core rest api for 42ity project.

%package -n libfty_rest1
Group:          System/Libraries
Summary:        common core rest api for 42ity project shared library

%description -n libfty_rest1
This package contains shared library for fty-rest: common core rest api for 42ity project

%post -n libfty_rest1 -p /sbin/ldconfig
%postun -n libfty_rest1 -p /sbin/ldconfig

%files -n libfty_rest1
%defattr(-,root,root)
%doc COPYING
#%{_libdir}/libfty_rest.so.*
%{_libdir}/%{name}/libfty_rest.so.*
%{_libdir}/%{name}/libfty_rest.so

%package devel
Summary:        common core rest api for 42ity project
Group:          System/Libraries
Requires:       libfty_rest1 = %{version}
Requires:       libsodium-devel
Requires:       zeromq-devel
Requires:       czmq-devel >= 3.0.2
Requires:       libcidr-devel
Requires:       cxxtools-devel
Requires:       tntnet-devel
Requires:       tntdb-devel
Requires:       file-devel
Requires:       log4cplus-devel
Requires:       fty-common-logging-devel
Requires:       fty-common-devel
Requires:       fty-common-db-devel
Requires:       fty-common-rest-devel
Requires:       malamute-devel >= 1.0.0
Requires:       openssl-devel
Requires:       fty-common-mlm-devel
Requires:       cyrus-sasl-devel
Requires:       fty-proto-devel >= 1.0.0
Requires:       fty_shm-devel >= 1.0.0
Requires:       fty-asset-activator-devel

%description devel
common core rest api for 42ity project development tools
This package contains development files for fty-rest: common core rest api for 42ity project

# Note: pathnames below were customized to match the Makefile with legacy paths
# and the .so file is delivered as part of main package for tntnet to find it
%files devel
%defattr(-,root,root)
%{_includedir}/*
#%{_libdir}/libfty_rest.so
###%{_libdir}/%{name}/libfty_rest.so
%{_libdir}/pkgconfig/libfty_rest.pc
%{_mandir}/man3/*
%{_mandir}/man7/*

%prep

%setup -q

%build
sh autogen.sh
%{configure} --enable-drafts=%{DRAFTS} --with-libmagic=yes
make %{_smp_mflags}

%install
make install DESTDIR=%{buildroot} %{?_smp_mflags}

# remove static libraries
find %{buildroot} -name '*.a' | xargs rm -f
find %{buildroot} -name '*.la' | xargs rm -f

# Note: pathnames below were customized to match the Makefile with legacy paths
%files
%defattr(-,root,root)
%doc README.md
%doc COPYING
#%{_bindir}/db/bios-csv
###%{_libexecdir}/%{name}/bios-csv
%{_prefix}/libexec/fty-rest/bios-csv
#%{_mandir}/man1/db/bios-csv*
%{_mandir}/man1/bios-csv*
%{_prefix}/libexec/%{name}/bios-passwd
%{_prefix}/libexec/%{name}/testpass.sh
#%{_datadir}/%{name}/.git_details-fty-rest
%{_datadir}/bios/.git_details-fty-rest
%{_datadir}/%{name}/examples/tntnet.xml.example
%if 0%{?suse_version} > 0
# The validator is pickier in OpenSUSE build targets
%dir %{_libdir}
%dir %{_libdir}/%{name}
%dir %{_libexecdir}
%dir %{_prefix}/libexec
%dir %{_prefix}/libexec/%{name}
%dir %{_datadir}
%dir %{_datadir}/bios
%dir %{_datadir}/%{name}
%dir %{_datadir}/%{name}/examples
# Symlinks on some distro layouts
%dir %{_prefix}/lib/%{name}
%{_prefix}/lib/%{name}/bios-passwd
%{_prefix}/lib/%{name}/testpass.sh
%endif

%changelog
