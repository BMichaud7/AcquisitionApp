%global debug_package %{nil}
Name:       sdr-acquisition
Version:    %{pkg_version}
Release:    1%{?dist}
Summary:    OpenRFStack spectrum scanner and signal acquisition
License:    Proprietary
URL:        https://github.com/OpenRFStack/AcquisitionApp
BuildArch:  x86_64
AutoReqProv: no
Requires:   qpid-proton-cpp tinyxml2 fftw spdlog fmt libpqxx

%description
AcquisitionApp sweeps a configured frequency range using SdrResourceManager,
runs Welch FFT + CA-CFAR detection on each dwell, and publishes confirmed
RF detections to AMQP 1.0 (rf.detections) and PostgreSQL. Supports P25
voice channel following and GPS/AIS/ADS-B/EAS/DSC threat detection.
Multi-band mode auto-splits frequency bands across all available SDR devices.

%prep
%build
%install

%files
/usr/bin/sdr_acquisition

%changelog
* Thu Jan 01 2026 OpenRFStack CI <noreply@github.com> - %{pkg_version}-1
- Automated build from main/1.0
