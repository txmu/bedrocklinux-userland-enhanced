all: kmon

kmon:
	$(CC) $(CFLAGS) -std=c99 -Wall -Wextra kmon.c -o kmon

clean:
	rm -f kmon

install:
	mkdir -p $(prefix)/libexec
	install -m 755 kmon $(prefix)/libexec/brl-kmon

uninstall:
	rm -f $(prefix)/libexec/brl-kmon
