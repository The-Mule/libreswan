#!/bin/sh

if test "$#" -le 1 ; then
   echo "usage: $0 <gateway> <export-dir> ..."
   exit 1
fi

set -e

# check NFS is installed and start it (no need to enable it).

RUN()
{
    echo "$@"
    shift
    "$@"
}

if test -f /lib/systemd/system/nfs-server.service ; then
    RUN "starting NFS:" sudo systemctl start nfs-server
#elif some other os ...
else
    echo "is NFS installed?" 1>&2
    exit 1
fi

# gateway

gateway=$1 ; shift

# export the testing directory

for d in "$@" ; do
    # this exports both $(srcdir) and $(srcdir)/testing; oops
    if sudo exportfs -s | grep "^${d} " ; then
	echo ${d} already exported
    else
	#sudo exportfs -r
	RUN "exporting ${d} ${gateway}:" sudo exportfs -o rw,all_squash,anonuid=$(id -u),anongid=$(id -g) ${gateway}:${d}
    fi
done

# poke a hole in the firewall; see systemctl; EXIT CODE 0 indicates it is running
if test -f /lib/systemd/system/firewalld.service && systemctl status firewalld > /dev/null ; then
    echo "configuring firewall ..."
    # add the zone swandefault; replace old
    #
    # problem is that libvirt screws around with the firewall zones so
    # add these to the libvirt zone
    #
    # sudo firewall-cmd --permanent --delete-zone=swandefault || true
    # sudo firewall-cmd --permanent --new-zone=swandefault
    # sudo firewall-cmd --permanent --zone=swandefault --add-interface=swandefault
    # sudo firewall-cmd --permanent --zone=swandefault --add-service=nfs
    # sudo firewall-cmd --permanent --zone=swandefault --add-service=mountd
    # sudo firewall-cmd --permanent --zone=swandefault --add-service=rpc-bind
    RUN "allowing NFS:"        sudo firewall-cmd --permanent --zone=libvirt --add-service=nfs
    RUN "allowing MOUNTD:"     sudo firewall-cmd --permanent --zone=libvirt --add-service=mountd
    RUN "allowing RPC-BIND:"   sudo firewall-cmd --permanent --zone=libvirt --add-service=rpc-bind
    RUN "reloading:"           sudo firewall-cmd --reload
else
    echo "assuming firewall is disabled"
fi
