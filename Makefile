LDFLAGS=$(shell pkg-config fuse --libs)
CLFALGS=$(shell pkg-config fuse --cflags) 
all: 
	gcc fusexmp.c -o fusexmp ${CLFALGS} -g ${LDFLAGS}
old:
	gcc oldfusexmp.c -o oldfusexmp ${CLFALGS} -g ${LDFLAGS}

clean:
	rm fusexmp oldfusexmp
