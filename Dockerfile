FROM centos:6.7

RUN cd /tmp && \
		echo "multilib_policy=all" >> /etc/yum.conf && \
		yum update -y && \
    yum groupinstall -y "Development tools" && \
    yum -y install git freeglut-devel cmake glibc glibc-devel libX11 libX11-devel mesa-libGL mesa-libGL-devel wget nasm && \
    wget http://ftp.gnu.org/gnu/m4/m4-1.4.18.tar.gz && tar -xvzf m4-1.4.18.tar.gz && cd m4-1.4.18 && ./configure --prefix=/usr/local && make && make install && cd .. && \
    wget http://www.nic.funet.fi/pub/gnu/ftp.gnu.org/pub/gnu/libtool/libtool-2.4.6.tar.gz && tar -xvzf libtool-2.4.6.tar.gz && cd libtool-2.4.6 && ./configure --prefix=/usr/local && make && make install && cd .. && \
    wget http://ftp.gnu.org/gnu/autoconf/autoconf-2.69.tar.gz && tar -xvzf autoconf-2.69.tar.gz && cd autoconf-2.69 && ./configure --prefix=/usr/local && make && make install && cd .. && \
    wget http://ftp.gnu.org/gnu/automake/automake-1.15.tar.gz && tar -xvzf automake-1.15.tar.gz && cd automake-1.15 && ./configure --prefix=/usr/local && make && make install && cd .. && \
    rm -Rf m4-1.4.18* libtool-2.4.6* autoconf-2.69* automake-1.15*

VOLUME /code
WORKDIR /code
