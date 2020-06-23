Name: sailfish-connman-plugin-tethering-wmtwifi
Version: 1.0.0
Release: 1
Summary: wmtWifi plugin for tethering
Group: Development/Libraries
License: GPLv2
URL: https://github.com/mer-hybris/sailfish-connman-plugin-tethering-wmtwifi
Source: %{name}-%{version}.tar.bz2
Requires: connman >= 1.32+git36
BuildRequires: pkgconfig(connman) >= 1.32+git36
BuildRequires: pkgconfig(libgsupplicant) >= 1.0.11
BuildRequires: pkgconfig(libglibutil)

%define plugin_dir %{_libdir}/connman/plugins

%description
This plugin switches wifi driver between AP and STA mode when tethering is being turned on and off.

%prep
%setup -q -n %{name}-%{version}

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} LIBDIR=%{_libdir} install

%files
%defattr(-,root,root,-)
%{plugin_dir}/*.so
