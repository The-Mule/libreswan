ifconfig ipsec1 create reqid 100
ifconfig ipsec1 inet tunnel 192.1.2.23 192.1.2.45
ifconfig ipsec1 inet 192.0.2.254/24 192.0.1.251

echo 'add 192.1.2.45 192.1.2.23 esp 4523 -m tunnel -u 100 -E rijndael-cbc "45--SecureKey-23";' | setkey -c
echo 'add 192.1.2.23 192.1.2.45 esp 2345 -m tunnel -u 100 -E rijndael-cbc "23--SecureKey-45";' | setkey -c
