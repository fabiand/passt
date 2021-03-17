CFLAGS += -Wall -Wextra -pedantic

all: passt qrap

passt: passt.c passt.h arp.c arp.h dhcp.c dhcp.h ndp.c ndp.h siphash.c siphash.h tap.c tap.h icmp.c icmp.h tcp.c tcp.h udp.c udp.h util.c util.h
	$(CC) $(CFLAGS) passt.c arp.c dhcp.c ndp.c siphash.c tap.c icmp.c tcp.c udp.c util.c -o passt

qrap: qrap.c passt.h
	$(CC) $(CFLAGS) qrap.c -o qrap

.PHONY: clean
clean:
	-${RM} passt *.o qrap
