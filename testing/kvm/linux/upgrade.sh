#!/bin/sh

exec </dev/null
set -xe
set -o pipefail

PREFIX=@@PREFIX@@

:
: disable useless repos
:

for repo in fedora-cisco-openh264 ; do
    echo disabling: ${repo}
    dnf config-manager setopt ${repo}.enabled=0
done


:
: enable useful repos
:

for repo in fedora-debuginfo updates-debuginfo ; do
    echo enabling: ${repo}
    dnf config-manager setopt ${repo}.enabled=1
done


:
: Point the cache at /pool/pkg.fedora.NNN
:

cachedir=$( . /etc/os-release ; echo /pool/pkg.${ID}.${VERSION_ID} )
mkdir -p ${cachedir}
dnf config-manager setopt keepcache=1
dnf config-manager setopt cachedir=${cachedir}
dnf config-manager setopt system_cachedir=${cachedir}

:
: give network time to come online!
:

sleep 5


:
: explicitly build the cache
:

dnf makecache


:
: limit kernel to two installs
:

# https://ask.fedoraproject.org/t/old-kernels-removal/7026/2
sudo sed -i 's/installonly_limit=3/installonly_limit=2/' /etc/dnf/dnf.conf


:
: Install then upgrade
:

# stuff needed to build libreswan; this is first installed and then
# constantly upgraded

building() {
    cat <<EOF | awk '{print $1}'
ElectricFence
audit-libs-devel
c++
make
ldns-devel
libcurl-devel
libseccomp-devel
libselinux-devel
mercurial			NSS
gyp				NSS
ninja-build			NSS
nss-devel
nss-tools
nss-util-devel
pam-devel
unbound
unbound-devel
xmlto
EOF
}

# latest kernel; this is only installed (upgrading kernels is not a
# fedora thing).  XL2TPD sucks in the latest kernel so is included in
# the list.

kernel() {
    cat <<EOF | awk '{print $1}'
kernel
kernel-devel
xl2tpd
EOF
}

# utilities used to test libreswan; these are only installed for now
# (so that there isn't too much version drift).

testing() {
    cat <<EOF | awk '{print $1}'
bind-dnssec-utils
bind-utils
conntrack-tools
fping
gawk
gdb
gnutls-utils				used by soft tokens
ike-scan
iptables
libcap-ng-utils
linux-system-roles
nc
net-tools
nftables
nsd
ocspd
openssl
python3-netaddr
python3-pexpect
rsync
selinux-policy-devel
socat
softhsm-devel				used by soft tokens
sshpass					used by ansible-playbook
strace
strongswan
strongswan-sqlite
systemd-networkd
tar
tcpdump
tpm2-abrmd
valgrind
vim-enhanced
wireshark-cli
EOF
}

dnf install -y `building` `testing` `kernel`
dnf upgrade -y `building` `testing`
