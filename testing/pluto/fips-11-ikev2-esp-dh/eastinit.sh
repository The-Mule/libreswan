/testing/guestbin/swan-prep --nokeys
../../guestbin/ip.sh address add 192.0.200.254/24 dev eth0:1
../../guestbin/ip.sh route add 192.0.100.0/24 via 192.1.2.45  dev eth1
ipsec start
../../guestbin/wait-until-pluto-started
ipsec add ikev2-esp=aes-sha1-modp4096
ipsec add ikev2-base
echo "initdone"
