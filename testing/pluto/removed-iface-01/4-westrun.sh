ipsec auto --up test1
../../pluto/bin/ping-once.sh --up -I 192.1.3.2 192.1.3.1
ipsec whack --trafficstatus
ip addr add 192.1.3.3/24 dev eth3
ipsec auto --ready
ipsec auto --up test2
../../pluto/bin/ping-once.sh --up -I 192.1.3.3 192.1.3.1
ipsec whack --trafficstatus
test "$(cat /var/run/pluto/pluto.pid)" == "$(pidof pluto)"
ip addr del 192.1.3.3/24 dev eth3
