sleep 5 # work-around broken ping
../../guestbin/ping-once.sh --up -I 192.0.23.1 192.0.45.1

../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh
