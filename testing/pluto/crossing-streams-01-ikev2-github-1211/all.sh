#
# set up east
#

east# /testing/guestbin/swan-prep
east# ipsec start

Redirecting to: [initsystem]

east# ../../guestbin/wait-until-pluto-started
east# ipsec whack --impair revival
east# ipsec whack --impair suppress_retransmits
east# ipsec auto --add east-west

"east-west": added IKEv2 connection

#
# set up west
#

west# /testing/guestbin/swan-prep
west# ipsec start

Redirecting to: [initsystem]

west# ../../guestbin/wait-until-pluto-started
east# ipsec whack --impair revival
west# ipsec whack --impair suppress_retransmits
west# ipsec whack --impair block_inbound:yes

IMPAIR: block all inbound messages: yes

west# ipsec auto --add east-west

"east-west": added IKEv2 connection

#
# initiate east
#
# hold east's IKE_SA_INIT request as inbound message 1

east# ipsec up --asynchronous east-west

"east-west" #1: initiating IKEv2 connection to 192.1.2.23 using UDP

west# ../../guestbin/wait-for-inbound.sh 1

packet from 192.1.2.23:500: IMPAIR: blocking inbound message 1

#
# initiate west (create IKE #1)
#
# hold east's IKE_SA_INIT response as inbound message 2

west# ipsec up --asynchronous east-west

"east-west" #1: initiating IKEv2 connection to 192.1.2.23 using UDP

west# ../../guestbin/wait-for-pluto.sh '^".*#1: sent IKE_SA_INIT request'

"east-west" #1: sent IKE_SA_INIT request to 192.1.2.23:UDP/500

west# ../../guestbin/wait-for-inbound.sh 2

packet from 192.1.2.23:500: IMPAIR: blocking inbound message 2

#
# on west, respond to east's IKE_SA_INIT request (message 1) (create IKE #2)
#
# hold east's IKE_AUTH request as inbound message 3

west# ipsec whack --impair drip_inbound:1

IMPAIR: start processing inbound drip packet 1
IMPAIR: stop processing inbound drip packet 1

west# ../../guestbin/wait-for-pluto.sh '^".*#2: sent IKE_SA_INIT'

"east-west" #2: sent IKE_SA_INIT (or IKE_INTERMEDIATE) response {cipher=AES_GCM_16_256 integ=n/a prf=HMAC_SHA2_512 group=MODP2048}

west# ../../guestbin/wait-for-inbound.sh 3

packet from 192.1.2.23:500: IMPAIR: blocking inbound message 3

#
# on west, process east's IKE_SA_INIT response (message 2) (create Child #3)
#
# hold east's IKE_AUTH response as inbound message 4

west# ipsec whack --impair drip_inbound:2

IMPAIR: start processing inbound drip packet 2
IMPAIR: stop processing inbound drip packet 2

west# ../../guestbin/wait-for.sh --match '^".*#1: sent IKE_AUTH request'  -- cat /tmp/pluto.log

"east-west" #1: processed IKE_SA_INIT response from 192.1.2.23:UDP/500 {cipher=AES_GCM_16_256 integ=n/a prf=HMAC_SHA2_512 group=DH19}, initiating IKE_AUTH
"east-west" #1: sent IKE_AUTH request to 192.1.2.23:UDP/500

west# ../../guestbin/wait-for-inbound.sh 4

packet from 192.1.2.23:500: IMPAIR: blocking inbound message 4

#
# on west, process east's IKE_SA_AUTH response (message 4)
#
# it should establish

west# ipsec whack --impair drip_inbound:4

IMPAIR: start processing inbound drip packet 4
IMPAIR: stop processing inbound drip packet 4

west# ../../guestbin/wait-for-pluto.sh 'established Child SA using #1'

"east-west" #3: initiator established Child SA using #1; IPsec tunnel [192.0.1.0/24===192.0.2.0/24] {ESP/ESN=>0xESPESP <0xESPESP xfrm=AES_GCM_16_256-NONE DPD=passive}

#
# on west, process east's IKE_SA_AUTH request (message 3) (create Child #4)
#
# it should establish

west# ipsec whack --impair drip_inbound:3

IMPAIR: start processing inbound drip packet 3
IMPAIR: stop processing inbound drip packet 3

west# ../../guestbin/wait-for.sh --match 'established Child SA using #2'  -- cat /tmp/pluto.log

"east-west" #4: responder established Child SA using #2; IPsec tunnel [192.0.1.0/24===192.0.2.0/24] {ESP/ESN=>0xESPESP <0xESPESP xfrm=AES_GCM_16_256-NONE DPD=passive}

#
# On east, delete the peer's IKE SA
#

west# ipsec whack --impair block_inbound:no

IMPAIR: block all inbound messages: no

east# ipsec showstates
east# ipsec down '#1'
