/testing/guestbin/swan-prep --hostkeys
ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-null
ipsec auto --status | grep westnet-eastnet-null
echo "initdone"
