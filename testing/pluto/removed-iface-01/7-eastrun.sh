ipsec auto --status
ipsec auto --ready
ipsec auto --status
sleep 30
test "$(cat /var/run/pluto/pluto.pid)" == "$(pidof pluto)"

