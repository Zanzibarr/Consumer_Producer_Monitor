PROGRAMS = server \
		   client
		   
LDLIBS = -lpthread

all: $(PROGRAMS)

clean:
	rm $(PROGRAMS)