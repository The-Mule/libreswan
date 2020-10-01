sleep 30
test "$(cat /var/run/pluto/pluto.pid)" == "$(pidof pluto)"

