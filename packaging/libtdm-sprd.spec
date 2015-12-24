Name:           libtdm-sprd
Version:        1.0.1
Release:        0
License:        MIT
Summary:        Tizen Display Manager Spreadtrum Back-End Library
Group:          Development/Libraries
ExcludeArch:    i586 x86_64
%if ("%{?tizen_target_name}" != "TM1")
ExclusiveArch:
%endif
Source0:        %{name}-%{version}.tar.gz
Source1001:     %{name}.manifest
BuildRequires: pkgconfig(libdrm)
BuildRequires: pkgconfig(libudev)
BuildRequires: pkgconfig(libtdm)
BuildRequires: kernel-headers-tizen-dev
BuildConflicts: linux-glibc-devel

%description
Back-End library of Tizen Display Manager Spreadtrum : libtdm-mgr SPRD library

%prep
%if "%{?tizen_target_name}" != "TM1"
%{error: target %{?tizen_target_name} is not supported!}
exit 1
%endif

%setup -q
cp %{SOURCE1001} .

%build
%reconfigure --prefix=%{_prefix} --libdir=%{_libdir}  --disable-static \
             CFLAGS="${CFLAGS} -Wall -Werror" \
             LDFLAGS="${LDFLAGS} -Wl,--hash-style=both -Wl,--as-needed"
make %{?_smp_mflags}

%install
%make_install

%post
if [ -f %{_libdir}/tdm/libtdm-default.so ]; then
    rm -rf %{_libdir}/tdm/libtdm-default.so
fi
ln -s libtdm-sprd.so %{_libdir}/tdm/libtdm-default.so

%postun -p /sbin/ldconfig

%files
%manifest %{name}.manifest
%license COPYING
%defattr(-,root,root,-)
%{_libdir}/tdm/libtdm-sprd.so
