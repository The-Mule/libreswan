/testing/guestbin/swan-prep --x509
ipsec certutil -D -n west
# add second identity/cert
ipsec pk12util -W foobar -K '' -i /testing/x509/pkcs12/otherca/othereast.p12
# check
ipsec certutil -L

ipsec start
../../guestbin/wait-until-pluto-started
../../guestbin/ipsec-add.sh main other
echo "initdone"
