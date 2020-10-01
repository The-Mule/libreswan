grep -i -e 'assert' -e 'abort' -e 'segfault' -e 'unhandled signal' -e 'User process fault' /tmp/pluto.log
journalctl -xe | grep -i -e 'assert' -e 'abort' -e 'segfault' -e 'unhandled signal' -e 'User process fault'
ipsec stop
ip link set dev eth3 down
ip tunnel del eth3
rmmod ip_gre
rmmod ip_tunnel

../../pluto/bin/ipsec-look.sh
: ==== cut ====
ipsec auto --status
: ==== tuc ====
../bin/check-for-core.sh
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi
: ==== end ====
