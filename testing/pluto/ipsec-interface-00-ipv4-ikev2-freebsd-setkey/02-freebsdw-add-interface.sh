../../guestbin/prep.sh

ifconfig ipsec1 create reqid 100
ifconfig ipsec1 inet tunnel 192.1.2.45 192.1.2.23
ifconfig ipsec1 inet 192.0.45.1/24 192.0.23.1

ifconfig ipsec1
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh
