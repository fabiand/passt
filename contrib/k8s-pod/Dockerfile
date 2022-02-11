FROM fedora:latest
RUN dnf install -y make git gcc
RUN git clone https://passt.top/passt
RUN cd passt && make install
RUN dnf install -y qemu-system-x86
RUN dnf install -y iproute

CMD bash -x /passt/doc/demo.sh
