# AsusWRT
RT-N56U AsusWRT source code

#### Install prerequisite  
- sudo apt-get install -y libncurses5 libncurses5-dev m4 bison gawk flex libstdc++6-4.4-dev g++-4.4 g++ \  
gengetopt git gitk zlib1g-dev autoconf autopoint libtool shtool autogen \  
mtd-utils intltool sharutils docbook-xsl-* libstdc++5 texinfo dos2unix \  
xsltproc uboot-mkimage device-tree-compiler python make pkg-config  

#### If you are running 64-bit Ubuntu, install the following:  
- sudo apt-get install -y libc6-i386 lib32stdc++6 lib32z1

**Note**: This was tested on Ubuntu/Precise64 Vagrant Box  

#### Perform the following:  
- sudo cp -r AsusWRT/asuswrt/tools/brcm/ /opt/.  
- cp AsusWRT/asuswrt/tools/buildroot-gcc342.tar.bz2 && bunzip buildroot-gcc342.tar.bz2 && tar -xvf buildroot-gcc342.tar && sudo mv buildroot-gcc342 /opt/.
- export PATH=$PATH:/opt/brcm/hndtools-mipsel-linux/bin  
- export PATH=$PATH:/opt/brcm/hndtools-mipsel-uclibc/bin  
- export PATH=$PATH:/opt/buildroot-gcc342/bin

#### To build binary for RT-N56U
- cd AsusWRT/asuswrt/release/src-ra  
- make rt-n56u  

**Note**: Once compiled, the binary will be in the ./image directory.
