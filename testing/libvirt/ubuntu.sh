#!/bin/bash

TESTING=`readlink -f $0  | sed "s/libvirt.*$/libvirt/"`
pushd $TESTING

echo "creating disks"

# Note: Replace this with your local Fedora tree if you have one.
#export tree=http://mirror.fedoraproject.org/linux/releases/17/Fedora/x86_64/os/
#export tree=http://76.10.157.69/linux/releases/17/Fedora/x86_64/os
#export tree=http://76.10.157.69/ubuntu/dists/precise/main/installer-amd64/
export tree=http://ftp.ubuntu.com/ubuntu/dists/precise/main/installer-amd64/
export BASE=/var/lib/libvirt/images/

if [ ! -f $BASE/swanubuntubase.img ]
then
	echo "Creating swanubuntubase image using libvirt"

# check for hardware VM instructions
cpu="--hvm"
grep vmx /proc/cpuinfo > /dev/null || cpu=""

# Looks like newer virt-install requires the disk image to exist?? How odd
echo -n "creating 8 gig disk image...."
sudo dd if=/dev/zero of=$BASE/swanubuntubase.img bs=1024k count=8192
echo done

# install base guest to obtain a file image that will be used as uml root
sudo virt-install --connect=qemu:///system \
    --network=network:default,model=virtio \
    --initrd-inject=./ubuntubase.ks \
    --extra-args="swanname=swanubuntubase ks=file:/ubuntubase.ks \
      console=tty0 console=ttyS0,115200" \
    --name=swanubuntubase \
    --disk $BASE/swanubuntubase.img,size=8 \
    --ram 1024 \
    --vcpus=1 \
    --check-cpu \
    --accelerate \
    --location=$tree  \
    --autostart  \
    --noreboot \
    --nographics \
    $cpu

fi

# create many copies of this image using copy-on-write
sudo qemu-img convert -O qcow2 $BASE/swanubuntubase.img $BASE/swanubuntubase.qcow2
sudo chown qemu.qemu $BASE/swanubuntubase.qcow2

for hostname in east west north road;
do
	sudo qemu-img create -F qcow2 -f qcow2 -b $BASE/swanubuntubase.qcow2 $BASE/$hostname.qcow2
	sudo chown qemu.qemu $BASE/$hostname.qcow2
	if [ -f /usr/sbin/restorecon ] 
	then
		sudo restorecon $BASE/$hostname.qcow2
	fi
done

sudo virsh undefine swanubuntubase

popd

