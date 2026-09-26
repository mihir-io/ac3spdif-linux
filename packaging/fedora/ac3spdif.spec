# Built by ./scripts/package.sh rpm in a clean Fedora container. The shared
# library dependencies (libavcodec and friends, libpipewire, alsa-lib, GLib,
# GTK, libappindicator) are generated from the binaries, so `dnf install` of
# the resulting RPM pulls them in. Fedora's ffmpeg-free carries the AC-3 and
# DTS encoders and the IEC 61937 muxer; RPM Fusion is not needed.

%global appid org.ac3spdif.App

Name:           ac3spdif
Version:        0.1.0
Release:        1%{?dist}
Summary:        Dolby Digital Live and DTS Connect for Linux
License:        MIT
URL:            https://github.com/mihir-io/ac3spdif-linux
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.20
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(alsa)
BuildRequires:  pkgconfig(libavcodec)
BuildRequires:  pkgconfig(libavformat)
BuildRequires:  pkgconfig(libavutil)
BuildRequires:  pkgconfig(libswresample)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(appindicator3-0.1)
BuildRequires:  desktop-file-utils
BuildRequires:  appstream

# The sound server the virtual sink and the passthrough transport live in.
# The direct ALSA transport works without it.
Recommends:     pipewire
Recommends:     wireplumber
# GNOME shows the tray icon only with this extension; every other desktop
# shows it natively.
Suggests:       gnome-shell-extension-appindicator

%description
Captures all system audio, encodes it to AC-3 (Dolby Digital) or DTS in real
time, and bitstreams the result out an S/PDIF or HDMI interface as an IEC 61937
data burst, so a receiver decodes 5.1 itself instead of being handed stereo
PCM. What sound cards on other platforms call Dolby Digital Live or DTS Connect.

A virtual PipeWire sink becomes the system output; whatever plays into it is
encoded in-process with ffmpeg's libraries and sent as a native passthrough
stream to the digital output, or straight to the ALSA device for the lowest
latency. Includes the ac3spdif-app tray application, the ac3spdif command line,
which can run as a systemd user service from login, and ac3spdif-probe.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -GNinja -DBUILD_APP=ON -DBUILD_TESTS=ON
%cmake_build

%install
%cmake_install

%check
%ctest
desktop-file-validate %{buildroot}%{_datadir}/applications/%{appid}.desktop
appstreamcli validate --no-net %{buildroot}%{_metainfodir}/%{appid}.metainfo.xml

%files
%license LICENSE
%doc README.md
%{_bindir}/ac3spdif
%{_bindir}/ac3spdif-app
%{_bindir}/ac3spdif-probe
%{_datadir}/ac3spdif/
%{_datadir}/applications/%{appid}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{appid}.svg
%{_metainfodir}/%{appid}.metainfo.xml

%changelog
* Sat Sep 26 2026 Mihir Joshi <mihir3535@gmail.com> - 0.1.0-1
- First release
