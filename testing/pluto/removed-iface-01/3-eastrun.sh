ipsec auto --ready
ipsec auto --status
test "$(cat /var/run/pluto/pluto.pid)" == "$(pidof pluto)"
