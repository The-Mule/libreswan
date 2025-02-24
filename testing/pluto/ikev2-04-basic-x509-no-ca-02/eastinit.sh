/testing/guestbin/swan-prep --nokeys

# delete the CA, both ends hardcode both certificates
/testing/x509/import.sh real/mainca/east.end.p12
/testing/x509/import.sh /testing/x509/real/mainca/west.end.cert
# check result
ipsec certutil -L

ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-ikev2
ipsec auto --status | grep westnet-eastnet-ikev2
echo "initdone"
